# Host allocation policy — design draft

> **Status:** proposal, fifth revision, 9 November 2026, against `main` at
> `bb6cc0c`. No implementation in this PR. Every factual claim names the
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
in an unlocked or stale screen. None of them is fixed in this phase; they
shape the design.

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
| c. **Lock state in shared memory** | One shared page between supervisor and compositor (details below). The compositor stores "locked" before the lock becomes visible and "unlocked" only after a successful unlock. The marker file stays as a second signal | Yes: a store into an already mapped page cannot fail, and a compositor that cannot map the page while supervised starts locked | One shared page and a startup flag; the supervisor's existing "unsafe" exit-code protocol is kept |

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
  compositor sets `FD_CLOEXEC` on it again at startup, before it starts any
  child, as it already does for the runtime-directory descriptor. No
  application may inherit it: a sandboxed application holding a writable
  view of the page could store "unlocked" into it.
- **Who writes.** Only the compositor writes; the supervisor only reads.
  The value persists across restarts: a restarted compositor reads the
  previous value first, then keeps the page in step with its own lock
  state. Nothing resets the page between restarts.
- **The decision at startup.** The restarted compositor starts locked if
  **any** of these holds: the page says "locked"; the marker is present;
  the marker's presence cannot be determined (an `fstatat` error other than
  `ENOENT`); the page cannot be mapped while `--lock-state-fd` was passed.
  Only a page that says "unlocked" **and** a marker that is positively
  absent start unlocked.
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

## Existing defects recorded during the audit

Documented here so the design accounts for them. They are not fixed in this
phase.

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

2. **The icon cache grows without bound, keyed by client input.**
   `vela_icons_app` (`icons.c`) keeps a linked list of `cached_icon`
   entries keyed by `(app_id, size)`. It adds an entry even when no icon is
   found, never evicts, and searches linearly. A client that keeps changing
   its `app_id` grows the compositor's memory, and the cost of every lookup,
   for the whole session; every miss also costs a search of the icon theme
   on disk. Different output scales add entries for the same app.

   `vela_icons_app` returns a pointer into the cache. Its only caller
   (`decoration.c`) copies the pixels immediately, so eviction is safe
   today; the contract "valid until the next call" becomes part of the
   function's documentation before eviction is added.

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

4. **OOM-only library results are mostly unchecked.**
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
| External, unbounded, **host-visible Vulkan memory** | Client shm buffer size → upload staging: `texture.c` `upload_buffer` → `vela_renderer_stage` | **None.** Each damaged rectangle gets its own staging of rectangle width × bytes per pixel × rectangle height, up to the whole buffer, kept as described in defect 3. The client pays for one pool it can reuse; the compositor pays for a copy of it. |
| External, unbounded, **Vulkan image extent** | Client buffer dimensions → `vkCreateImage` extent: `texture.c` `upload_buffer` (shm) and `vela_vulkan_import_dmabuf` | **No check against the device's limits.** `upload_buffer` passes `buffer->width/height` straight to `vkCreateImage`; the dmabuf format probe in `vulkan.c` calls `vkGetPhysicalDeviceImageFormatProperties2` but ignores `maxExtent`. A shm buffer of 100000×1 needs only a 400 KB pool. An extent above the limit is *invalid usage* in Vulkan (undefined behaviour), not a clean error. |

Conclusion: **no host-heap site has an unbounded external size**, so none
needs a fallible path. Two unbounded external quantities exist, both in
Vulkan:

- **The image extent.** It is capped where it enters (point 6): a buffer
  larger than the device's `maxImageDimension2D`, or the format's
  `maxExtent` for dmabuf, is rejected before `vkCreateImage`.
- **The staging memory for shm uploads.** The extent cap does not bound it:
  at the common limits of 16384 and 32768 (*to verify* per driver on the CI
  runners), one 4-byte-per-pixel buffer is 1 GiB or 4 GiB. It is bounded by
  a byte budget and copied in pieces (point 6).

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
| Icon cache (`icons.c`) | Every distinct `(app_id, size)`, including misses | None, never evicted | Fixed entry budget with LRU eviction, and a hash or direct lookup instead of a list |
| Upload staging (`render/renderer.c`) | Size and number of shm uploads in one frame | Two idle chunks per kind, of any size; nothing bounds a frame's peak | Byte budget over every upload chunk, in flight included, like readback; uploads copied in fixed-size pieces (point 6) |
| Readback staging | Screen capture requests | 128 MiB including chunks in flight, with eviction (`make_readback_room`) | Already compliant: this is the model |
| Per-client protocol objects | Client requests | Freed when the client disconnects | Per-client quotas: shared with every wlroots compositor, out of scope here |
| IPC connections and queued topics (`command.c`) | Same-user processes | One queued line per topic per connection | Same user: not a trust boundary |
| Bluetooth model (`bluetooth/model.c`) | BlueZ objects, including nearby devices | BlueZ's own expiry of temporary devices | Nearby devices are other parties: *to verify* under a spoofed-device flood, and given an entry budget if BlueZ's expiry does not bound it |

