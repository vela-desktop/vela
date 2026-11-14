# Host allocation policy — design draft

> **Status:** accepted with option c, fifth revision, 9 November 2026,
> against `main` at `bb6cc0c`. Migration steps 1–3 are done (#148): the
> fail-closed recovery, `common/alloc` with its CI check, and the unchecked
> `realloc` and `getline` sites. Step 4 is done (#243). Step 5 is done
> (#246): `VELA_MUST` and the classification of library results, the local
> paths for descriptor exhaustion, the fallbacks removed with the baseline
> at zero, and the C++ new handler. Step 6 is done: the frame path draws
> without allocating, measured on the CPU and on a GPU. Step 7 is done,
> and with it the migration: the image-extent cap, upload staging in pieces
> under a byte budget, with deferred uploads that switch atomically and
> progress oldest first (point 6, "As implemented"), and the icon cache's
> entry budget (see "Migration order"). Every factual claim names the
> code it was checked against; claims not yet measured are marked
> *to verify*.

## Decision in one paragraph

C code in Vela obtains heap memory only through a small family of checked
helpers that never return `NULL`. A failed allocation, or overflow in a size
computation, reports the caller's file and line and calls `abort()`. The
process then restarts through the recovery path it already has.

That decision rests on four conditions, and this document treats them as
part of the policy, not as background:

1. **Crash recovery fails closed in every state.** Today it does not. Four
   paths can reveal the desktop after a crash; see "The premise".
2. **External input never reaches an aborting helper unvalidated.** The
   supervisor ends the session after three crashes in a minute
   (`supervisor.c`), so an abort that another party can trigger is a way to
   end the user's session. A size or count chosen by another party is
   checked against a documented bound where it enters, and rejected there.
3. **Memory that external input can make grow has a budget,** host heap and
   host-visible Vulkan memory alike. The allocator bounds nothing by
   itself. A single size is bounded where it enters; an aggregate (a cache,
   a pool, the staging needed in one frame) is bounded by its owner.
4. **Library failures that mean "out of memory" take the same fail-fast
   path.** Failures that have a safer local fallback (damage regions) use
   that fallback instead.

There is no fallible allocation tier, because no current host-heap site
needs one (see the audit).

## The premise: crash recovery must fail closed

Abort-on-failure is acceptable only if every crash leads to a safe state.
The supervisor restarts the compositor locked when the lock marker exists
(`supervisor.c` → `VELA_START_LOCKED` → `server.c`). The audit found four
ways in which a crash, from an allocation or from anything else, can end up
in an unlocked or stale screen. They shape the design. Items 1 and 2 are
fixed by the shared lock-state page (`lock_state.c`), which also closes
item 3; the fatal path no longer allocates on its own (see below). Item 4
is fixed by the damage fallback ("Design for (4)").

1. **Unchecked `setenv` after `fork()`.** The supervisor's restart child
   calls `setenv("VELA_START_LOCKED", "1", 1)` without checking the result
   (`supervisor.c` restart loop), and the restarted compositor decides
   whether to lock **only** from that variable (`server.c`). If `setenv`
   fails with `ENOMEM`, the compositor starts unlocked. This needs memory
   exhaustion across the whole system, not just in the compositor: by the
   time the child runs, the crashed compositor's memory has been freed. It
   is unlikely, but nothing makes it fail closed. The other `setenv` calls
   in the same child are unchecked too.

2. **A marker that cannot be read counts as absent.**
   `vela_lock_marker_present_at` (`path_security.c`) returns `false` when
   `fstatat` fails for any reason, not only `ENOENT`. An `ENOMEM` from the
   kernel, or a transient error on the runtime directory, therefore restarts
   the compositor unlocked even though the marker exists.

3. **The lock's fatal path allocates, and a crash anywhere during it
   restarts unlocked.** `lock_fatal_shutdown` (`lock.c`) is the path taken
   when the marker cannot be persisted (`vela_lock_marker_create_at`
   fails), or when the locker's retry timer cannot be armed or cancelled.
   Its job is to black out every output and end the session, never to
   restart it. On the way it:
   - grows `fatal_waiting` with `vela_grow`;
   - creates a timer with `wl_event_loop_add_timer`;
   - renders blackout frames through the whole frame path, which allocates
     (point 7);
   - then waits up to `VELA_LOCK_FATAL_TIMEOUT_MS` (2 s) for those frames
     to reach the screen.

   When the path was entered because the marker could not be created, a
   crash during that window is restarted **unlocked**: the supervisor sees a
   crash and finds no marker. This applies to a crash **anywhere** in the
   compositor during those two seconds, not only to one in the fatal path's
   own code. Making the fatal path allocation-free therefore cannot close
   this case by itself. The invariant the code documents ("end the entire
   session instead of ever allowing a restart without a marker") holds only
   as long as nothing crashes before the exit.

4. **A failed damage computation leaves stale pixels.** No pixman region
   operation's result is checked anywhere in the compositor: there are 33
   operations that can allocate (`union`, `union_rect`, `intersect`,
   `intersect_rect`, `subtract`, `copy`), 24 of them in `scene/frame.c`,
   including `union_box`, which also implements
   `vela_output_frame_damage_whole`. On `ENOMEM`, pixman turns the region
   into an empty "broken" region, and every later operation that takes a
   broken region as input produces a broken region too. The accumulated
   `scene_damage` of that frame is lost as a whole, the frame redraws
   nothing, and the region is only reset by the `pixman_region32_clear` at
   the end of the frame. Later frames redraw only their own damage, so the
   lost area stays stale until something else damages it. After the lock
   engages, that can leave desktop pixels on screen while the compositor
   believes it is showing the lock. This is unlikely, because these
   allocations are tiny, but it fails open.

### Design options for (1)–(3)

| Option | How | Fails closed? | Cost |
|---|---|---|---|
| a. Check every step | `_exit(127)` on any `setenv` failure; treat an unreadable marker as present; preallocate `fatal_waiting` for every output when outputs are added; give the fatal path a blackout that does not allocate | No. A crash elsewhere in the compositor during the 2 s blackout window still restarts unlocked, and every future change to these paths must stay allocation-free | Fragile: the invariant depends on code review |
| b. Restart locked after **every** crash | The restarted compositor locks unless the supervisor positively passes "was unlocked" | Yes, by construction: any failure to communicate means locked | The user re-enters the password after every crash, even when the desktop was unlocked |
| c. **Lock state in shared memory** | One shared page between supervisor and compositor (details below). The compositor stores "locked" before the lock becomes visible and "unlocked" only after a successful unlock. The marker file stays as a second signal | Yes: a store into an already mapped page cannot fail, and a compositor that cannot map the page while supervised ends the session (`VELA_EXIT_LOCK_UNSAFE`) | One shared page and a startup flag; the supervisor's existing "unsafe" exit-code protocol is kept |

**Recommendation: c**, plus the allocation-free fatal path from (a) as
defence in depth: with c, a crash in the blackout window restarts locked
instead of unlocked, and (a) makes it likelier that the session ends as
intended instead of restarting at all. Option c removes the dependency on
`setenv` and on file I/O at the moment it matters. Option b is the simplest
fallback if c is rejected.

**Option c in detail.** These points are part of the design, not
implementation choices:

- **Creation.** The supervisor creates a one-page `memfd` once, before the
  first fork, and seals its size (`F_SEAL_SHRINK`, `F_SEAL_GROW`), so no
  holder can truncate it under the other's mapping. It appends
  `--lock-state-fd=N` to the argv it already builds before the restart loop;
  no `setenv` is involved.
- **Inheritance.** The descriptor survives `execv` in the restart child only
  (its `CLOEXEC` flag is cleared there, as for `socket.fd` today). The
  compositor maps the page first thing in `main`, before it creates the
  server or starts any child, and then closes the descriptor: the mapping
  keeps the page alive, and no descriptor is left that a child could
  inherit. No application may hold one: a sandboxed application holding a
  writable view of the page could store "unlocked" into it.
- **Threat model.** "Sandboxed" means confined by namespaces or an LSM
  (Flatpak, a strictly confined Snap). Such an application has its own PID
  namespace or policy and cannot reach the compositor's `/proc/<pid>/fd`.
  An unconfined process of the same user (including a Snap in devmode) can
  reopen the page through `/proc/<pid>/fd` and store "unlocked", but it can
  already delete the marker in `$XDG_RUNTIME_DIR` today, so the page opens
  nothing new; such a process is "same user" (see the audit), not a trust
  boundary.
- **Who writes.** Only the compositor writes. The supervisor creates the
  page already zeroed ("unlocked") and backed by memory (it writes the
  initial value instead of only truncating, so the compositor's first store
  never has to allocate), then only holds it; it no longer decides whether
  to start locked and no longer passes `VELA_START_LOCKED`. The value
  persists across restarts: a restarted compositor reads the previous value
  first, then keeps the page in step with its own lock state. Nothing
  resets the page between restarts. Any value other than "unlocked" reads
  as locked.
- **The page cannot be mapped.** When `--lock-state-fd` was passed but the
  page cannot be mapped (or the descriptor is not a sealed one-page
  `memfd`), the compositor exits with `VELA_EXIT_LOCK_UNSAFE` before it
  shows anything, and the supervisor ends the session without restarting,
  as it already does for that status. Starting locked instead would not be
  enough: that instance could not record its lock state, so if the user
  later locked and the marker could not be created, a crash during the
  blackout window would again find a page saying "unlocked" and no marker,
  and restart unlocked. Ending the session costs little in practice: a
  one-page `mmap` fails only when the whole system is out of memory or
  under a very low `RLIMIT_AS`, and the rest of startup (Vulkan, wlroots)
  would almost certainly fail too. Without `--lock-state-fd` (an older
  supervisor), the compositor keeps today's marker-only behaviour.
- **The decision at startup** (`vela_lock_restart_locked`, unit-tested;
  the restart child's setup is `supervisor_child.c`). With the page mapped, the restarted
  compositor starts locked if **any** of these holds: the page says
  "locked"; the marker is present; the marker's presence cannot be
  determined (an `fstatat` error other than `ENOENT`, or an entry with the
  marker's name that is not this user's private regular file). Only a page
  that says "unlocked" **and** a marker that is positively absent start
  unlocked (`vela_lock_start_locked`, `vela_lock_marker_state_at`). A
  `VELA_START_LOCKED` from an older supervisor still forces a locked start.
- **The fatal path.** Because "locked" is stored before the marker is
  created, a marker that cannot be created no longer matters for a restart;
  the fatal path still ends the session as today.

### Design for (4)

Damage is a case where failing safe beats failing fast. A failed region
operation sets a "damage unknown" flag on the frame. The next frame then
redraws the whole buffer, without consulting the region or the damage ring,
since both may be broken and computing a full-output region would itself
be another region operation. The state
`vela_output_frame_reset_damage` already produces ("all buffers start over:
the next frame redraws them whole") is the model; *to verify* that this
path allocates nothing. Aborting would restart the session for a few bytes,
and ignoring the failure fails open; a full redraw is correct and cheap.
Every damage helper (`union_box` and the other region operations in
`scene/frame.c`) checks the `pixman_bool_t` result.

A flag alone is not enough, because the frame that hit the failure may
already be committed with stale content, and the lock trusts rendered
frames today: `vela_server_output_rendered` → `vela_lock_output_rendered`
(`lock.c`) counts an output as black, and once every output is counted the
locker receives `locked` (`wlr_session_lock_v1_send_locked`). The fatal
path counts `fatal_waiting` the same way. Therefore:

- **The flag schedules the recovery.** Setting it also schedules a frame on
  that output (`vela_output_schedule_frame`), so the full redraw happens
  even if nothing else changes.
- **A flagged frame is not a safe frame.** A frame rendered while its
  output's flag was set, or whose own damage computation failed, is not
  reported to `vela_lock_output_rendered`. Only the full redraw that clears
  the flag is.
- **`locked` follows a safe frame.** The locker is told `locked`, and the
  fatal path starts its scanout grace period, only once every output has
  reported a frame that was rendered whole or with valid damage after the
  lock engaged.

**As implemented.** `vela_region_failures` (`util.h`) counts failed region
operations; every allocating operation in `scene/frame.c` and in the pass's
clipping (`render/pass.c` `add_draw`) goes through `vela_region_ok`. The
regions wlroots computes (`wlr_region_transform`, the damage ring) are
checked with `pixman_region32_selfcheck`, which a broken region fails.
`vela_output_frame_render` checks at three points, each covering only the
failures since the previous one:

1. **Before drawing:** a failure since the last frame (including damage
   reported between frames), a broken scene damage or ring, or a previous
   frame that was not drawn whole resets the ring and makes the scene damage
   the whole output. A one-rectangle region (`pixman_region32_init_rect`)
   needs no allocation, so the recovery cannot fail in turn. This happens in
   the same frame: the failure does not reach the screen at all.
2. **After the damage ring:** a failure there makes the frame and buffer
   damage whole and resets the ring.
3. **After drawing:** a failure while drawing (a clip region) can leave parts
   of the buffer undrawn. The frame is marked unsafe
   (`vela_output_frame_safe`), is not reported to the lock, and schedules the
   next frame, which redraws whole.

Occlusion culling needed its own rule, found by the tests: a failure there
breaks the "covered" region and dropped elements that were visible, which no
full redraw brings back. A culling failure now keeps the elements (drawing
too much is correct; dropping one is not).

Tests (`test_session.py` `DamageFailure`): an `LD_PRELOAD` shim
(`compositor/tests/pixman_fail_preload.c`) makes `malloc` fail inside a
chosen pixman operation, so pixman takes its real failure path. Every
operation (`union`, `union_rect`, `intersect`, `subtract`, `copy`) is made to
fail at several points of a one-step window move (keyboard Move, with and
without decorations), which reaches culling, the damage diff, the ring and
the drawing. Each time the recovery frame must come by itself, before any
screen capture, and three captures (each after a frame that reuses another
swapchain buffer) must match a full redraw (`SIGUSR1`). A failure while
locking must show only the lock. Removing the counter, the culling rule or
the check before drawing makes these tests fail.

The three checks are also separate functions (`check_damage_before_drawing`,
`check_damage_after_ring`, `check_damage_after_drawing`), and
`compositor.damage-fallback` runs them and the culling rule on the CPU,
with pixman failing for real (the test's own `malloc`): removing any part
of any check, or the culling rule, fails it. The headless functional tests
could not tell two of them apart, because headless ignores the frame
damage the ring check protects (DRM uses it as `FB_DAMAGE_CLIPS`) and the
drawing failures reachable there clip shadow pieces that end up covered.
One part has no test that fails without it: the lock's wait for a safe
frame (`output.c`), since a lock frame's damage is the whole output, a
single rectangle, so its drawing never allocates.

## Existing defects recorded during the audit

Documented here so the design accounts for them. Defect 1 is fixed in
migration step 1; the others belong to later steps.

1. **A failed open or read erases the user's configuration.** `read_lines`
   (`compositor/src/config.c`) returns zero lines when `fopen` fails, for
   any reason, and stops at the first `-1` from `getline`, which means both
   end of file and an error (`ENOMEM`, I/O). `vela_config_write_checked`
   then writes what it collected back over `vela.conf` with an atomic
   rename. A file that exists but cannot be opened (`EMFILE`, `EACCES`,
   `ENOMEM`) is therefore replaced by a header and the one key being
   written, and an error halfway through the file drops the rest of the
   user's settings. Descriptor exhaustion is not hypothetical: the
   supervisor already samples the compositor's descriptor count.

   The same shape exists in three more places:
   - `output_config.c`: `vela_output_config_save` calls `read_file` and
     rewrites the file. If `outputs.conf` cannot be opened for any reason,
     `read_file` falls back to the legacy `schermi.conf`, so the rewrite
     either loses the saved settings of other monitors or brings back stale
     ones;
   - `workspace.c`: `desktop.conf` is read at startup and later rewritten
     from memory, so a failed or truncated read becomes permanent on the
     next save (rename a desktop, change a sticky app);
   - `vela-update-daemon` writes `vela.conf` through the same
     `vela_config_write_checked` (`updates/src/check.c`, linked through
     `vela-pure`), so it shares the first defect.

   **Fix:** every reader tells "the file does not exist" (`ENOENT` from the
   open) apart from "the file could not be read" (any other open error, or
   an error during the read; point 3). A read that fails is reported to the
   caller, and nothing derived from it is written back. The remaining loops
   (`icons.c` ×2, `text.c`) only read; they get the same treatment so that
   an error is never mistaken for a short or missing file.

   **As implemented.** `vela_config_open` and `vela_read_line` (`config.h`)
   carry the rule: only `ENOENT` is a missing file, and a read ends cleanly
   only when `getline` fails with `errno` still 0 and no stream error. Until
   `vela_oom` exists (step 2), `ENOMEM` from `getline` is a read error like
   `EIO`: nothing is written back, and the next step turns it into the
   fail-fast path. Writers refuse instead of rewriting: `vela.conf`
   (`vela_config_write_checked`, which `vela-update-daemon` uses, and the
   startup migration), `outputs.conf` (`vela_output_config_save`; the
   legacy `schermi.conf` is read only when `outputs.conf` is positively
   missing), and `desktop.conf` (read by `desktop_conf.c`; marked
   unreadable for the session, with a log line). The read-only readers
   treat a partial read as no data: the settings (`vela_config_read`), a
   saved monitor (`vela_output_config_load`), the section reader, now
   `vela_ini_read` in `config.c` and shared by `icons.c` and `text.c`, and
   the icon theme index in `icons.c`. Tests: `compositor.config-read` (a C test under
   an `LD_PRELOAD` shim, `compositor/tests/getline_fail_preload.c`, that
   makes `getline` fail on a chosen line: `EIO`, `ENOMEM`, descriptors
   exhausted, no permission),
   `ConfigurationRead` in `test_session.py` for `desktop.conf`, and
   `unreadableSettingsAreNeverRewritten` in `tests/updates/client.cpp` for
   the daemon. The `ferror` half of the end-of-file check is not covered:
   the shim cannot make glibc set the stream's error flag without `errno`.

2. **The icon cache grows without bound, keyed by client input.**
   `vela_icons_app` (`icons.c`) keeps a linked list of `cached_icon`
   entries keyed by `(app_id, size)`. It adds an entry even when no icon is
   found, never evicts, and searches linearly. A client that keeps changing
   its `app_id` grows the compositor's memory, and the cost of every lookup,
   for the whole session; every miss also costs a search of the icon theme
   on disk. Different output scales add entries for the same app.

   `vela_icons_app` returned a pointer into the cache. Its only caller
   (`decoration.c`) copied the pixels immediately, so eviction was already
   safe in practice.

   **Fixed in step 7.** The cache is `icon_cache.c`: at most
   `VELA_ICON_CACHE_BUDGET` (256) entries, hits and misses together, in a
   hash table with a use list; a full cache frees the least recently used
   entry first (`vela_icon_cache_insert`), and a successful lookup counts
   as a use (`vela_icon_cache_find`). `vela_icons_app` now returns a copy
   of the pixels that the caller owns (`icons.h`); `decoration.c` hands
   that copy to `set_image`, so an entry can be evicted while a title bar
   shows its icon. Pointers returned by `vela_icon_cache_find` and
   `vela_icon_cache_insert` are valid only until the next insert
   (`icon_cache.h`). An `app_id` longer than `VELA_ICON_APP_ID_MAX` (255
   bytes) gets no entry and no search (`vela_icons_app`); before, it was
   searched under its first 255 bytes (`icon_name`'s `char id[256]`).

3. **Upload staging is budgeted by count, not by size.** `stage`
   (`render/renderer.c`) creates a chunk of `max(size, 8 MiB)` when no
   existing chunk fits. `upload` (`texture.c`) stages each damaged rectangle
   separately, with `size` = rectangle width × bytes per pixel × rectangle
   height; the client chooses the damage, and a new buffer is damaged whole.
   `vela_renderer_collect`, which runs after
   every submission, frees idle chunks beyond `IDLE_STAGING_CHUNKS_KEPT` (2)
   of each kind, but whatever their size. Two consequences:
   - **Retention.** Up to two idle upload chunks of any size stay allocated,
     and a small upload may keep reusing an oversized chunk (`chunk->size >=
     size`).
   - **Peak within a frame.** A chunk being filled for commands not yet
     submitted cannot be reused, so N surfaces committing large shm buffers
     in the same frame need N such chunks at once.

   This memory is host-visible Vulkan memory, which is system RAM on most
   configurations. Readback staging already has the right model: a 128 MiB
   budget (`VELA_READBACK_MAX_BYTES`) that counts every readback chunk,
   including those in flight, with eviction of completed chunks
   (`make_readback_room`).

   **Fixed in step 7** (point 6, "As implemented"): upload chunks are all
   one piece (8 MiB) and at most 16 of them exist, so upload staging holds
   at most 128 MiB, in flight included; idle ones beyond two go once they
   have rested for a second (`vela_renderer_collect`).

4. **Library results are mostly unchecked.**
   - `wl_event_loop_add_fd`/`_timer` are called 22 times; most results are
     not checked. For example, `command.c` `handle_connection` stores a
     `NULL` source and then inserts the client, which then never receives
     events; `a11y.c` passes the result straight to
     `wl_event_source_timer_update`.
   - `wl_resource_create` results are checked with
     `wl_client_post_no_memory` where inspected (`output_preview.c`,
     `scene/effects.c`, `x11_root.c`); `scene/output_capture.c` is still to
     review.
   - 5 of the 46 `wlr_*_create` calls are checked on the following line.
     Some of those constructors can fail for reasons other than memory, so
     they need a per-call classification, not a blanket rule.

## Inventory at `bb6cc0c`

200 direct allocation calls in the in-scope C code: 162 in the session code
(`compositor/`, `bluetooth/`, `brightness/`, `capture/`) and 38 in the
update daemon (`updates/`).

| Shape | Session | Updates | Becomes |
|---|---|---|---|
| `calloc(1, sizeof(*x))`: one object | 72 | 0 | `VELA_NEW(type)` |
| `strdup` / `strndup` / `asprintf` | 43 | 32 | `vela_strdup` / `vela_strndup` / `vela_asprintf` |
| array or buffer with a fixed or computed size | 28 | 5 | `VELA_NEW_N(type, n)`, or a reused array (frame path) |
| `realloc` | 13 | 1 | `vela_grow` |
| `getline` (allocates through libc) | 6 | 0 | stays `getline`, with mandatory `ferror()` and `errno == ENOMEM` handling (see "`getline`" below) |

The first revision's search pattern missed `getline`. The pattern used for
the CI check (point 8) covers it, together with `getdelim`, `vasprintf`,
`open_memstream`, `realpath(…, NULL)`, `scandir`, `posix_memalign` and
`aligned_alloc`. A search on `bb6cc0c` found no uses of those others (the
one `realpath` call, in `backlight_choice.c`, passes a buffer).

About 30 sites in the session code check the result today. They follow
three conventions:
- return `false`/`NULL` silently (`capture_watch.c`, `config.c`,
  `output_manager.c`, `path_security.c`);
- log, then return (`render/allocator.c`, `render/pass.c`
  `vela_renderer_begin_pass`);
- reply with `wl_client_post_no_memory` (`output_preview.c`).

The update daemon adds a fourth: drop the data and continue
(`updates/src/check.c` `on_check_output`, whose report then fails to
parse). Only one failure path is tested today:
`compositor.wait-arrays-oom` (`compositor/tests/wait_arrays_oom_test.c`)
forces `realloc` to fail with `-Wl,--wrap=realloc` and checks `vela_grow`
through `render/wait_arrays.c`. It is migrated in step 2 of the migration
order, because it depends on `vela_grow` calling `realloc`.

Other defects:

- **Unchecked `p = realloc(p, n)`**, which loses the block and writes
  through `NULL`: `buffer.c` `reserve`, `config.c` `lines_push`, `icons.c`
  (lines 35, 104, 427) and `output_config.c`. The other `realloc` in
  `config.c`, the Bluetooth daemon's five and the update daemon's one go
  through a temporary and are checked.
- **`vela_buffer` overflows silently.** `buffer.c` `reserve` computes
  `length + more + 1` in `size_t` with no overflow check, and its capacity
  is `size_t`, not `int`.
- **`vela_grow` overflows silently.** It doubles an `int` capacity without
  a guard, multiplies `grown * size` unchecked, and returns `NULL` (no
  allocation) for `needed == 0` with an empty array.

## Audit: where sizes come from

Every site whose size is not a fixed `sizeof`, meaning the computed-size
sites, the string copies and the `getline` loops, falls into one of four
classes:

| Class | Meaning | Policy |
|---|---|---|
| **Internal** | Derived from Vela's own state | Fail fast |
| **External, bounded by the transport** | Chosen by another party, but limited by a hard limit in the channel | Fail fast; the bound is documented and tested |
| **Same user** | Files and IPC from processes running as the same user, outside any sandbox | Fail fast; not a trust boundary, because such a process can already end the session |
| **External, unbounded** | Chosen by another party, with no hard limit | Must be capped where it enters (point 6) |

Remote Bluetooth devices and sandboxed applications are **other parties**,
never "same user".

Results:

| Class | Sites | Bound |
|---|---|---|
| Internal | `command.c` `end_task`; `output_manager.c` (3); `snap.c`; `view.c`; `workspace.c` ordering; `icons.c` theme dirs; `render/pass.c` (4); `supervisor.c` argv; `brightness.c` and `capture_watch.c` published text; `path_security.c` components; `brightness/probe.c` sysfs names; `process.c` command copy; `updates/` argv arrays (`install.c`, `report.c`) | Counts of views, outputs, draws, waits, arguments |
| Internal, from the driver | `render/vulkan.c` (6); `render/allocator.c` modifiers | Vulkan and DRM enumeration counts |
| Internal, pixels chosen by the compositor | `decoration.c` (2); `icons.c` icon pixels; `render/pixel_buffer.c`; `text.c` | Scale × design size; text is ellipsised to `max_width`, which `decoration.c` only passes when `> 0` |
| External, Wayland | `output_preview.c` `preserve_position`; `decoration.c` title and app ID; `icons.c` cached app ID | A wire message carries its size in 16 bits, so no message exceeds 64 KiB whatever the configuration. Within that, libwayland's connection buffer applies: Vela does not call `wl_display_set_default_max_buffer_size`, so the default applies (4096 bytes, *to verify* with a test sending an oversized string) |
| External, X11 | Titles and classes of Xwayland windows, through wlroots | *To verify:* the length wlroots' XWM reads per property |
| External, system D-Bus | `bluetooth/agent.c` (5); `bluez.c`; `model.c` paths and values; `session.c`; `compositor/bluetooth.c` logind path | BlueZ and logind object paths are short; remote device names are capped at 248 bytes by the Bluetooth specification and by BlueZ (*to verify* for the other properties) |
| Same user | `config.c` (8); `icons.c` theme files; `output_config.c`; `workspace.c` names; `command.c` IPC lines (`MAX_LINE`); `shell.c` IPC line; `session.c` desktop names; every `getline` line; `updates/` state files, session-bus requests and `vela-update` output | IPC is capped by `MAX_LINE`; `vela-update` output by `MAX_OUTPUT` (1 MiB, `updates/src/check.c`); file lines are not capped, but the files belong to the user |
| External, unbounded, host heap | — | none found |
| External, unbounded, **host-visible Vulkan memory** | Client shm buffer size → upload staging: `texture.c` `upload_buffer` and `update_from_buffer` → `vela_renderer_stage_rows` | **Bounded since step 7.** Before: each damaged rectangle got its own staging of rectangle width × bytes per pixel × rectangle height, up to the whole buffer, kept as described in defect 3. Now: pieces of whole rows in chunks of `VELA_UPLOAD_PIECE_BYTES` (8 MiB), at most `VELA_UPLOAD_MAX_CHUNKS` (16) upload chunks, 128 MiB in all, in flight included; what does not fit waits for the next frames (point 6). |
| External, unbounded, **Vulkan image extent** | Client buffer dimensions → `vkCreateImage` extent: `texture.c` `upload_buffer` (shm) and `vela_vulkan_import_dmabuf` | **Capped since step 7.** Before: `upload_buffer` passed `buffer->width/height` straight to `vkCreateImage`, and the dmabuf format probe in `vulkan.c` called `vkGetPhysicalDeviceImageFormatProperties2` but ignored `maxExtent`; a shm buffer of 100000×1 needs only a 400 KB pool, and an extent above the limit is *invalid usage* in Vulkan (undefined behaviour), not a clean error. Now `extent_allowed` (`texture.c`) refuses it first (point 6). |

Conclusion: **no host-heap site has an unbounded external size**, so none
needs a fallible path. Two unbounded external quantities exist, both in
Vulkan:

- **The image extent.** It is capped where it enters (point 6): a buffer
  larger than the device's `maxImageDimension2D`, or the format's
  `maxExtent` for dmabuf, is rejected before `vkCreateImage`.
- **The staging memory for shm uploads.** The extent cap does not bound it:
  at the common limits of 16384 and 32768, one 4-byte-per-pixel buffer is
  1 GiB or 4 GiB. RADV on an RX 9070 XT (Mesa 26.2.6) reports 32768 for
  shm XRGB8888 (`maxImageDimension2D` 32768) and 16384 for linear XRGB8888
  dmabufs (the compositor logs both at startup; other drivers and the CI
  runners *to verify*). A wl_shm pool's size is an `int32_t`, so a client
  can send at most 2 GiB per pool. It is bounded by a byte budget and
  copied in pieces (point 6).

The revision-two claim that "no size is controlled by another party" was
wrong, and so was the revision-four claim that the image-extent cap is the
only one needed. Several sizes are chosen by clients; they are safe for
fail-fast only because the transport bounds them, so the bounds become
documented assumptions with tests (see Tests).

A client-chosen size is not the same thing as a failure that should affect
only that client. A 4 KB `strdup` fails only when the whole process is out of
memory. Replying `wl_client_post_no_memory` to one client does not free
anything, and the next allocation anywhere fails too. Isolating a failure to
one client makes sense only for a **large** allocation attributable to that
client, and that is the unbounded class, which is capped where it enters
instead.

### Aggregate growth

Bounding each allocation is not enough. Memory also grows through
collections that external input keeps adding to, and the allocator cannot
see that. Every long-lived pool was checked:

| Pool | Grows with | Bound today | Policy |
|---|---|---|---|
| Icon cache (`icons.c`, `icon_cache.c`) | Every distinct `(app_id, size)`, including misses | Since step 7: at most `VELA_ICON_CACHE_BUDGET` (256) entries, hits and misses together; a full cache frees the least recently used entry first, and a successful lookup counts as a use (`vela_icon_cache_insert`, `vela_icon_cache_find`). Before: none, never evicted | Done: a hash table with chaining, its buckets fixed at creation (at least twice the budget), and a use list (`icon_cache.c`); an `app_id` longer than `VELA_ICON_APP_ID_MAX` (255) gets no entry and no search (`vela_icons_app`) |
| Upload staging (`render/renderer.c`) | Size and number of shm uploads in one frame | Since step 7: at most 16 chunks of 8 MiB (128 MiB), idle, being filled or in flight; idle ones beyond two freed after a second at rest. Before: two idle chunks per kind, of any size, and nothing bounded a frame's peak | Byte budget over every upload chunk, in flight included, like readback; uploads copied in fixed-size pieces (point 6). Done |
| Readback staging | Screen capture requests | 128 MiB including chunks in flight, with eviction (`make_readback_room`) | Already compliant: this is the model |
| Per-client protocol objects | Client requests | Freed when the client disconnects | **Known risk, out of scope here.** A sandboxed client can keep creating objects while connected, and libwayland has no per-client limit; the problem is shared with every wlroots compositor. This policy does not make it worse: under overcommit, exhaustion ends in the OOM killer rather than in a failed `malloc`, and today only three sites answer a failure with `wl_client_post_no_memory` while the rest already crash on `NULL`. Per-client quotas, with their tests, are separate work |
| IPC connections and queued topics (`command.c`) | Same-user processes | One queued line per topic per connection | Same user: not a trust boundary |
| Bluetooth model (`bluetooth/model.c`) | BlueZ objects, including nearby devices | BlueZ's own expiry of temporary devices | Nearby devices are other parties: *to verify* under a spoofed-device flood, and given an entry budget if BlueZ's expiry does not bound it |

**Rule:** every long-lived collection whose size depends on input from
outside the process has an owner-defined budget (entries or bytes) and an
eviction rule, documented next to its definition and covered by a test.

## Policy

### 1. The helpers (`common/alloc/alloc.{h,c}`)

The public names are macros that pass the caller's location to `_at`
functions. Only the `_at` functions are real symbols.

```c
// Internal entry points. Every failure reaches vela_oom with these file/line.
void *vela_alloc_at(size_t count, size_t size, const char *file, int line);
void *vela_grow_size_at(void *items, size_t *capacity, size_t needed, size_t size,
    const char *file, int line);
// `needed` is wider than the int capacity on purpose: a caller's size_t or
// int64_t count arrives intact and is range-checked here, not truncated by an
// implicit conversion at the call.
void *vela_grow_int_at(void *items, int *capacity, long long needed, size_t size,
    const char *file, int line);
char *vela_strdup_at(const char *s, const char *file, int line);
char *vela_strndup_at(const char *s, size_t n, const char *file, int line);
char *vela_asprintf_at(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
size_t vela_size_add_at(size_t a, size_t b, const char *file, int line);
size_t vela_size_mul_at(size_t a, size_t b, const char *file, int line);

// Public API.
#define vela_alloc(count, size) vela_alloc_at((count), (size), __FILE__, __LINE__)
#define VELA_NEW(type) ((type *)vela_alloc(1, sizeof(type)))
#define VELA_NEW_N(type, n) ((type *)vela_alloc((size_t)(n), sizeof(type)))
// One name for both capacity types (C17 _Generic): size_t for byte buffers,
// int for the existing element arrays.
#define vela_grow(items, capacity, needed, size)                                   \
    _Generic((capacity), size_t *: vela_grow_size_at, int *: vela_grow_int_at)(   \
        (items), (capacity), (needed), (size), __FILE__, __LINE__)
#define vela_strdup(s) vela_strdup_at((s), __FILE__, __LINE__)
#define vela_strndup(s, n) vela_strndup_at((s), (n), __FILE__, __LINE__)
#define vela_asprintf(...) vela_asprintf_at(__FILE__, __LINE__, __VA_ARGS__)
#define vela_size_add(a, b) vela_size_add_at((a), (b), __FILE__, __LINE__)
#define vela_size_mul(a, b) vela_size_mul_at((a), (b), __FILE__, __LINE__)
```

The contract, and every item in it is tested:

- **Never `NULL`.** This includes `vela_alloc(0, x)`, `vela_alloc(x, 0)`,
  and `vela_grow` with `needed == 0` on an empty array. In both cases a
  minimum block is allocated: 1 byte for `vela_alloc`, the initial capacity
  of 8 elements for `vela_grow`. Callers can rely on a valid pointer; for
  example, `memcpy(p, src, 0)` with `p == NULL` is undefined behaviour in
  C17.
- **Zeroed.** `vela_alloc` returns zeroed memory. `vela_grow` zeroes the
  bytes between the old and the new capacity. All 180 non-`realloc`,
  non-`getline` sites already zero their block or write every byte they
  later read (`calloc`, string copies, `malloc` followed by a full copy), so
  zeroing changes no behaviour. Zeroing large blocks is
  cheap, because `calloc` gets them from `mmap` already zeroed.
- **Overflow is failure.** `count * size`, capacity doubling, and the
  narrowing of `needed` to `int` in `vela_grow_int_at` are all checked with
  `__builtin_*_overflow`. Because `needed` is `long long` there, the check
  sees the caller's real value: a count of 2³² is rejected instead of
  arriving as 0. A negative `needed` in the `int` form is a programming
  error and aborts with a distinct message; a `size_t` above `LLONG_MAX`
  arrives negative and takes the same path. Callers that compute
  a `needed` value (`length + more + 1` in `vela_buffer`) use
  `vela_size_add`/`vela_size_mul`, so the overflow is caught before the
  call.
- **Growth.** `vela_grow` is the only way to resize; there is no public
  `vela_realloc`. The `size_t` core serves `vela_buffer`, whose capacity
  stays `size_t`. The `int` adapter serves the existing element arrays
  (`int` counts, about 30 callers) unchanged.
- **Hidden allocations are not wrapped.** For example, `getline` keeps its
  libc form under the rule below.

**External values never reach the helpers unvalidated.** An abort in the
helpers, overflow included, is reserved for Vela's own values and for
values already checked at the boundary. A size or count derived from
another party is compared with its documented bound where it enters, and a
value beyond it is rejected in that boundary's own terms (point 6), before
any arithmetic on it reaches `vela_size_*` or `vela_alloc`. The reason is
the supervisor's crash budget: three crashes in a minute end the session,
so an abort that a client, a nearby Bluetooth device or a sandboxed
application can trigger at will ends the user's session.

### 2. Failure: `vela_oom`

```c
noreturn void vela_oom(size_t count, size_t size, const char *file, int line);
noreturn void vela_alloc_misuse(const char *what, const char *file, int line);
noreturn void vela_must_failed(const char *expression, const char *file, int line);
```

- `vela_oom` writes one line to stderr with `write(2)`, for example
  `vela: out of memory (3 x 4096 bytes) at compositor/src/render/pass.c:616; rss 412 MiB; available 37 MiB`.
  It uses no stdio, no `wlr_log` and no heap, then calls `abort()`.
- **The two memory figures say which kind of failure this was:** a huge
  request in a healthy system, or real exhaustion. `vela_oom` reads them
  with `open`/`read` into a stack buffer: `VmRSS` from `/proc/self/status`
  and `MemAvailable` from `/proc/meminfo`. If a read fails, the field is
  omitted.
- `vela_alloc_misuse` handles a negative `needed` and similar programming
  errors, with its own message
  (`vela: allocation misuse (negative count) at <file>:<line>`).
- An overflow in `vela_size_add` is not a request for memory, so it says
  so: `vela: size overflow (<a> + <b>) at <file>:<line>`, with the same
  memory figures. Every other overflow is reported by `vela_oom` with the
  operands as count and size.
- `vela_must_failed` serves `VELA_MUST` (point 2b). It writes the same line
  with the expression text in place of the sizes, for example
  `vela: out of memory (wl_event_loop_add_idle(loop, f, d)) at <file>:<line>; rss 412 MiB; available 37 MiB`,
  and then aborts. The three functions share the allocation-free writer.
- The session redirects stderr to `vela.log` (`session/vela-session.in`).
  `vela-report` already collects `vela.log` and crash metadata (signal and
  executable, through `coredumpctl`), so an allocation abort reaches a bug
  report with no changes to the report tool. Core dumps stay excluded by
  default, under the consent model in [reporting.md](reporting.md).

### 2b. OOM-only library results

Some library calls can fail only for lack of memory. For example:
- `wl_event_loop_add_idle`, which only allocates;
- `wl_resource_create`, which runs after libwayland has already validated
  the new id.

`wl_event_loop_add_fd`, `_timer` and `_signal` are **not** in this group.
They also duplicate a descriptor or create one (`timerfd_create`,
`signalfd`) and register it with `epoll_ctl`, so they can fail with
`EMFILE`, `ENFILE` or `ENOSPC`. Descriptor exhaustion is a realistic
failure here (the supervisor already samples the compositor's descriptor
count), so each call site handles their failure locally: for example, a new
IPC connection is closed and dropped, and a timer that cannot be created
takes the same path as one that cannot be armed. In libwayland 1.26 a loop's
timers share one `timerfd`, made by its first `wl_event_loop_add_timer`;
later timers only allocate. Vela's own `timerfd`s (an output's latch and
virtual vblank) are separate descriptors.

The functional descriptor-pressure tests distinguish authentication from event
registration. With no spare descriptor after accept, peer verification must
refuse the connection because it cannot obtain a pidfd. Authentication needs
more transient descriptors than watch registration, so a library shim isolates
the latter's `EMFILE` path after the real peer check succeeds. The timer case
first receives a state reply over an authenticated, watched connection, then
lowers the process's descriptor limit before creating an output. Both refused
connections and timer failures must leave the compositor usable after pressure
is removed; authentication is never bypassed for these tests.

OOM-only calls take the same path as the helpers:

```c
#define VELA_MUST(expr) vela_must_at((expr), #expr, __FILE__, __LINE__)
```

`vela_must_at` returns the pointer if it is non-`NULL`; otherwise it calls
`vela_must_failed` with the expression text (point 2). Each library call is classified once,
in a table kept next to `common/alloc/alloc.h`:
- **OOM-only:** wrapped in `VELA_MUST`.
- **Can fail for other reasons:** handled as a real error at the call site.
  Many `wlr_*_create` constructors (backends, renderers, GBM) belong here.
- **Has a safer fallback:** uses the fallback. Pixman damage is the case
  covered in "The premise".

Under this rule, `wl_client_post_no_memory` disappears: the resource either
exists or the process aborts, as with any other allocation.

### 3. `getline`

`getline` stays as it is; wrapping it gains nothing. It returns `-1` both at
end of file and on error, and whether a failed allocation sets the stream's
error flag is not something to rely on. `errno` is therefore cleared
immediately before each call, because the loop body may set it, and checked
after the loop. The open that precedes the loop follows the same idea: only
`ENOENT` means "no file".

```c
FILE *file = fopen(path, "re");
if (!file) {
    if (errno == ENOENT) {
        // No file: the defaults apply, and writing a new file is safe.
    } else {
        // Unreadable: report it, and never write back over the file.
    }
    …
}
for (;;) {
    errno = 0;
    ssize_t n = getline(&line, &capacity, file);
    if (n < 0) {
        break;
    }
    …
}
int error = errno; // 0 at a clean end of file
if (error == ENOMEM) {
    vela_oom(capacity, 1, __FILE__, __LINE__);
}
if (error || ferror(file)) {
    // Incomplete read: report it, and never write back what was derived from it.
}
```

### 4. Recovery, per process

| Process | After `abort()` | Checked against |
|---|---|---|
| `vela-compositor` | The supervisor restarts it and keeps the Wayland socket; Qt/KDE apps reconnect, others close. A locked session restarts locked: the shared lock-state page or a marker that is present or unreadable is enough; see "The premise". The budget is three crashes per minute, then the session ends. | `supervisor.c` restart loop (`WIFSIGNALED` covers `SIGABRT`); `tests/functional/test_session.py` `test_crash_while_locked_restarts_locked`, `test_crash_while_locked_restarts_locked_without_marker`, `test_unreadable_lock_marker_restarts_locked` and `test_gives_up_after_three_quick_crashes`, all using `SIGSEGV` |
| `vela-bluetooth` | Restarted by the compositor's generic child supervision. It stops after three consecutive exits that each happen within 5 s of start. | `process.c` `handle_exit` |
| `vela-update-daemon` | Started and restarted by the same generic child supervision, with the same limit | `updates.c` `vela_child_start`; `process.c` `handle_exit` |
| `vela-brightness` | Restarted by its own `exited` handler, with its own limit | `brightness.c` |
| Supervisor | Only its startup allocations (argv) use the helpers. The shutdown path stays allocation-free, as #90 established. | `supervisor_children.c` |

### 5. After `fork()`

Fork sites in scope: `lock.c` `lock_start_launcher`, `process.c`
`vela_spawn` and `vela_child_start`, `session.c` `vela_session_run_hook`,
and `supervisor.c` `run_and_wait` and the restart loop. The update daemon
does not fork: it starts its children with `posix_spawn`
(`updates/src/child.c` `vela_up_spawn`), so this point does not apply to
it.

- **Compositor children:** they only reset signals and descriptors
  (`sigprocmask`, `prctl`, `fcntl`) and then call `exec*` or `_exit`. They
  make no allocations. That matters: the compositor process can have
  threads started by libraries (for example the Vulkan driver), so `malloc`
  in a forked child could deadlock on a lock held by another thread at the
  moment of the fork.
- **The supervisor's restart child** calls `fcntl`, `snprintf`, `setenv`
  and `unsetenv` before `execv`; its argv, `--lock-state-fd` included, is
  built before the first fork. `setenv` can allocate. That is safe as far
  as locks go, because the supervisor creates no threads (no
  `pthread_create` in `supervisor*.c`; *to verify* that no library it
  initialises starts one). Every one of those calls is checked and a
  failure ends the child with `_exit(127)`, and the lock decision no longer
  travels through the environment ("The premise", items 1 and 2).
- **Rule:** after `fork()`, code calls none of the helpers. Any libc call
  that can fail is checked, and failure means `_exit(127)`.

### 6. External sizes

None is unbounded today in the host heap. Two are unbounded in Vulkan
memory (see the audit), and both are capped at texture import:

- **Extent:** a shm buffer larger than `maxImageDimension2D`, or a dmabuf
  larger than the format's `maxExtent`, is rejected before `vkCreateImage`.
  The surface keeps its previous content, and the rejection is logged once
  per client.
- **Staging:** each damaged rectangle is copied through staging in pieces
  of at most a fixed size (the 8 MiB chunk size is the starting point),
  splitting it by rows, so no chunk is ever larger than one piece. Every upload chunk, in flight
  included, counts against a byte budget. When the budget is reached within
  a frame, the remaining pieces wait for the next frame: the client buffer
  stays locked until its last piece is copied, and its damage stays
  pending, so nothing is skipped.
- **Atomic presentation.** An upload that does not complete within one
  frame never shows half-updated content. Its pieces go into a separate
  image of the same size, and the surface keeps drawing its previous,
  complete image until the last piece has been copied; then it switches to
  the new image in one step, and the old one is retired. The separate image
  is device memory, sized by the capped extent, and exists only while such
  an upload is in progress. An upload that fits within the frame's budget
  is copied in place, as today. Copying into a separate image is the only
  way that also works when a single damaged rectangle is larger than the
  whole budget.
- **Progress.** Pending uploads are served oldest first: each frame
  continues the upload that has waited longest before starting a newer
  one, so a surface that commits large buffers every frame cannot starve
  another one. A surface that commits a new buffer while its previous
  upload is still pending replaces that upload (the older buffer is
  released unseen), so a client cannot queue work faster than it is
  served.

**As implemented** (step 7, Vulkan part; checked against `render/vulkan.c`,
`render/renderer.c`, `render/texture.c`, `render/pass.c`, `scene/frame.c`
and `scene/surface.c`, and against wlroots 0.20.2's `wlr_compositor.c` and
`types/buffer/client.c`):

- **Extent.** `query_formats` (`vulkan.c`) keeps, for every shm format, the
  `maxExtent` of `vkGetPhysicalDeviceImageFormatProperties` for the image
  `upload_buffer` makes (optimal tiling; sampled, transfer source and
  destination), and for every (format, modifier) of `texture_formats` the
  `maxExtent` of `vkGetPhysicalDeviceImageFormatProperties2` for the uses
  `import_dmabuf` asks for. The limit that matters for `vkCreateImage` is
  that `maxExtent` (VUID-VkImageCreateInfo-extent-02252), not
  `maxImageDimension2D` alone, so each is the format's limit clamped to
  `maxImageDimension2D`; the XRGB8888 ones are logged at startup.
  `extent_allowed` (`texture.c`) checks shm buffers in `upload_buffer` and
  dmabufs in `import_dmabuf` before anything is created or imported. A
  refused buffer gets no texture, and wlroots then keeps the surface's
  previous buffer (`surface_apply_damage` returns when
  `wlr_client_buffer_create` fails), as point 6 asks. The refusal is
  counted (`state`: `upload.rejected`) and remembered with the buffer;
  the surface's commit handler (`scene/surface.c`) takes it
  (`vela_renderer_take_extent_rejection`) and logs it once per client: a
  destroy listener on the `wl_client` marks it as told. Only surfaces the
  scene watches log; a cursor surface's refusal is only counted. A shm row
  must also fit in one piece, which every limit a driver reports allows
  (32768 pixels × 8 bytes is 256 KiB).
- **Staging in pieces.** Upload chunks are all `VELA_UPLOAD_PIECE_BYTES`
  (8 MiB), and at most `VELA_UPLOAD_MAX_CHUNKS` (16) of them exist, idle,
  being filled or in flight, so upload staging never holds more than
  `VELA_UPLOAD_MAX_BYTES` (128 MiB, plus each allocation's alignment). It
  is a pool of its own, apart from readback's (`readback_test.c` already
  required that the two do not evict each other). One chunk is filled at a
  time (`vela_renderer_stage_rows`): a piece is as many whole rows of its
  rectangle as fit in what is left of that chunk, or in a fresh one (an
  idle chunk, else a new one within the budget). A copy made when its
  buffer comes (in place) is first planned with the same rule by
  `vela_renderer_reserve_upload`, which also allocates the chunks it needs
  before the first piece, so a failed allocation can never leave an image
  half updated. Idle upload chunks beyond two are freed once unused for
  `VELA_UPLOAD_IDLE_NS` (1 s); before, `vela_renderer_collect` kept two
  idle chunks of any size.
- **Deferred uploads and atomic presentation.** A copy that does not fit
  in what is left of the budget, or that comes while others wait, becomes
  the texture's pending upload (`struct vela_upload`, inside the texture,
  so deferring allocates nothing on the heap): the client's buffer is
  locked until the last piece, so the client gets `wl_buffer.release` only
  then. `vela_renderer_continue_uploads`, at the start of every output
  frame (`vela_output_frame_render`, before the scene is flattened), copies
  the next pieces as far as the budget goes and submits them on their own
  (`vela_renderer_flush_uploads`), since a frame with nothing to draw has
  no submission to carry them and their chunks only come back once
  submitted. The pieces go into an image of the same size made for the
  upload; when the damage is partial it starts as a GPU copy of the shown
  image (`vkCmdCopyImage`, no staging). The texture draws its previous
  image until the last piece, then switches in one step and retires the
  old image (`switch_image`). The scene sees the switch through
  `vela_texture_content`, which the frame diff compares (`same_look`), so
  the texture's area is redrawn although no commit brings that damage.
  While uploads wait, output frames go on, paced like any other (a frame
  with nothing else to draw costs almost nothing).
- **Where the design did not hold as written.** A texture with no previous
  image cannot keep showing it: the first buffer of a surface, and a buffer
  of another size, for which wlroots makes a new `wlr_client_buffer` and
  drops the previous one before the compositor sees the commit
  (`surface_apply_damage`). Such a texture fills its own image and is not
  drawn until its last piece (`vela_texture_shown`, checked by the scene
  and by `prepare_texture`); for those frames the surface shows nothing
  rather than its previous, differently sized content, which would also
  need its previous geometry. A half-updated image is never drawn in
  either case.
- **Progress.** Uploads are served oldest first, strictly: while one is
  waiting, a new upload waits behind it even if it would fit, so a small
  update can be delayed by the uploads ahead of it (by the length of the
  queue in budgets). A commit on a texture whose upload is pending
  replaces it: the older buffer is released unseen, and what it had left
  to copy, together with the new damage, is copied from the new buffer
  (the rectangles are merged, at most `VELA_UPLOAD_MAX_RECTS`). The
  replaced upload goes to the back of the queue, which is what keeps a
  client that commits above the budget every frame from starving the
  others; the price is that such a client's own new content appears only
  once it pauses for as long as its copy takes.
- **Budget values** (the open question): pieces of 8 MiB, the starting
  point, and an upload budget of its own of 128 MiB, the readback figure.
  128 MiB lets a whole 7680x4320 buffer at 4 bytes per pixel (126.6 MiB,
  16 chunks of 273 rows) be copied in place in one frame, as before, so
  common displays up to 8K keep the behaviour they had; only larger
  buffers, or several large ones in the same frame, are deferred. Measured
  on a Ryzen 9 9950X with an RX 9070 XT (Mesa 26.2.6 RADV), with
  `vela-bigbuffer`: staging and recording 128 MiB costs 7.0–7.8 ms of CPU
  with chunks already allocated (about 17 GB/s), the same as `main`'s
  single-chunk copy (7.5–7.7 ms for 126–128 MiB). Chunks allocated fresh
  cost more: 50 ms for the first 128 MiB of a session in 8 MiB or 16 MiB
  chunks, 13 ms in 32 MiB chunks, and 51 ms for `main`'s single 512 MiB
  chunk of a 16384x8192 buffer; a fresh 3840x2160 upload took 8.1 ms in
  8 MiB chunks, 3.2 ms in a 32 MiB one and 4.7 ms on `main`. 32 MiB
  pieces were not chosen because the two idle chunks kept would then pin
  64 MiB of host-visible memory for good, against 16 MiB; the one-second
  rest before idle chunks go keeps a copy that continues over frames, or
  comes every frame, on warm chunks. Why amdgpu populates fresh 8 MiB
  allocations four times more slowly than 32 MiB ones is *to verify*, as
  are these figures on other drivers.
- **Device loss and teardown.** On a lost device nothing continues and no
  new upload starts (`vela_texture_create` and `update_from_buffer`
  return early). A texture destroyed with an upload pending, or released
  at the renderer's teardown (`vela_texture_release`), drops the buffer's
  lock and frees the upload's image, at once when the GPU is idle or
  through `vela_renderer_retire_image` otherwise.
- **The frame path** still allocates nothing through Vela's helpers once
  warm, with uploads in place or deferred and replaced every frame
  (`compositor.upload-staging`); `functional-frame-path` is unchanged.

Whenever another external size appears, it is capped where it enters, and
rejected in that boundary's own terms: a protocol error, a declined command,
a default value. The cap is a named constant with a test at the boundary. If
a capped size can still legitimately fail (a user-requested multi-gigabyte
buffer), the policy is revised with that concrete case: a fallible helper,
its test, and a CI allowlist entry.

### 7. The frame path

**Before step 6** (checked at `7841a9b`, by reading and with the counter of
"Tests"), every pass, per frame and per output and for every capture,
allocated and freed:
- in `vela_renderer_begin_pass`: the `struct vela_pass`, plus
  `pass->draws`, `pass->blur_ops` and `pass->waits` grown with `vela_grow`.
  `pass_free` freed all four at the end of every submission, so none of
  them was reused across frames;
- in `foreign_textures`, `record` (`acquire`, `release`) and
  `vela_pass_submit` (the sync_files to wait for): four temporary arrays.

That is eight arrays and objects per pass, as this point said; the number of
allocations is higher, because `draws` starts at 8 elements and doubles: in
the functional test's scene (a decorated window and a dmabuf client), each
pass made nine, `draws` growing three times. The arrays in
`vela_output_frame` (`scene/frame.c`) and in the renderer (`commands`,
staging, semaphores, `retired`, the wait arrays, pipelines) persist and are
only freed at destroy; the counter saw none of them grow in steady state.

**As implemented.** The renderer owns its passes (`render/pass.c`):
- An open pass is on `renderer->passes_open`, from `vela_renderer_begin_pass`
  to `vela_pass_submit`; a finished one on `renderer->passes_free`.
  `begin_pass` takes the first pass of the free list, or creates one.
- `pass_release` is the end of every pass, submitted or not: a lost device,
  a command buffer that could not be had, a failed recording or submission
  all go through it. It drops the buffer lock and the timeline references
  held in `waits` and `signal_timeline`, zeroes the counts and the
  per-drawing state (target, color filter, timing slot), keeps the arrays,
  and puts the pass on the free list. No reference survives into the next
  pass. There is no other way to abandon a pass: neither Vela's API nor
  wlroots' `wlr_render_pass` has one.
- The four temporaries are arrays of the pass, grown with `vela_grow` and
  kept with it.
- **Several passes open at once.** Today only one is ever open: every
  caller (`scene/frame.c`, the window capture in `scene/capture.c`, and the
  passes wlroots begins through `begin_buffer_pass` for cursors and
  GPU-to-GPU capture copies) begins and submits its pass in one function,
  and nothing it calls in between begins another. The free list does not
  rely on it: each open pass is its own object, and the CPU test keeps an
  output frame and a capture open together.
- **Cap.** The free list keeps at most `VELA_PASS_FREE_MAX` (4) passes; a
  pass ending when it is full is freed. Since one pass is open at a time,
  the list holds one pass in practice; four leave room for an output frame
  and a capture or two open together without keeping a pass for every past
  peak. Nothing in the frame path opens more, so the cap is a bound, not a
  tuning knob.
- **Capacity.** Kept arrays keep their high-water capacity, which is the
  point. One unusual drawing can make that large: `draws` takes one
  element (200 bytes) per clip piece of every quad, and a fragmented damage
  region multiplies the pieces. So an array whose capacity is above
  `VELA_PASS_KEEP_BYTES` (1 MiB, about 5000 pieces for `draws`) when its
  pass ends is freed and grows again from 8 at the next drawing. An
  ordinary frame is far below that, so the trim never runs in steady state;
  a scene that really needs more than 1 MiB every frame allocates every
  frame, which is the price of not pinning that memory after a peak. Below
  the threshold nothing is trimmed: at most 4 passes × 1 MiB per array stays
  allocated.
- **Destroy and device loss.** `vela_renderer_destroy_passes`, first in the
  renderer's teardown, frees every pass once: those on the free list, and
  any still open, whose lock and references are dropped first (with an
  error in the log, since no caller leaves one open). A pass is host memory
  only, so device loss changes nothing for it: `vela_pass_submit` on a lost
  device releases the pass without recording it, the compositor then exits
  (`vulkan-device-loss.md`: the device is not rebuilt in place), and the
  teardown frees the list.

Allocations made by libraries in the same path are outside this goal and
are listed so the goal is stated honestly: the pixman region operations in
`scene/frame.c` (now checked through `vela_region_ok`; their failure is
handled by the damage fallback, "The premise", item 4), and wlroots'
internal allocations during commit.

**Allocations that belong to a client's request.** The counter also found,
in the frame path of a real session, allocations that are made once per
client request, not once per frame, and freed with the request. They are
not part of this goal, like wlroots' own objects for the same requests
(each captured frame is an `ext_image_copy_capture_frame_v1` that wlroots
allocates), and the functional test names each one:

| Site | Once per |
|---|---|
| `render/texture.c` `new_texture`, from `import_dmabuf` | dmabuf buffer committed by a client: an imported dmabuf is not kept once wlroots lets go of it (see `texture_destroy`, a RADV stall) |
| `scene/ready.c` `handle_client_commit` (`struct held`) | client commit with a buffer; freed at once when there is no fence to wait for |
| `scene/output_capture.c` `output_source_copy_frame` | captured output frame |
| `scene/capture.c` `vela_capture_copy_frame` | capture into shared memory |
| `render/texture.c` `vela_renderer_readback`, and the import of the output buffer it reads (`new_texture` again) | readback, at most `VELA_READBACK_MAX_JOBS` (4) at a time |
| `capture_watch.c` `publish` and `buffer.c` | change in who captures the screen, reported as JSON |

A capture of a 60 Hz output therefore still makes four allocations per
captured frame, and a client that commits a dmabuf every frame two. Pooling
them would change the capture and commit code, not the frame path; it is
left for when a measurement shows a cost. The readback's import of the
output buffer also repeats a Vulkan import per captured frame, which is
likely to cost more than the allocations.

The measurable goal is zero allocations through Vela's helpers per frame in
steady state: no `vela_alloc`, string copy or `vela_asprintf`, and no
`vela_grow` that grows. A `vela_grow` that finds room is a comparison, not
an allocation; the counter counts those calls apart. It is measured, not
assumed (see Tests).

### 8. Enforcement

A CI check (`scripts/check-allocations.py`, the `allocation-sites` test)
rejects, in the in-scope C sources (point 9, plus Vela's C libraries in
`common/alloc` and `common/caller`), everything except
`common/alloc/alloc.c`:
- `malloc`, `calloc`, `realloc`, `reallocarray`, `strdup`, `strndup`,
  `asprintf`, `vasprintf`, `posix_memalign`, `aligned_alloc`,
  `open_memstream` and `scandir`;
- `realpath` whose second argument is `NULL`. A `realpath` with a
  caller-supplied buffer (`backlight_choice.c`) does not allocate and stays
  allowed;
- `getline` and `getdelim` calls that do not follow the point-3 pattern:
  `errno = 0;` as the statement right before the call (so never in a loop
  condition, where a clear before the loop happens once), and `ENOMEM`
  checked later in the same function.

The check is a script over the preprocessed-free sources, so it can be
wrong in both directions. It has its own test
(`scripts/tests/test_check_allocations.py`): a fixture with one call of
every rejected form, each of which must be reported, and one with the
allowed forms (the helpers, `realpath` with a buffer, the point-3 `getline`
loop, members such as `allocator->malloc`, the names inside comments and
strings), none of which may be. The fixtures are text in the test, not `.c`
files, so the code scanners do not analyse them.

During migration, a baseline in `common/alloc/legacy-sites.txt` (outside
`docs/`, because CI reads it) counts the remaining legacy sites per file and
call. A count above the baseline fails, with the lines of that call; a count
below it fails too, asking for the baseline to be lowered in the same
change, so it only shrinks, down to empty. `--write-baseline` rewrites it.

### 9. Scope

- **In scope:** the C code in `compositor/`, `bluetooth/`, `brightness/`,
  `capture/` and `updates/`.
- **Out of scope:** `tools/` and the C test programs. They are developer
  and test tools; none of them is installed or runs in a session.
- **Library results:** classified by point 2b. The full classification of
  the 46 `wlr_*_create` calls and of the FreeType, cairo, GBM and sd-bus
  calls is a migration step, not a later project.
- **Vulkan** `VK_ERROR_OUT_OF_HOST_MEMORY` / `VK_ERROR_OUT_OF_DEVICE_MEMORY`
  stay under [vulkan-device-loss.md](vulkan-device-loss.md). The budget rule
  for aggregate growth applies to Vulkan pools too (upload staging).
- **C++/Qt processes** (`vela-shell`, Settings, `vela-lock`, polkit agent
  and prompt, `vela-files`, `vela-report-gui`) already end on allocation
  failure. No Vela code catches `std::bad_alloc` and Vela's builds do not
  disable exceptions, so a failed `new` ends in `std::terminate` →
  `abort()`. Qt's own internals may instead crash directly; the outcome is
  the same. The shell is restarted by the
  compositor's child supervision (`server.c`, `vela_child_init` for
  "Shell"). The only addition proposed is a `std::set_new_handler` in a
  shared C++ header that writes the same `vela: out of memory …` line before
  aborting, so `vela.log` reads the same whichever process failed (step 5
  of the migration order).
- **Signal handlers:** checked. The supervisor's handlers only call `kill`
  and set a `sig_atomic_t` (`supervisor.c` `on_terminate`). The compositor
  and Bluetooth daemon handle signals through their event loops
  (`wl_event_loop_add_signal`, `sd_event_add_signal`). Rule: no helper is
  ever called from an asynchronous signal handler.

## Tests

- **Call site:** a death test forces a failure through a size that
  overflows (`vela_alloc(SIZE_MAX, 2)`), which is deterministic and needs no
  hook. It checks that the printed `file:line` is the test's own line. The
  same test is repeated through `VELA_NEW_N`, `vela_grow` (both capacity
  types, including a 64-bit count of 2³² in the `int` form), `vela_size_add`
  and `vela_size_mul`. A string copy cannot overflow, so `vela_strdup`,
  `vela_strndup` and `vela_asprintf` are checked through a forced
  allocation failure instead: the test binary wraps `calloc` and `realloc`
  (`-Wl,--wrap`), and the wrappers also return `NULL` for a zero size, as C
  allows, and fill new `realloc` memory with a pattern, so only the helpers'
  own handling makes a zero-size block non-`NULL` and a grown tail zero
  (`alloc.helpers`, `common/alloc/tests/alloc_test.c`).
- **Real allocation failure:** a death test in a child process with a low
  `RLIMIT_AS` checks the `vela_oom` message and `SIGABRT`. It is skipped
  under ASan, which reserves its shadow memory beyond any such limit; the
  memory figures are also checked through a forced failure.
- **Existing test:** `compositor.wait-arrays-oom` keeps forcing the
  `realloc` under `vela_grow` to fail, and expects the `vela_oom` abort and
  its line, which names `render/wait_arrays.c` and the size of the array
  that failed.
- **Contract:** the zero-size cases return non-`NULL` and can be freed.
  `vela_grow` zeroes the tail, doubles the capacity, preserves contents, and
  does nothing when `needed` is not larger than the capacity and the array
  exists. The `int` adapter aborts on a negative `needed` and on values
  beyond `INT_MAX`.
- **`vela_buffer`:** growth through the `size_t` core; an append whose
  length overflows aborts.
- **Configuration reads:** `vela.conf`, `outputs.conf` and `desktop.conf`
  are left unchanged when:
  - the open fails with an error other than `ENOENT` (the file made
    unreadable, and descriptors exhausted with a low `RLIMIT_NOFILE`); for
    `outputs.conf`, the legacy `schermi.conf` is not read in that case;
  - `getline` fails with `ENOMEM` or `EIO` on the Nth line (an `LD_PRELOAD`
    shim).

  The same test runs through `vela-update-daemon` for `vela.conf`.
- **Fail-closed recovery, as a matrix:** the compositor is killed with
  `SIGABRT` (and `SIGSEGV`) in each of these states. A crash while the
  desktop is unlocked restarts unlocked, as designed: that is the first
  row. In every other row the result must be a compositor that restarts
  locked or a session that ends, never an unlocked compositor and never
  desktop pixels on screen:
  - unlocked (restarts unlocked);
  - locked;
  - while locking (between the shared-state store and the first lock
    frame);
  - inside `lock_fatal_shutdown` with the marker creation forced to fail,
    both in the fatal path itself and from an unrelated point during the
    blackout window;
  - with `setenv` forced to fail in the restart child;
  - with the marker present but `fstatat` on it forced to fail.
- **Lock-state page:** an application started by the compositor holds no
  descriptor for the page (checked through `/proc/<pid>/fd`). A page that
  cannot be mapped while `--lock-state-fd` was passed (a descriptor that is
  not a sealed one-page `memfd`, and `mmap` forced to fail) makes the
  compositor exit with `VELA_EXIT_LOCK_UNSAFE` before its first frame, and
  the supervisor ends the session without a restart. Without
  `--lock-state-fd`, the marker-only behaviour is unchanged.
- **Damage failure:** a region operation forced to fail (`LD_PRELOAD` shim
  on pixman) during the lock transition. The test records every frame
  presented from the forced failure on, not only the next one:
  - the full redraw is presented without any other scene change;
  - no presented frame after the lock engaged shows desktop pixels, with
    the possible exception of the flagged frame itself, which is then not
    counted;
  - the locker receives `locked` only after the full redraw has been
    presented on every output, and the fatal path, with the same failure
    forced, starts its grace period only after it.
- **Budgets:** the icon cache stays within its entry budget under a client
  that cycles through distinct `app_id` values. Upload staging stays within
  its byte budget, both after one oversized shm buffer and while several
  surfaces commit large shm buffers in the same frame, and every surface
  still ends up showing its whole buffer.

  **As implemented** (icon cache): `compositor.icon-cache`
  (`compositor/tests/icon_cache_test.c`, CPU) checks the cache against a
  model, and that the count never exceeds the budget while cycling app
  ids, sizes and misses. `compositor.icons`
  (`compositor/tests/icons_test.c`, CPU) goes through `vela_icons_app` on
  a theme the test writes, including a held icon whose entry is evicted
  and is then drawn again (the cases that draw an icon need librsvg).
  `functional-decorations` `IconCache` (GPU) has a client cycle 600 app
  ids with `vela-pattern --cycle-app-ids`: the other window's bar keeps
  its icon, and the evicted icon is drawn again. Upload staging is below.
- **Deferred uploads:**
  - every frame presented while an upload spans several frames shows the
    surface's previous buffer complete, never a mix (checked with a
    pattern that differs in every row and a screen dump of each frame);
    the frame after the last piece shows the new buffer complete;
  - under sustained competing uploads (one client committing a buffer
    above the budget every frame), a second surface's upload still
    completes within a bounded number of frames.

  **As implemented** (upload staging and deferred uploads):
  `compositor.upload-staging` (`compositor/tests/upload_test.c`, CPU, also
  in hosted CI) runs the production `renderer.c`, `texture.c` and `pass.c`
  on a fake device that keeps images and buffers in memory and runs the
  recorded copies and draws only when its timeline reaches them. Every
  draw compares the image it reads with the buffers committed, and one
  that is no whole buffer fails the test: that is the "never a mix" check,
  exact and for every frame, including a frame the GPU runs late. The
  budget is the production code with smaller numbers (64 KiB pieces, 8
  chunks). It covers one buffer of three budgets (four frames, staging
  never above the budget, no piece above one, the buffer locked until its
  last piece, its chunks freed after their rest), four surfaces committing
  in the same frame, a deferred update with full and with partial damage
  (the previous buffer whole until the last piece, the new one from that
  frame on, the old image retired after its last use), a commit replacing
  a pending upload, a second surface completing within three frames while
  another commits above the budget every frame, staging in flight with the
  GPU behind, a lost device with uploads pending, and no allocation once
  warm. Each of these was broken on purpose and the test failed: the
  budget ignored when allocating a chunk or when planning a copy, chunks
  reused while in flight, the deferred pieces copied into the shown image,
  the switch made before the last piece, the copy of the shown image
  skipped, a first upload drawn before its last piece, a replaced upload
  kept in its place in the queue or its buffer not released, the buffer
  not locked while pending, pieces not split, new uploads copied in place
  while others wait, and idle chunks never freed.
  `functional-uploads` (`tests/functional/test_uploads.py`, GPU) repeats
  them through a session with `tools/vela-bigbuffer`, whose buffers are
  bands with a red per version and a green per band: a 16384x8192 buffer
  (512 MiB) is deferred, stays within the 16 chunks and is shown whole;
  so are three buffers of 128–132 MiB committed together; every frame
  `vela-screen-record` records while a deferred update is copied shows one
  version (with Vulkan validation on); and a 136 MiB upload completes
  within 8 frames (2 measured) while another client commits 144 MiB every
  frame. Comparing `vela_texture_content` in the frame diff is checked
  there: without it the new buffer never reaches the screen. Not covered:
  that frames are paced while uploads wait; without the condition in
  `vela_output_frame_render` the uploads still complete, by retrying at
  once instead of at the next frame, and no test measures that.
- **`VELA_MUST`:** prints the expression and call site, through
  `vela_must_failed`.
- **Event sources:** with the running compositor's `RLIMIT_NOFILE` lowered
  to its next free descriptor, a new IPC connection is dropped; lowered to
  leave room for a connection and its watch alone, the connection kept
  open while its command runs,
  a hot-plugged output gets neither its latch nor its virtual vblank
  `timerfd` (nor its buffers, so it starts off) and, switched on once the
  limit is restored, presents frames following the backend's; the
  compositor keeps running
  (`functional-resources`, `DescriptorExhaustion`). libwayland's own timers
  cannot fail this way after startup (point 2b).
- **Enforcement:** the CI check's own fixtures (point 8).
- **Image extent cap:** a client attaches a shm buffer wider than
  `maxImageDimension2D` (and a dmabuf above the format's `maxExtent`). The
  buffer is rejected before `vkCreateImage`, and Vulkan validation layers
  report no error.

  **As implemented:** `compositor.upload-staging` gives the fake device
  limits and checks a shm buffer and a dmabuf at the limit (taken) and one
  pixel above it in either direction (refused, with no `vkCreateImage` or
  import call, and reported once); a limit of one pixel less, or no check,
  makes it fail. `functional-uploads` asks the real device's limits from
  the log and sends a shm buffer and a linear dmabuf one pixel wider
  (refused, the window shows nothing, one log line for the client even
  after a second refused commit) and one at the limit (taken), with Vulkan
  validation on and no validation error.
- **Frame path:** a counting wrapper linked only into test binaries
  (`compositor/tests/alloc_count.c`, `-Wl,--wrap` on the `_at` functions)
  counts the helpers' allocations, in two places:
  - `compositor.frame-path`, on the CPU (hosted CI included): the production
    `pass.c`, `renderer.c` and `texture.c` with the Vulkan entry points
    stubbed. 240 warmed frames, each an output frame and a capture with both
    passes open at once (rectangles in several clip pieces, a shadow, a
    blur, a shared-memory texture, an imported one read with explicit
    sync), make no allocation, and every buffer lock and timeline reference
    is back after each frame. It also covers a pass ended by a lost device,
    the cap of the free list, the trim after one huge drawing, and the
    renderer's teardown with passes kept and one open, each freed once.
  - `functional-frame-path`, on a GPU (label `gpu`, skipped without one):
    `vela-compositor-alloc-count`, the compositor's own objects linked with
    the counter, in a headless session that redraws every frame (a
    decorated window and a dmabuf client committing every frame with
    explicit sync), then with a capture copying every frame. No allocation
    is allowed outside the per-request sites of point 7, which it names and
    prints.

  The drawing itself is checked on the CPU because its stubbed device is
  deterministic; the session test is there for what a real frame calls
  besides the pass. The headless fixture needs a real GPU (it never falls
  back to software rendering), and it showed no GPU-specific flakiness:
  the renderer's persistent arrays (command buffers, semaphores, retired
  images), which a slower GPU could make grow, stayed at their warmed size
  in 10 runs of 10 here.
- **Transport bounds:**
  - a Wayland client sends a `preserve_position` larger than the documented
    bound, and the connection is refused by libwayland, not allocated;
  - an X11 window with an oversized title is truncated by wlroots
    (*to verify* first).
- ASan/UBSan, plus the CPU and GPU suites touched by each step.

## Migration order

1. **Fail-closed recovery first,** because the whole policy depends on it:
   - **done:** the shared lock-state page (option c), including an
     unreadable marker counting as present, checked `setenv` in the restart
     child, and their tests (`compositor.LockState.*`,
     `compositor.LockMarker.*`, and the lock-state tests in
     `test_session.py`);
   - **done:** an allocation-free lock fatal path. Its timer is created
     with the lock, `fatal_waiting` is reserved for every output when the
     lock engages and on every layout change while locked, and the path no
     longer rebuilds the backdrop (it is already current while locked). It
     still renders through the frame path (step 6) and sends libwayland
     leave events; a crash there restarts locked through the page
     (`test_crash_during_lock_blackout_restarts_locked`);
   - **done:** the damage fallback (see "Design for (4)", as implemented);
   - **done:** the configuration-read defects (open errors and read
     errors, in the compositor and in the update daemon; see "Existing
     defects", 1);
   - **done:** the recovery test matrix (`Supervisor` in
     `test_session.py`): unlocked, locked, while locking, during the fatal
     blackout, `setenv` failing in the restart child once and always, and
     `fstatat` failing on the marker (an `LD_PRELOAD` shim,
     `compositor/tests/libc_fail_preload.c`), with `SIGSEGV` and `SIGABRT`.
     A crash inside `lock_fatal_shutdown` itself is not reproduced: the
     tests stop the compositor once the fatal path has logged its start,
     which is the window it opens, not its own code.
2. **Done:** `common/alloc` with its tests (`alloc.helpers`); `vela_grow`
   with its two capacity types, replacing the one in `util.c`, and
   `compositor.wait-arrays-oom` moved onto it; `vela_buffer` on the `size_t`
   core with checked addition (`compositor.Buffer*`); the CI check with a
   full baseline (`allocation-sites`, 193 sites). An empty array asked for
   zero elements is now allocated instead of left `NULL`: the scene diff's
   first empty frame allocates its match flags once
   (`compositor.scene-diff`). The one `getline`, in `vela_read_line`
   (`config.c`), is in the baseline until step 3 adds its `ENOMEM` check.
3. **Done:** the unchecked `realloc` sites (`config.c` `lines_push`,
   `icons.c` three, `output_config.c` one) are on `vela_grow`; the checked
   one in `config.c` waits for step 4 with the rest of that file.
   `vela_read_line` follows the `getline` rule: `ENOMEM` ends in `vela_oom`
   instead of counting as a read error (`compositor.config-read` runs it in
   a child and checks the abort and its line). The baseline is down to 188
   sites, with no `getline` left in it.
4. Mechanical conversion of objects and strings, in one PR with one commit
   per subsystem: render, scene, input/command, windows, outputs/session,
   config/icons, daemons, update daemon. Windows: `view`, `layer`, `popup`,
   `xwayland`, `x11_root`, `snap`, `switcher`, `workspace`, `decoration`,
   `text`. Outputs/session: `output`, `output_manager`, `output_preview`,
   `lock`, `session`, `server`, `nested`, `snapshot`, `supervisor_child`,
   `a11y`, `capture_watch`, `brightness`, `bluetooth`. Config/icons also
   takes `output_config.c` and `path_security.c`.
   A `NULL` check right after a converted allocation is removed with it,
   together with the cleanup it guarded: the helper never returns `NULL`,
   so the branch is unreachable and cannot leave partial state. Checks of a
   function's result in its callers stay until step 5, because those
   functions still fail for other reasons. A `count ? count : 1` guard
   against a zero-size `calloc` goes too, since `vela_alloc` allocates a
   minimum block for zero. **Done:** 186 of the 187 sites, so the baseline
   is down to one, `check.c` `on_check_output`'s `realloc` (see the update
   daemon below).
   - **render** (`compositor/src/render/`, 21 sites): the Vulkan and DRM
     enumeration arrays, the pass temporaries (which step 6 turns into
     arrays owned by the pass), and the renderer, target, texture, readback,
     GBM allocator and buffer objects. The pixel buffer's `width × height`
     goes through `vela_size_mul`. Six dead checks went
     (`vela_renderer_begin_pass`, `vela_target` creation, the readback,
     three in `allocator.c`).
   - **scene** (`compositor/src/scene/`, 15 sites): the scene, its tree,
     surface, rect and buffer nodes, the output frame, the surface state,
     the background-effect object, the ready gate and its held commits, and
     the window and output capture sources with their pending frames. Four
     dead checks went: the two pending capture frames (which failed the
     frame), the output source (which posted `no_memory` to the manager) and
     the output-capture manager.
   - **input/command** (`input.c`, `keyboard.c`, `interact.c`, `command.c`,
     `shell.c`, `process.c`; 13 sites): the input, pointer, constraint, drag
     icon, keyboard and interaction objects; the IPC listener, its clients
     and their queued topic lines; the `end_task` PID list; the line sent to
     the shell, whose `length + 1` goes through `vela_size_add`; and the
     spawned command's copy. None had a check.
   - **windows** (23 sites): the XDG and X11 views, unmanaged X11 windows,
     layer surfaces, popups, the X11 root's `xdg_output` objects and
     manager, the switcher, snapping and its group list, workspaces with
     their names and the taskbar ordering, the decoration with its title,
     app ID, texture owner and images, and the text renderer with its
     pixels. Image sizes (`decoration.c` twice, `text.c`) go through
     `vela_size_mul`, and the cleaned desktop name's `length + 1` through
     `vela_size_add`. Two dead checks went, both in `x11_root.c`: the
     `xdg_output` (which posted `no_memory` to the client) and the manager.
   - **outputs/session** (28 sites): outputs, nested outputs, the output
     manager's tracked views and saved entries, output previews, the lock
     with its surfaces and inhibitor watches, the server, the session's
     desktop names (`length + 16` through `vela_size_add`), the supervisor's
     restart argv, accessibility, window snapshots and their animations, the
     capture watch (capturers, sessions, frames, streams, published text),
     brightness (outputs, helpers, published text) and the logind session
     path for Bluetooth. Nine dead checks went: five in `capture_watch.c`,
     `tracked` and the `keys && entries` condition in `output_manager.c`,
     the restart argv in `supervisor_child.c`, and the preview owner. The
     supervisor's message for a `NULL` argv now names the remaining causes
     instead of "out of memory". The two `wl_client_post_no_memory` branches
     in `output_preview.c` stay for step 5, though they cannot be reached
     any more.
   - **config/icons** (`config.c`, `icons.c`, `output_config.c`,
     `path_security.c`; 20 sites): configuration keys, lines and entries,
     the legacy-key rewrite and the written line (`vela_asprintf`), icon
     directories, theme directories and names, cached icons and their pixels
     (`size × size` through `vela_size_mul`), output-config section names
     and path components. The checked `realloc` in `config.c` `set_entry` is
     on `vela_grow`, so `config.c` has no legacy site left. Four dead checks
     went: the entry copy and its array growth in `set_entry`, the written
     line in `vela_config_write_checked` (which returned `false`), and the
     path component in `path_security.c`.
   - **daemons** (`bluetooth/`, `brightness/`, `capture/`; 29 sites). In the
     Bluetooth daemon, the five checked `realloc` sites (adapters, devices,
     removed paths, waiters, search leases) grew by one element per call;
     they are on `vela_grow` with a new `size_t` capacity next to each
     count, reset wherever the array is freed. Its objects (adapters,
     devices, questions, operations, leases) and strings use `VELA_NEW` and
     `vela_strdup`. Fourteen dead checks went with them: the property copy
     in `set_text`, the new adapter and device, both in `note_removed`, the
     two that rejected a question, and the `-ENOMEM` paths of
     `vela_bt_waiters_add`, the operation, the power request, the search
     adapter, the lease array and the lease, and the adapter choice. The
     logind session path keeps its check, which also covers a reply without
     a path. `brightness/probe.c` copies its sysfs names with `vela_strdup`,
     and libvela-capture's job uses `VELA_NEW` without its dead check. Both
     libraries now link `vela-alloc`, which the top-level build adds
     whenever the compositor, the shell or the capture tools are built,
     since libvela-capture is built without the compositor too.
   - **update daemon** (`updates/src/`, 37 of 38 sites): the report strings
     and pair arrays parsed from `vela-update`'s JSON and the state files,
     the terminal argv, the install argv (`count + 17` through
     `vela_size_add`), the state paths (`vela_asprintf`), the source URL and
     the synthesized check and install results. Fourteen dead checks went,
     among them the combined `NULL` tests after parsing a check, an install
     and a run, the two inside `pairs_at`, the terminal argv's copy loop,
     and `make_dirs`. Parse errors (schema, JSON) still fail as before. The
     one left is `check.c` `on_check_output`'s `realloc`: its failure drops
     the data and continues, which step 5 names, so it stays whole until
     then.
5. Closing the failure contract, in one PR:
   - classification of library results (point 2b) and `VELA_MUST`, with
     local failure paths for the event sources that create descriptors;
   - removal of the old fallback branches (`wl_client_post_no_memory` in
     `output_preview.c`, the silent returns, the update daemon's dropped
     data) and of the callers' checks left by step 4, with a review of
     each for partial state. The baseline reaches zero;
   - the C++/Qt processes (point 9, "Scope"): a `std::set_new_handler` in
     a shared C++ header, installed at the start of `main` in
     `vela-shell`, Settings, `vela-lock`, the polkit agent and prompt,
     `vela-files` and `vela-report-gui`. The handler has no size, file or
     line to report, so it writes
     `vela: out of memory (operator new) in <process>` followed by the
     same `rss` and `available` figures, through the allocation-free writer
     that `vela_oom` uses, then calls `abort()`. Tested like `vela_oom`: a
     child forces a failed `new` and checks the abort and the line.
   - **Done.** `VELA_MUST` and `vela_must_failed` are in `common/alloc`,
     with the classification in `common/alloc/library-results.md`
     (checked against the wlroots 0.20.2 and libwayland 1.26.0 sources).
     `wl_event_loop_add_idle`, `wl_resource_create`, `wl_global_create`
     and the wlroots constructors that only allocate and create a global
     go through it, so the nine `wl_client_post_no_memory` branches
     (`output_preview.c`, `x11_root.c`, `scene/effects.c`,
     `scene/output_capture.c`) are gone, and so are the callers' checks
     of `vela_output_manager_snapshot`, `vela_output_capture_manager_create`
     (now `void`), `vela_x11_root_create`, the update daemon's state paths
     and the Bluetooth model's new adapters and devices.
     `wlr_ext_foreign_toplevel_handle_v1_create` is not OOM-only (its
     identifier comes from `/dev/urandom`), and its unchecked result in
     `view.c` is now handled: the window has no handle, which every user
     of `ext_handle` already allows. Descriptor exhaustion takes local
     paths at the twelve compositor sources that had none (the IPC
     listener and its connections, the brightness helper's timer and
     socket, a spawned child's pidfd, which is now stopped and reaped,
     paste, key repeat, snap layouts, accessibility, the idle timer, and an
     output's latch and virtual vblank) and at the seven unchecked
     `sd_event_add_*` calls of the daemons. `check.c` `on_check_output`
     grows its buffer through `vela_grow` (bounded by `MAX_OUTPUT`), so
     the baseline is empty. The new handler is in
     `common/alloc/new_handler.h`, installed first in the seven C++
     programs; `vela-files` and `vela-settings` were seen writing the line
     under a low `prlimit --as`, the other five installing it under gdb.
     `common/alloc` is now part of every build.
6. The frame path (point 7), measured against the existing benchmarks.
   **Done.** The renderer keeps finished passes on a free list (at most 4)
   with their arrays, and the four temporaries of a submission are arrays of
   the pass; arrays above 1 MiB are freed when their pass ends. The code
   matched point 7's description; what it did not list are the allocations
   made once per client request (a dmabuf import per committed buffer, a
   held commit, a captured frame with its copy and readback), now listed
   there and left as they are. Measured with a counting wrapper of the
   helpers: `compositor.frame-path`, on the CPU, 0 allocations in 240
   frames of an output and a capture open at once (a pass created every
   frame instead: 3840); `functional-frame-path`, on a GPU, 0 in the frame
   path over 90 frames of a headless session, with and without a capture,
   where the same window counted 810 in `render/pass.c` before (nine per
   pass). The benchmarks show no measurable change, before (`7841a9b`) and
   after, alternating builds on one machine (Ryzen 9 9950X, RX 9070 XT,
   Mesa 26.2.6 RADV): the `VELA_STATS=1` frame CPU time of the same scene at
   1920x1080@240, 50 two-second periods each, median 0.281 ms both
   (interquartile range 0.280–0.281); the capture-stream benchmark of
   `test_capture.py`, three runs each, the same frame rates and p95 latency
   within run-to-run spread (2560x1440, one client: 179.6–180.0 fps, p95
   5.8 ms on both). Nine small allocations per pass are too little to show
   in a frame's CPU time. The scene-diff benchmark does
   not cover this code (it excludes Vulkan submission), and `frame.c` did
   not change.
7. The Vulkan caps and budgets: the image-extent cap, upload staging in
   pieces under a byte budget with atomic presentation and oldest-first
   progress, and the icon-cache budget.
   **Done.** The Vulkan part first (point 6, "As implemented"). The code matched the
   audit: `upload` staged each damaged rectangle whole through
   `vela_renderer_stage`, and the dmabuf probe dropped `maxExtent`. Two
   parts of the design changed with the evidence: the limit checked is the
   format's `maxExtent` clamped to `maxImageDimension2D`, for shm buffers
   too, since that is what `vkCreateImage` requires; and a texture with no
   previous image (a surface's first buffer, or one of a new size, which
   wlroots gives a new texture) is not drawn until its last piece instead
   of keeping a previous content it no longer has. Budget values: 8 MiB
   pieces, 128 MiB of upload staging of its own. Measured with
   `compositor.upload-staging` and `functional-uploads` (see Tests). The
   benchmarks show no measurable change, alternating builds of `main`
   (`79ac129`) and this step on the machine of point 6: the `VELA_STATS=1`
   frame CPU time at 1920x1080@240, two runs of 24 two-second periods
   each, median 0.273 and 0.247 ms on `main` against 0.256 and 0.239 ms
   (the decorated window and dmabuf client of `functional-frame-path`),
   0.273 and 0.271 against 0.259 and 0.209 ms with `vela-bigbuffer`
   committing a 1920x1080 shm buffer every frame, 0.275 and 0.270 against
   0.282 and 0.287 ms with a 3840x2160 one (120 fps, the client's own
   rate); this figure covers the frame, not the copy made at commit. The
   capture-stream benchmark of `test_capture.py`, two runs each, gave the
   same frame rates and p95 within run-to-run spread (2560x1440, one
   client, buffer reuse: 180.0 fps, p95 5.75–5.84 ms on both; 3840x2160:
   176.9–177.7 against 177.3–178.3 fps). A whole buffer copied when it
   comes costs the same CPU time as before (above).
   The icon cache: `icon_cache.c` keeps 256 entries with LRU eviction and
   a hash lookup, and `vela_icons_app` returns copies (defect 2, "Fixed in
   step 7"). The code matched the audit there too: a list, searched
   linearly, never evicted, with misses stored. Measured with
   `compositor.icon-cache`, `compositor.icons` and `functional-decorations`
   `IconCache` (see Tests).

## Options considered

| | Option | Outcome |
|---|---|---|
| A | First draft: fatal helpers plus `vela_try_*` wherever "a feature can cleanly fail" | Rejected on evidence: the audit found no unbounded external size in the host heap. Point 6 keeps the door open for a concrete case. |
| B | Graceful `NULL` handling at every site | Hundreds of untested branches and half-built objects. Restart-based recovery already exists. |
| C | Interpose `malloc` globally (`--wrap`, `LD_PRELOAD`) in production | Would turn graceful library paths (libwayland, Mesa, FreeType) into aborts. In tests only, it is useful (see Tests). |
| D | Emergency reserve block freed on OOM | Useless under overcommit. Orderly shutdown already exists as the crash path. |
| E | **One fail-fast family, with external sizes validated and bounded where they enter (this proposal)** | Matches the crash-only recovery Vela already has. Enforceable in CI. Tested in one place. |

## Related, separate work: exhaustion at session level

With `vm.overcommit_memory=0`, ordinary allocations rarely fail. Real
exhaustion is handled by the kernel OOM killer or by systemd-oomd, which
kills a whole cgroup. Supervisor, compositor, shell and every application
share logind's `session-N.scope`:
- `vela-session` `exec`s the supervisor;
- the shell launches apps with `vela::process::startDetached`;
- the compositor launches the terminal with `vela_spawn`.

*Expected, to verify in a disposable VM:* oomd ends the whole session
instead of the application that caused the pressure. Plasma and GNOME avoid
this with one transient `app-*.scope` per application. That needs its own PR
and document.

## Open questions

- **Lock state after a crash:** option a, b or c (recommended: c, plus an
  allocation-free fatal path).
- **Budget values:** answered. Icon-cache entries (step 7): 256. A
  session needs one per app with a Vela title bar per output scale (about
  a hundred in a busy session, an estimate, *to verify* in a real
  session), and a full cache holds about 1 MiB of pixels at 200% (4 MiB at
  400%) plus at most 64 KiB of keys; the reasoning is next to
  `VELA_ICON_CACHE_BUDGET` in `icon_cache.h`. Upload staging (answered in
  step 7, point 6): a budget of its own of 128 MiB, the readback figure
  but a separate pool, so captures and uploads cannot evict each other;
  pieces of 8 MiB.
- **Core dumps under memory pressure.** `abort()` during real exhaustion
  starts `systemd-coredump` on a process with large GPU mappings. That adds
  pressure and delays the restart while the screen stays frozen. Options:
  keep it (the default), lower `RLIMIT_CORE` only for `vela_oom`, or end
  with a dedicated exit status instead of `SIGABRT`, which the supervisor
  would count as a crash. *To measure* how long the dump takes for the
  compositor before deciding.
- Whether `vela_oom` should leave a marker so the supervisor can report
  "out of memory" instead of a generic crash. The `rss`/`available` fields
  may make that unnecessary.
