# Host allocation policy — design draft

> **Status:** proposal, fourth revision, 9 November 2026, against `main` at
> `bb6cc0c`. No implementation in this PR. Every factual claim names the
> code it was checked against; claims not yet measured are marked
> *to verify*.

## Decision in one paragraph

C code in Vela obtains heap memory only through a small family of checked
helpers that never return `NULL`. A failed allocation, or overflow in a size
computation, reports the caller's file and line and calls `abort()`. The
process then restarts through the recovery path it already has.

That decision rests on three conditions, and this document treats them as
part of the policy, not as background:

1. **Crash recovery fails closed in every state.** Today it does not. Three
   paths can reveal the desktop after a crash; see "The premise".
2. **Memory that external input can make grow has a budget.** The allocator
   bounds nothing by itself. A single size is bounded where it enters; an
   aggregate (a cache, a pool, a high-water mark) is bounded by its owner.
3. **Library failures that mean "out of memory" take the same fail-fast
   path.** Failures that have a safer local fallback (damage regions) use
   that fallback instead.

There is no fallible allocation tier, because no current site needs one (see
the audit).

## The premise: crash recovery must fail closed

Abort-on-failure is acceptable only if every crash leads to a safe state.
The supervisor restarts the compositor locked when the lock marker exists
(`supervisor.c` → `VELA_START_LOCKED` → `server.c`). The audit found three
ways in which a crash, from an allocation or from anything else, can end up
in an unlocked or stale screen. None of them is fixed in this phase; they
shape the design.

1. **Unchecked `setenv` after `fork()`.** The supervisor's restart child
   calls `setenv("VELA_START_LOCKED", "1", 1)` without checking the result,
   and the restarted compositor decides whether to lock **only** from that
   variable (`server.c`). If `setenv` fails with `ENOMEM`, which is exactly
   the condition after an allocation abort, the compositor starts unlocked.

