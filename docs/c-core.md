# The C core

> **Status:** Implemented. **Checked:** 20 November 2026, `215337ef`.
>
> **Remaining:** nothing. The input paths ported line by line in step 9 of §4 were checked by hand on real hardware (AMD, NVIDIA and Intel) by 11 October 2026.

The compositor has been rewritten in plain C, one subsystem at a time, with
the behavior that users and tests can observe left exactly as it is. This
document records where it started, the rules the C code follows, the order
of the steps and what was found along the way.

The shell, Settings, Files and the lock screen stay in Qt/QML. They talk to
the compositor only through Wayland protocols and the command socket, so the
core never depends on Qt.

## 1. The C++ compositor before the rewrite (October 2026)

About 18,000 lines in `compositor/src`. wlroots 0.20 provides backends,
protocols and buffers; scene, renderer, frame timing and window policy are
Vela's own.

```text
main.cpp ──► Server (server.hpp: 1,000 lines of declarations, the god object)
              │  owns: wl_display, backend, Vulkan device, renderer, allocator,
              │        scene, layers, seat, cursor, every list (outputs,
              │        toplevels, layers, keyboards), every policy's state
              │        (workspaces, a11y, input, lock, idle, snap, switcher,
              │        keyboard grab, supervised shell, command socket)
              │
              ├─ Output ────── FrameClock (frame_clock.hpp, pure, tested)
              │    │            virtual vblank, late latching, VRR, power
              │    └─ scene::OutputFrame (frame.cpp): flatten, occlusion,
              │         damage, direct scanout, dmabuf feedback, night light
              ├─ Toplevel ──── Popup, Decoration (text.cpp, icons.cpp),
              │                 WindowCapture, Snapshot animations, snap.cpp,
              │                 workspaces.cpp, xwayland.cpp
              ├─ LayerSurface  (layer.cpp)
              ├─ Keyboard ──── keyboard.cpp, accessibility.cpp (sticky keys)
              ├─ input.cpp, pointer.cpp (pointer, gestures, constraints)
              ├─ lock.cpp (ext-session-lock, idle)
              └─ outputconfig.cpp, nested.cpp, settings.cpp

scene/   Scene, Node/Tree/SurfaceNode/RectNode/BufferNode (virtual classes),
         SurfaceState (enter/leave, preferred scale, frame pacing),
         effects.cpp (ext-background-effect), readiness.cpp (commits held
         until their fences signal), capture.cpp (window capture)
render/  VulkanDevice, Renderer (also a wlr_renderer), Texture, Pass (also
         a wlr_render_pass), GBM allocator, pixel buffers, GLSL shaders
supervisor.cpp  a separate process holding the Wayland socket; restarts the
         compositor after a crash, locked if it was locked
```

Ownership and lifecycle at the time:

- Most objects are created with `new` from a wlroots signal and destroy
  themselves (`delete this`) on the matching destroy signal; `Listener`
  (RAII around `wl_listener`) disconnects automatically.
- Everything reaches everything through `Server&`. Policy code in one file
  (snap, workspaces, accessibility) is written as `Server::` methods and
  touches the private state of others.
- Globals: `Scene::s_instance` (used by every node constructor to request
  frames), `ReadyCommits` counters, `TextRenderer`/`IconLoader` singletons.
- The scene graph uses virtual destructors and `std::vector` children; damage
  is found by diffing the flattened scene with the previous frame
  (`std::unordered_map` keyed by surface/node address).

Contracts that tests define:

| Area | Tests |
|---|---|
| Frame clock, vblank grid, late latching, margin, learned latency | `compositor/tests/frame_clock_test.cpp` |
| Animation curves | `motion_test.cpp` |
| Default scale, `VELA_SCALE`, exact-pixel placement, 1:1 copies | `geometry_test.cpp` |
| Night light, color filters, sunrise/sunset | `color_sun_test.cpp` |
| `vela.conf` reading, writing, legacy Italian names | `settings_test.cpp` |
| Windows, snap, maximize, minimize, Alt+Tab, desktops, system prompts | `tests/functional/test_windows.py` (at 100% and 125%) |
| Hotplug, per-output scale, rescue of windows | `test_outputs.py` |
| Lock, screens off/on, supervisor restart (also locked), giving up | `test_session.py` |
| Late app GPU never stalls the screen (explicit and implicit sync) | `test_ready.py` |
| Bit-exact sharpness at fractional scales | `scripts/test-sharpness.sh` |