**Rule:** every long-lived collection whose size depends on input from
outside the process has an owner-defined budget (entries or bytes) and an
eviction rule, documented next to its definition and covered by a test.

## Policy

### 1. The helpers (`common/alloc.{h,c}`)

The public names are macros that pass the caller's location to `_at`
functions. Only the `_at` functions are real symbols.

```c
// Internal entry points. Every failure reaches vela_oom with these file/line.
void *vela_alloc_at(size_t count, size_t size, const char *file, int line);
void *vela_grow_size_at(void *items, size_t *capacity, size_t needed, size_t size,
    const char *file, int line);
void *vela_grow_int_at(void *items, int *capacity, int needed, size_t size,
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
  `size_t` → `int` conversion in `vela_grow_int_at` are all checked with
  `__builtin_*_overflow`. A negative `needed` in the `int` form is a
  programming error and aborts with a distinct message. Callers that compute
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
  errors, with its own message.
- The session redirects stderr to `vela.log` (`session/vela-session.in`).
  `vela-report` already collects `vela.log` and crash metadata (signal and
  executable, through `coredumpctl`), so an allocation abort reaches a bug
  report with no changes to the report tool. Core dumps stay excluded by
  default, under the consent model in [reporting.md](reporting.md).

### 2b. OOM-only library results

Some library calls can fail only for lack of memory. For example:
- `wl_event_loop_add_fd`, `_timer`, `_signal` and `_idle`;
- `wl_resource_create`, which runs after libwayland has already validated
  the new id.

These take the same path as the helpers:

```c
#define VELA_MUST(expr) vela_must_at((expr), #expr, __FILE__, __LINE__)
```

`vela_must_at` returns the pointer if it is non-`NULL`; otherwise it calls
`vela_oom` with the expression text. Each library call is classified once,
in a table kept next to `common/alloc.h`:
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
| `vela-compositor` | The supervisor restarts it and keeps the Wayland socket; Qt/KDE apps reconnect, others close. A locked session restarts locked, but today only while the lock marker exists and can be read, and `setenv` succeeds; see "The premise". The budget is three crashes per minute, then the session ends. | `supervisor.c` restart loop (`WIFSIGNALED` covers `SIGABRT`); `tests/functional/test_session.py` `test_crash_while_locked_restarts_locked` and `test_gives_up_after_three_quick_crashes`, both using `SIGSEGV` |
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
- **The supervisor's restart child** calls `snprintf`, `setenv`,
  `unsetenv` and `fstatat` before `execv`. `setenv` can allocate. That is
  safe as far as locks go, because the supervisor creates no threads (no
  `pthread_create` in `supervisor*.c`; *to verify* that no library it
  initialises starts one). It is not safe as far as failure goes; see
  "The premise", items 1 and 2.
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
  pending, so nothing is skipped. *To decide in implementation:* whether
  one surface may use the whole budget, or the budget is shared fairly
  between surfaces.

Whenever another external size appears, it is capped where it enters, and
rejected in that boundary's own terms: a protocol error, a declined command,
a default value. The cap is a named constant with a test at the boundary. If
a capped size can still legitimately fail (a user-requested multi-gigabyte
buffer), the policy is revised with that concrete case: a fallible helper,
its test, and a CI allowlist entry.

### 7. The frame path

Per frame and per output, Vela currently allocates and frees:
- in `vela_renderer_begin_pass`: the `struct vela_pass`, plus
  `pass->draws`, `pass->blur_ops` and `pass->waits` grown with `vela_grow`.
  `pass_free` frees all four at the end of every submission, so none of
  them is reused across frames;
- in `foreign_textures`, `record` (`acquire`, `release`) and
  `vela_pass_submit` (`waits`): four temporary arrays.

That makes eight allocation/free pairs per pass. The arrays in
`vela_output_frame` (`scene/frame.c`) and in the renderer (`staging`,
semaphores, `retired`) persist and are only freed at destroy.

**Proposal:** a free list of `vela_pass` objects owned by the renderer.
- `begin_pass` takes a pass from the list (or creates one) and resets its
  counts while keeping its arrays.
- `pass_free` drops the references (the `wlr_buffer`, and the
  `wlr_drm_syncobj_timeline` references held in `waits` and
  `signal_timeline`) and puts the object back on the list instead of
  freeing it.
- The four temporaries become arrays owned by the pass.
- Several passes can be open at once (for example an output frame and a
  screen capture), and the free list handles that. Timeline references are
  released before reuse exactly as they are today, so no reference
  survives into the next pass.

Allocations made by libraries in the same path are outside this goal and
are listed so the goal is stated honestly: the pixman region operations in
`scene/frame.c` (24 that can allocate, none checked), and wlroots' internal
allocations during commit. Their failure is handled by the damage fallback
("The premise", item 4), not by this goal.

The measurable goal is zero calls to Vela's allocation helpers per frame in
steady state. It is measured, not assumed (see Tests).

### 8. Enforcement

A CI check rejects, in the in-scope C sources, everything except
`common/alloc.c`:
- `malloc`, `calloc`, `realloc`, `reallocarray`, `strdup`, `strndup`,
  `asprintf`, `vasprintf`, `posix_memalign`, `aligned_alloc`;
- `getline` and `getdelim` calls that do not follow the point-3 pattern
  (`errno` cleared before the call and checked after the loop).

During migration, a baseline in `common/alloc-legacy-sites.txt` (outside
`docs/`, because CI reads it) lists the remaining legacy sites. Additions
fail, and the baseline only shrinks, down to empty.

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
  aborting, so `vela.log` reads the same whichever process failed.
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
  types), `vela_strndup` and `vela_size_add`.
- **Real allocation failure:** a death test in a child process with a low
  `RLIMIT_AS` checks the `vela_oom` message and `SIGABRT`.
- **Existing test:** `compositor.wait-arrays-oom` keeps forcing the
  allocation under `vela_grow` to fail, now through whatever `vela_grow_*_at`
  calls, and expects the `vela_oom` abort.
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
  `SIGABRT` (and `SIGSEGV`) in each of these states, and none may ever
  produce an unlocked compositor or end in desktop pixels on screen:
  - unlocked;
  - locked;
  - while locking (between the shared-state store and the first lock
    frame);
  - inside `lock_fatal_shutdown` with the marker creation forced to fail,
    both in the fatal path itself and from an unrelated point during the
    blackout window;
  - with `setenv` forced to fail in the restart child;
  - with the marker present but `fstatat` on it forced to fail.
- **Lock-state page:** an application started by the compositor holds no
  descriptor for the page (checked through `/proc/<pid>/fd`), and a
  page that cannot be mapped while `--lock-state-fd` was passed starts the
  compositor locked.
- **Damage failure:** a region operation forced to fail (`LD_PRELOAD` shim
  on pixman) leads to a full redraw on the next frame. This is checked in
  the lock transition with a screen dump.
- **Budgets:** the icon cache stays within its entry budget under a client
  that cycles through distinct `app_id` values. Upload staging stays within
  its byte budget, both after one oversized shm buffer and while several
  surfaces commit large shm buffers in the same frame, and every surface
  still ends up showing its whole buffer.
- **`VELA_MUST`:** prints the expression and call site.
- **Image extent cap:** a client attaches a shm buffer wider than
  `maxImageDimension2D` (and a dmabuf above the format's `maxExtent`). The
  buffer is rejected before `vkCreateImage`, and Vulkan validation layers
  report no error.
- **Frame path:** a counting wrapper linked only into the test binary
  (`-Wl,--wrap` on the `_at` functions) checks that a steady scene makes
  zero helper calls across N frames, with several passes open at once
  (output frame plus capture).
- **Transport bounds:**
  - a Wayland client sends a `preserve_position` larger than the documented
    bound, and the connection is refused by libwayland, not allocated;
  - an X11 window with an oversized title is truncated by wlroots
    (*to verify* first).
- ASan/UBSan, plus the CPU and GPU suites touched by each step.

## Migration order

1. **Fail-closed recovery first,** because the whole policy depends on it:
   - the shared lock-state page (option c, or the option chosen), including
     an unreadable marker counting as present;
   - an allocation-free lock fatal path;
   - the damage fallback;
   - the configuration-read defects (open errors and read errors, in the
     compositor and in the update daemon);
   - the recovery test matrix.
2. `common/alloc` with its tests; `vela_grow` with its two capacity types,
   and `compositor.wait-arrays-oom` moved onto it;
   `vela_buffer` on the `size_t` core with checked addition; the CI check
   with a full baseline.
3. The unchecked `realloc` sites (`buffer.c`, `config.c`, `icons.c`,
   `output_config.c`) and the `getline` rule.
4. Mechanical conversion of objects and strings, one subsystem per PR
   (render, scene, input/command, config/icons, daemons, update daemon).
5. The frame path (point 7), measured against the existing benchmarks.
6. Classification of library results (point 2b) and `VELA_MUST`.
7. The Vulkan caps and budgets: the image-extent cap, upload staging in
   pieces under a byte budget, and the icon-cache budget.
8. Removal of the old fallback branches (`wl_client_post_no_memory` in
   `output_preview.c`, the silent returns, the update daemon's dropped
   data), with a review of each for partial state. The baseline reaches
   zero.

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
- **Budget values:** icon-cache entries; upload-staging bytes, which could
  share the 128 MiB readback figure or have their own; the size of one
  upload piece.
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
- Whether the frame-path counter test can run in the existing headless
  fixture without GPU-specific flakiness.