2. **The lock's fatal path allocates, and runs precisely when the marker is
   missing.** `lock_fatal_shutdown` (`lock.c`) is the path taken when the
   marker cannot be persisted (`vela_lock_marker_create_at` fails) or when
   the locker's retry timer cannot be armed or cancelled. Its job is to black out every output and
   end the session, never to restart it. On the way it:
   - grows `fatal_waiting` with `vela_grow`;
   - creates a timer with `wl_event_loop_add_timer`;
   - renders blackout frames through the whole frame path, which allocates
     (point 7).

   If anything crashes there, the supervisor sees a crash, finds no marker,
   and restarts the compositor **unlocked**. The invariant the code
   documents ("end the entire session instead of ever allowing a restart
   without a marker") holds only as long as nothing in that path crashes.

3. **A failed damage computation leaves stale pixels.** No pixman region
   operation's result is checked anywhere in the compositor. In
   `scene/frame.c` alone there are 23 region operations, including
   `union_box`, which also implements `vela_output_frame_damage_whole`. On
   `ENOMEM`, pixman turns the region into an empty "broken" region. The next
   frame then sees no damage and redraws nothing. After the lock engages,
   that can leave desktop pixels on screen while the compositor believes it
   is showing the lock. This is unlikely, because these allocations are
   tiny, but it fails open.

### Design options for (1) and (2)

| Option | How | Fails closed? | Cost |
|---|---|---|---|
| a. Check every step | `_exit(127)` on any `setenv` failure; preallocate `fatal_waiting` for every output when outputs are added; give the fatal path a blackout that does not allocate | Only as long as every future change to those paths stays allocation-free | Fragile: the invariant depends on code review |
| b. Restart locked after **every** crash | The restarted compositor locks unless the supervisor positively passes "was unlocked" | Yes, by construction: any failure to communicate means locked | The user re-enters the password after every crash, even when the desktop was unlocked |
| c. **Lock state in shared memory** | The supervisor creates a one-page `memfd` before forking and passes its descriptor through the argv it already builds at startup, with no `setenv`. The compositor stores "locked" into the page with an atomic store **before** the lock becomes visible, and "unlocked" only after unlock. The supervisor reads the page after the child dies; the restarted compositor reads it at startup. The marker file stays as a second signal. | Yes: a store into an already mapped page cannot fail. A compositor that cannot map the page while supervised starts locked. | One shared page and a startup flag; the supervisor's existing "unsafe" exit-code protocol is kept |

**Recommendation: c**, plus the allocation-free fatal path from (a) as
defence in depth. Option c removes the dependency on `setenv` and on file
I/O at the moment it matters. Option b is the simplest fallback if c is
rejected.

### Design for (3)

Damage is a case where failing safe beats failing fast. A failed region
operation sets a "damage unknown" flag on the frame, and the next frame
redraws the whole output. Aborting would restart the session for a few
bytes, and ignoring the failure fails open; a full redraw is correct and
cheap. Every damage helper (`union_box` and the other region operations in
`scene/frame.c`) checks the `pixman_bool_t` result.

## Existing defects recorded during the audit

Documented here so the design accounts for them. They are not fixed in this
phase.

1. **A read error truncates the user's configuration.** `read_lines`
   (`compositor/src/config.c`) loops while `getline` returns `>= 0` and
   stops at the first `-1`. `getline` returns `-1` both at end of file and on
   error, including `ENOMEM` and I/O errors. `vela_config_write_checked`
   then writes the lines it collected back over `vela.conf` with an atomic
   rename. An error halfway through the file therefore permanently drops the
   rest of the user's settings.

   The same shape exists in two more places:
   - `output_config.c`: `vela_output_config_save` calls `read_file` and
     rewrites the file, so a truncated read loses the saved settings of
     other monitors;
   - `workspace.c`: `desktop.conf` is read at startup and later rewritten
     from memory, so a truncated read becomes permanent on the next save
     (rename a desktop, change a sticky app).

   **Fix:** every `getline` loop tells end of file apart from an error
   (point 3). A read that fails is reported to the caller, and nothing
   derived from it is written back. The remaining loops (`icons.c` ×2,
   `text.c`) only read; they get the same treatment so that an error is
   never mistaken for a short file.

2. **The icon cache grows without bound, keyed by client input.**
   `vela_icons_app` (`icons.c`) keeps a linked list of `cached_icon`
   entries keyed by `(app_id, size)`. It adds an entry even when no icon is
   found, never evicts, and searches linearly. A client that keeps changing
   its `app_id` grows the compositor's memory, and the cost of every lookup,
   for the whole session. Different output scales add entries for the same
   app.

3. **Upload staging keeps its high-water mark forever.** `stage`
   (`render/renderer.c`) creates a chunk of `max(size, 8 MiB)` when no
   existing chunk fits, and upload chunks are freed only when the renderer
   is destroyed (`destroy` in the same file). One very large client shm buffer therefore
   permanently raises the compositor's host-visible Vulkan memory. Readback
   staging already has the right model: a 128 MiB budget
   (`VELA_READBACK_MAX_BYTES`) with eviction of completed chunks
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
   - 5 of the 36 `wlr_*_create` results are checked on the following line.
     Some of those constructors can fail for reasons other than memory, so
     they need a per-call classification, not a blanket rule.

## Inventory at `bb6cc0c`

162 direct allocation calls in the in-scope C code (`compositor/`,
`bluetooth/`, `brightness/`, `capture/`):

| Shape | Count | Becomes |
|---|---|---|
| `calloc(1, sizeof(*x))`: one object | 72 | `VELA_NEW(type)` |
| `strdup` / `strndup` / `asprintf` | 43 | `vela_strdup` / `vela_strndup` / `vela_asprintf` |
| array or buffer with a computed size | 28 | `VELA_NEW_N(type, n)`, or a reused array (frame path) |
| `realloc` | 13 | `vela_grow` |
| `getline` (allocates through libc) | 6 | stays `getline`, with mandatory `ferror()` and `errno == ENOMEM` handling (see "`getline`" below) |

The first revision's search pattern missed `getline`. The pattern used for
the CI check (point 8) covers it, together with `getdelim`, `vasprintf`,
`open_memstream`, `realpath(…, NULL)`, `scandir`, `posix_memalign` and
`aligned_alloc`. A search on `bb6cc0c` found no uses of those others.

About 30 sites check the result today. They follow three conventions, none
of them tested:
- return `false`/`NULL` silently (`capture_watch.c`, `config.c`,
  `output_manager.c`, `path_security.c`, `render/pass.c`
  `vela_renderer_begin_pass`);
- log, then return (`render/allocator.c`);
- reply with `wl_client_post_no_memory` (`output_preview.c`).

Other defects:

- **Unchecked `p = realloc(p, n)`**, which loses the block and writes
  through `NULL`: `buffer.c` `reserve`, `config.c` `lines_push`, `icons.c`
  (lines 35, 104, 427) and `output_config.c`. The Bluetooth daemon's five
  `realloc` calls go through a temporary and are checked correctly.
- **`vela_buffer` overflows silently.** `buffer.c` `reserve` computes
  `length + more + 1` in `size_t` with no overflow check, and its capacity
  is `size_t`, not `int`.
- **`vela_grow` overflows silently.** It doubles an `int` capacity without
  a guard, multiplies `grown * size` unchecked, and returns `NULL` (no
  allocation) for `needed == 0` with an empty array.

## Audit: where sizes come from

Every site whose size is not a fixed `sizeof`, meaning the 28 computed-size
sites, the 43 string copies and the 6 `getline` loops, falls into one of
four classes:

| Class | Meaning | Policy |
|---|---|---|
| **Internal** | Derived from Vela's own state | Fail fast |
| **External, bounded by the transport** | Chosen by another party, but limited by a hard limit in the channel | Fail fast; the bound is documented and tested |
| **Same user** | Files and IPC from processes running as the same user | Fail fast; not a trust boundary, because such a process can already end the session |
| **External, unbounded** | Chosen by another party, with no hard limit | Must be capped where it enters (point 6) |

Results:

| Class | Sites | Bound |
|---|---|---|
| Internal | `command.c` `end_task`; `output_manager.c` (3); `snap.c`; `view.c`; `workspace.c` ordering; `icons.c` theme dirs; `render/pass.c` (4); `supervisor.c` argv; `brightness.c` and `capture_watch.c` published text; `path_security.c` components; `brightness/probe.c` sysfs names; `process.c` command copy | Counts of views, outputs, draws, waits, arguments |
| Internal, from the driver | `render/vulkan.c` (6); `render/allocator.c` modifiers | Vulkan and DRM enumeration counts |
| Internal, pixels chosen by the compositor | `decoration.c` (2); `icons.c` icon pixels; `render/pixel_buffer.c`; `text.c` | Scale × design size; text is ellipsised to `max_width`, which `decoration.c` only passes when `> 0` |
| External, Wayland | `output_preview.c` `preserve_position`; `decoration.c` title and app ID; `icons.c` cached app ID | libwayland's connection buffer. Vela does not call `wl_display_set_default_max_buffer_size`, so the default applies (4096 bytes, *to verify* with a test sending an oversized string) |
| External, X11 | Titles and classes of Xwayland windows, through wlroots | *To verify:* the length wlroots' XWM reads per property |
| External, system D-Bus | `bluetooth/agent.c` (5); `bluez.c`; `model.c` paths and values; `session.c`; `compositor/bluetooth.c` logind path | BlueZ and logind object paths are short; remote device names are capped at 248 bytes by the Bluetooth specification and by BlueZ (*to verify* for the other properties) |
| Same user | `config.c` (8); `icons.c` theme files; `output_config.c`; `workspace.c` names; `command.c` IPC lines (`MAX_LINE`); `shell.c` IPC line; `session.c` desktop names; every `getline` line | IPC is capped by `MAX_LINE`; file lines are not capped, but the files belong to the user |
| External, unbounded, host heap | — | none found |
| External, unbounded, **Vulkan memory** | Client buffer dimensions → `vkCreateImage` extent: `texture.c` `upload_buffer` (shm) and dmabuf import | **No check against the device's limits.** `upload_buffer` passes `buffer->width/height` straight to `vkCreateImage`; the dmabuf format probe in `vulkan.c` calls `vkGetPhysicalDeviceImageFormatProperties2` but ignores `maxExtent`. A shm buffer of 100000×1 needs only a 400 KB pool. An extent above the limit is *invalid usage* in Vulkan (undefined behaviour), not a clean error. |

Conclusion: **no host-heap site has an unbounded external size**, so none
needs a fallible path. The one unbounded external size found is a Vulkan
image extent. It is capped where it enters (point 6): a buffer larger than
the device's `maxImageDimension2D`, or the format's `maxExtent` for dmabuf,
is rejected before `vkCreateImage`. The revision-two claim that "no size is controlled by
another party" was wrong. Several sizes are chosen by clients; they are safe
for fail-fast only because the transport bounds them, so the bounds become
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
| Upload staging (`render/renderer.c`) | Largest simultaneous upload | None; high-water mark kept until destroy | Byte budget, with completed chunks above it freed, like readback |
| Readback staging | Screen capture requests | 128 MiB, with eviction (`make_readback_room`) | Already compliant: this is the model |
| Per-client protocol objects | Client requests | Freed when the client disconnects | Per-client quotas: shared with every wlroots compositor, out of scope here |
| IPC connections and queued topics (`command.c`) | Same-user processes | One queued line per topic per connection | Same user: not a trust boundary |
| Bluetooth model (`bluetooth/model.c`) | BlueZ objects, including nearby devices | BlueZ's own expiry of temporary devices | Mirrors BlueZ; *to verify* under a spoofed-device flood |

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
  bytes between the old and the new capacity. All 143 non-`realloc`,
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
after the loop:

```c
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
| `vela-compositor` | The supervisor restarts it and keeps the Wayland socket; Qt/KDE apps reconnect, others close. A locked session restarts locked, but today only while the lock marker exists and `setenv` succeeds; see "The premise". The budget is three crashes per minute, then the session ends. | `supervisor.c` restart loop (`WIFSIGNALED` covers `SIGABRT`); `tests/functional/test_session.py` `test_crash_while_locked_restarts_locked` and `test_gives_up_after_three_quick_crashes`, both using `SIGSEGV` |
| `vela-bluetooth` | Restarted by the compositor's generic child supervision. It stops after three consecutive exits that each happen within 5 s of start. | `process.c` `handle_exit` |
| `vela-brightness` | Restarted by its own `exited` handler, with its own limit | `brightness.c` |
| Supervisor | Only its startup allocations (argv) use the helpers. The shutdown path stays allocation-free, as #90 established. | `supervisor_children.c` |

### 5. After `fork()`

Fork sites in scope: `lock.c` `lock_start_launcher`, `process.c`
`vela_spawn` and `vela_child_start`, `session.c` `vela_session_run_hook`,
and `supervisor.c` `run_and_wait` and the restart loop.

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
  "The premise", item 1.
- **Rule:** after `fork()`, code calls none of the helpers. Any libc call
  that can fail is checked, and failure means `_exit(127)`.

### 6. Unbounded external sizes

None exists today in the host heap. One exists in Vulkan memory: client
buffer extents (see the audit), capped by the device limits at texture
import. Whenever one appears, the size is capped where it enters, and
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
`scene/frame.c` (23 calls, one checked across the compositor), and
wlroots' internal allocations during commit. On `ENOMEM`, pixman marks the
region as broken rather than crashing; the visible effect would be missed
damage. Checking those results belongs to the follow-up audit of library
results (point 9).

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

- **In scope:** the C code in `compositor/`, `bluetooth/`, `brightness/` and
  `capture/`.
- **Library results:** classified by point 2b. The full classification of
  the 36 `wlr_*_create` calls and of the FreeType, cairo, GBM and sd-bus
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
- **Contract:** the zero-size cases return non-`NULL` and can be freed.
  `vela_grow` zeroes the tail, doubles the capacity, preserves contents, and
  does nothing when `needed` is not larger than the capacity and the array
  exists. The `int` adapter aborts on a negative `needed` and on values
  beyond `INT_MAX`.
- **`vela_buffer`:** growth through the `size_t` core; an append whose
  length overflows aborts.
- **Configuration reads:** a fault-injection test (an `LD_PRELOAD` shim
  that makes `getline` fail with `ENOMEM` or `EIO` on the Nth line) checks
  that `vela.conf`, `outputs.conf` and `desktop.conf` are left unchanged.
- **Fail-closed recovery, as a matrix:** the compositor is killed with
  `SIGABRT` (and `SIGSEGV`) in each of these states, and none may ever
  produce an unlocked compositor or end in desktop pixels on screen:
  - unlocked;
  - locked;
  - while locking (between the shared-state store and the first lock
    frame);
  - inside `lock_fatal_shutdown` with the marker creation forced to fail;
  - with `setenv` forced to fail in the restart child.
- **Damage failure:** a region operation forced to fail (`LD_PRELOAD` shim
  on pixman) leads to a full redraw on the next frame. This is checked in
  the lock transition with a screen dump.
- **Budgets:** the icon cache stays within its entry budget under a client
  that cycles through distinct `app_id` values. Upload staging returns
  under its byte budget after one oversized shm buffer.
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
   - the shared lock-state page (option c, or the option chosen);
   - an allocation-free lock fatal path;
   - the damage fallback;
   - the configuration-read defects;
   - the recovery test matrix.
2. `common/alloc` with its tests; `vela_grow` with its two capacity types;
   `vela_buffer` on the `size_t` core with checked addition; the CI check
   with a full baseline.
3. The unchecked `realloc` sites (`buffer.c`, `config.c`, `icons.c`,
   `output_config.c`) and the `getline` rule.
4. Mechanical conversion of objects and strings, one subsystem per PR
   (render, scene, input/command, config/icons, daemons).
5. The frame path (point 7), measured against the existing benchmarks.
6. Classification of library results (point 2b) and `VELA_MUST`.
7. Budgets for the icon cache and upload staging.
8. Removal of the old fallback branches (`wl_client_post_no_memory` in
   `output_preview.c`, the silent returns), with a review of each for
   partial state. The baseline reaches zero.

## Options considered

| | Option | Outcome |
|---|---|---|
| A | First draft: fatal helpers plus `vela_try_*` wherever "a feature can cleanly fail" | Rejected on evidence: the audit found no unbounded external size. Point 6 keeps the door open for a concrete case. |
| B | Graceful `NULL` handling at every site | Hundreds of untested branches and half-built objects. Restart-based recovery already exists. |
| C | Interpose `malloc` globally (`--wrap`, `LD_PRELOAD`) in production | Would turn graceful library paths (libwayland, Mesa, FreeType) into aborts. In tests only, it is useful (see Tests). |
| D | Emergency reserve block freed on OOM | Useless under overcommit. Orderly shutdown already exists as the crash path. |
| E | **One fail-fast family, with bounded external sizes (this proposal)** | Matches the crash-only recovery Vela already has. Enforceable in CI. Tested in one place. |

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
  share the 128 MiB readback figure or have their own.
- Whether `vela_oom` should leave a marker so the supervisor can report
  "out of memory" instead of a generic crash. The `rss`/`available` fields
  may make that unnecessary.
- Whether the frame-path counter test can run in the existing headless
  fixture without GPU-specific flakiness.