Baseline on the machine where the migration started (Intel ADL GT2,
Mesa ANV, no numpy): all unit tests pass; the functional suite has three
failures that exist before any C code: `test_ready` explicit and implicit
(the slow app shares the only GPU engine, ~42 missed vblanks) and
`test_screens_off_and_on_while_locked` (the C++ renderer crashes in a
deferred destruction inside the Intel driver after the output is powered
back on). The sharpness test is skipped without numpy and Pillow. These are
recorded so they are not mistaken for regressions; the crash is to be fixed
by the C renderer. It is not: the C renderer crashes at the same driver
offset, so the cause lies outside the renderer's structure. It is a
use-after-free that shows up as `vkFreeMemory` of the 24×24 cursor texture's
memory crashing in ANV the frame after an output is powered back on while
the screen is locked; validation layers are silent, and ASan or glibc's
malloc checks make it disappear (they change heap reuse), so the culprit is
uninstrumented code (wlroots or Mesa).

## 2. Principles of the C core

- Plain C17, no typedefs for structs, `struct vela_<thing>` and
  `vela_<module>_<verb>()` for what a module exports; everything else is
  `static`. snake_case, like wlroots, which the core calls everywhere.
- One module, one header with the minimal public API; the struct is public
  only when other modules really need its fields.
- Ownership is written down: every struct says who creates it, who destroys
  it and when. Objects tied to a wlroots object die with its destroy signal,
  in one function that removes every listener explicitly.
- No global state except the process-wide pieces that are truly unique (the
  supervisor's and the server's signal flags); the server is passed
  explicitly.
- Allocations are visible: fixed arrays where the size is known, the
  `common/alloc` helpers (`VELA_NEW`, `vela_grow`, `vela_strdup`...) where it
  is not, freed in the matching destroy function; what happens when memory
  runs out is in the [host allocation policy](memory-allocation-policy.md).
  No hidden allocation in hot paths (the frame loop reuses its arrays).
- No macros where a function works (the allocation helpers are macros only
  to get the type's size right); no callbacks where a direct call works.
  Lists are `wl_list`, arrays are plain arrays with a count and a capacity.
- Comments, logs and messages are in English.
- The GoogleTest suites remain the specification: they include the C headers
  inside `extern "C"` and keep their names and assertions.

## 3. Target layout

The layout the migration aimed for. Modules added since (brightness,
Bluetooth, capture watching, output arrangement and preview, security
policy...) are in the development guide's
[project layout](development.md#project-layout).

```text
compositor/src/
  main.c          argument parsing, logging, server lifetime
  supervisor.c/h  socket holder and crash recovery
  server.c/h      struct vela_server: display, backend, globals, lists;
                  startup, the main loop, shutdown, animations
  command.c/h     the command socket and the JSON state
  session.c/h     session environment (systemd, D-Bus) and hooks
  output_manager.c/h  wlr-output-management (apply and test)
  util.c/h        environment flags, time, small string helpers
  listen.h        vela_listen()/vela_unlisten() for wlroots signals
  config.c/h      vela.conf; legacy_names.c/h shared with Settings
  motion.c/h      curves and tweens
  geometry.c/h    scale from DPI, exact-pixel placement
  color.c/h       night light and color filters
  sun.c/h         sunrise and sunset
  frame_clock.c/h one output's time
  render/         vulkan.c, renderer.c, texture.c, pass.c, allocator.c,
                  pixel_buffer.c (struct vela_renderer is a wlr_renderer)
  scene/          scene.c (nodes), surface.c, frame.c, effects.c,
                  ready.c, capture.c
  output.c/h      struct vela_output: frame cycle, vblank, VRR, power,
                  hotplug; output_config.c, nested.c
  input.c/h       seat, pointers, gestures, constraints, drag icon, paste
  keyboard.c/h    keyboards, layouts, key handling; super_numbers.c/h
                  the Super+number rules
  shell.c/h       the supervised shell and one-line messages to it
  polkit.c/h      starting Vela's polkit agent (docs/polkit-agent.md)
  view.c/h        struct vela_view: windows (xdg-shell), handles, animation;
                  xwayland.c for X11 windows and menus
  popup.c/h       menus of windows and shell pieces
  layer.c/h       shell pieces (wlr-layer-shell)
  decoration.c/h  Vela's title bar; text.c, icons.c
  snapshot.c/h    frozen window images and their animations
  focus.c/h       keyboard focus; interact.c/h pointer on windows
  bindings.c/h    shortcuts, window menu, window actions
  switcher.c/h    Alt+Tab
  snap.c/h        snap, preview, Snap Assist, layouts, groups
  workspace.c/h   virtual desktops; a11y.c/h; lock.c/h
  process.c/h     launching programs; supervised children (struct vela_child)
  buffer.c/h      growing text buffer (messages, JSON)
```

## 4. Migration plan, ordered by risk

Each step: find the current behavior, find the tests that define it (add
them first if the area is not covered), design a small C API, write it,
pass the tests, compare behavior and cost with the C++ version, remove the
C++ only when the replacement is complete. Vela builds and runs after every
step.

| # | Step | Risk | Contract | Status |
|---|---|---|---|---|
| 1 | Pure logic: motion, geometry, color, sun, frame clock | low | unit tests | done |
| 2 | `vela.conf` (config + legacy names shared with Settings) | low | unit tests | done |
| 3 | Supervisor and `main` | low | `test_session` supervisor cases | done |
| 4 | `outputs.conf`, text, icons, pixel buffers | low | new unit tests; new `test_decorations`; A/B screenshots identical | done |
| 5 | Renderer: device, allocator, renderer, textures, passes | medium | functional, sharpness, validation layers | done: A/B screenshots identical at 100–200% (windows, corners, shadows, title bars, snapped, maximized); CPU equal within noise |
| 6 | Scene: nodes, surface state, effects, ready commits, capture | medium | functional, `test_ready` | done |
| 7 | Frame: flatten, occlusion, damage, scanout, feedback, night light | high | functional, sharpness, damage debug | done: A/B identical with blur, color filter, night light, magnifier at 100% and 125%; new `test_layers` (partial redraw equals full redraw); CPU equal |
| 8 | Outputs: frame cycle, virtual vblank, late latching, VRR, hotplug, nested | high | frame clock tests, `test_outputs` | done: `output.c`, `nested.c`; `test_outputs` no longer needs wlr-randr (`tools/vela-randr`) and gains a scale/position case; A/B identical at 100–200% with blur, filters, night light, magnifier; CPU equal idle and dragging |
| 9 | Input and seat, accessibility | high | `test_windows`, manual | done: `input.c`, `keyboard.c`, `a11y.c`, `shell.c`; new `test_accessibility` (quick settings and vela.conf, night light schedule, sticky keys, Super alone opens Start, magnifier keys) through a fake shell socket; A/B identical; CPU equal. Gestures, wheel lines, touchpad settings, pointer constraints, paste and keyboard layouts are ported line by line and need a manual check on real hardware |
| 10 | Views, layers, decorations, snapshots, snap, workspaces | high | `test_windows`, sharpness | done: `view.c`, `xwayland.c`, `popup.c`, `layer.c`, `decoration.c`, `snapshot.c`, `workspace.c`, `snap.c`, `focus.c`, `interact.c`, `bindings.c`, `switcher.c`, `lock.c` (lock and idle moved here early: it only needed the focus); new tests for popups (`vela-pattern --popup`), X11 windows and menus (`tools/vela-x11`, `test_x11`), and pointer/keyboard interaction (`test_interaction`); A/B identical at 100–200% including light theme, wallpaper tint and hovered buttons; CPU equal |
| 11 | Command socket and state, session, shell supervision, output configuration, server; the god object is gone | high | `test_session`, everything | done: `server.c`, `command.c`, `session.c`, `output_manager.c`, shell supervision in `shell.c`; new `test_session.Shell` (a shell that keeps crashing is given up, one that exits cleanly is not restarted) and unit tests for the text buffer and the desktop names; the last C++ files are gone and the compositor links with the C linker; A/B identical at 100–200%; CPU equal idle and dragging |

`struct vela_server` (`server.h`) is no longer a god object: it holds the
wlroots objects every module needs (display, backend, renderer, scene,
layers, seat) and the lists (outputs, views, layer surfaces), and one
pointer per subsystem (input, accessibility, lock, interaction, switcher,
snapping, workspaces, command socket), each created and destroyed by its own
module's `init`/`finish`.

A fake shell, `tools/vela-panel` (wallpaper and a blurred taskbar on
wlr-layer-shell), lets the headless tests cover layers and blur without Qt.

An A/B harness builds the original C++ compositor in a separate worktree and
compares screenshots of the same scene pixel by pixel; title bars (text,
icons, buttons) were identical at 100% and 125% after step 4.

During steps 5–11 the remaining C++ called the C modules through their
headers; where a C module had to call code that was still C++, the function
was declared in the C header and implemented, temporarily, in C++ with C
linkage. Those implementations moved to C with their subsystem. After step
11 `compositor/src` was about 22,000 lines of C and no C++ (about 36,000 in
November 2026); only the GoogleTest suites are C++.

## 5. Bugs found along the way, and their fixes

The migration kept behavior unchanged, bugs included; the new tests and A/B
runs found these, all present in the original C++ compositor too, and they
were fixed after step 11:

- **Crash with screens powered off while locked (Intel), and a heap
  corruption everywhere.** Vela's GBM and pixel buffers freed themselves
  without calling `wlr_buffer_finish()`, so their destroy signal never
  fired: damage rings and the renderer's caches stayed attached to freed
  buffers and later wrote into the freed memory. Found with wlroots built
  with AddressSanitizer and instrumented `wl_list` functions; it is the
  `vkFreeMemory` crash of section 1 (the memory had been reused by Mesa) and
  most likely the old amdgpu crash at shutdown too. `test_session` covers it.
- **Crash on exit (C core only).** At shutdown the backend destroyed the
  outputs after snapping and accessibility were gone, and the output destroy
  handler used them: every exit ended in SIGSEGV, which the supervisor
  would take for a real crash. The functional harness now fails any test
  whose compositor doesn't exit with 0 when asked to stop.
- **First X11 window lost when it starts Xwayland.** Two wlroots behaviors
  together. While setting up its window manager wlroots flushes requests
  outside its event handler, and xcb reads whatever events have arrived into
  its queue, which is drained only when the socket becomes readable again:
  the client that started Xwayland created its window in that moment and
  never got it managed; later, the message pairing an X window with its
  Wayland surface could get stuck the same way. And wlroots maps an X11
  surface only at a commit after the pairing: when Xwayland had already
  committed the content, no other commit came. Vela now wakes the window
  manager (a root property change) when Xwayland is ready and whenever an
  X window gets its Wayland surface, and maps an already drawn surface when
  it's paired. `test_x11` no longer starts Xwayland first, and `vela-x11`
  draws only on Expose, like most apps.
- **`state` JSON and Vela's title bar.** For decorated windows `y` was 32
  too high and `h` 32 too tall (the bar counted twice); the Snipping Tool's
  `window-rects` had the same error. Both report the frame now.
- **Regions as large as possible.** A blur or opaque region of `INT32_MAX`
  (what toolkits send for "everything") overflowed when scaled to pixels:
  no blur, and no occlusion. Regions are now clamped to the element before
  the conversion; `vela-panel` asks for blur that way.
- **Magnifier view origin depending on the frame count.** The zoomed view
  was derived every animation frame from the previous one, so its last bits
  depended on how many frames the zoom took. It is now derived from where
  the cursor is drawn, fixed at the start of the zoom; A/B runs of the
  magnifier are identical.

The two `test_ready` failures on the Intel machine are not bugs: the slow
app and the compositor share the only GPU engine there.

## 6. Simplification after the migration

With the C core complete, a pass removed what wasn't earning its keep:
functions used only in their own file became `static` (and lost the
`vela_` prefix, which is for what a module exports), two unused functions
and write-only fields went, the three copies of the listener helpers and 39
hand-written `notify`/`wl_signal_add` pairs became `vela_listen()` and
`vela_unlisten()` (`listen.h`), field-by-field resets became `memset`,
duplicated code was folded into one function (restoring a window's frame,
the list of a view's listeners, sun times, the night light transition,
moving a desktop, the region-to-pixels conversion, JSON strings), trivial
accessors gave way to the fields (`struct vela_renderer` is visible to the
render module through `render/render.h`), and every comment was rewritten
in English.
