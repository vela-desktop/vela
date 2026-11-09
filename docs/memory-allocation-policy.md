# Host allocation policy — design draft

> **Status:** proposal, third revision, 9 November 2026, against `main` at
> `bb6cc0c`. No implementation in this PR. Every factual claim names the
> code it was checked against; claims not yet measured are marked
> *to verify*.

## Decision in one paragraph

C code in Vela obtains heap memory only through a small family of checked
helpers that never return `NULL`. A failed allocation, or overflow in a size
computation, reports the caller's file and line and calls `abort()`. The
process then restarts through the recovery path it already has. Sizes that
come from outside the process must be bounded at the point where they enter;
the allocator never decides what a client is allowed to request. There is no
fallible allocation tier, because no current site needs one (see the audit).
Two existing bugs found during the audit (a configuration file truncated
after a read error, and a locked session that can restart unlocked) are fixed
first, as separate PRs.

## Prerequisite fixes found during the audit

These two bugs exist today, independently of the policy. Each one gets its
own PR before any allocator change.

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

2. **A locked session can restart unlocked.** After `fork()`, the
   supervisor's child (`compositor/src/supervisor.c`, the restart loop)
   calls `setenv("VELA_START_LOCKED", "1", 1)` without checking its result.
   The restarted compositor decides whether to start locked **only** from
   that variable (`server.c`, `getenv("VELA_START_LOCKED")`). If `setenv`
   fails (`ENOMEM`, which is exactly the situation after an allocation
   abort), the compositor starts unlocked. The same child also calls
   `setenv` for the socket and runtime-directory descriptors without
   checking.

   **Fix:** every `setenv` in that child checks its result and calls
   `_exit(127)` on failure. That counts as a crash, so the marker stays and
   the next attempt starts locked. After the crash budget is spent, the
   session ends; it never opens unlocked.

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
| External, unbounded | — | none found |

Conclusion: **no current site has an unbounded external size**, so none
needs a fallible path. The revision-two claim that "no size is controlled by
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

Out of scope here, and noted so it is not forgotten: a client can still
create an unbounded **number** of small objects. That is a per-client quota
question, shared with every wlroots compositor, and not an allocation-size
question.

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
  `vela: out of memory (3 x 4096 bytes) at compositor/src/render/pass.c:616`.
  It uses no stdio, no `wlr_log` and no heap, then calls `abort()`.
- `vela_alloc_misuse` handles a negative `needed` and similar programming
  errors, with its own message.
- The session redirects stderr to `vela.log` (`session/vela-session.in`).
  A core dump through systemd-coredump depends on the system's configuration
  and is not part of the contract.

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
| `vela-compositor` | The supervisor restarts it and keeps the Wayland socket; Qt/KDE apps reconnect, others close. A locked session restarts locked, after prerequisite fix 2. The budget is three crashes per minute, then the session ends. | `supervisor.c` restart loop (`WIFSIGNALED` covers `SIGABRT`); `tests/functional/test_session.py` `test_crash_while_locked_restarts_locked` and `test_gives_up_after_three_quick_crashes`, both using `SIGSEGV` |
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
  prerequisite fix 2.
- **Rule:** after `fork()`, code calls none of the helpers. Any libc call
  that can fail is checked, and failure means `_exit(127)`.

### 6. Unbounded external sizes

None exists today. When one appears, the size is capped where it enters, and
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
- **Follow-up, with its own inventory:** `NULL` and error results from
  library constructors and operations (`wlr_*_create`, pixman, FreeType,
  cairo, GBM, sd-bus). They are checked where they are called and never
  dereferenced unchecked.
- **Vulkan** `VK_ERROR_OUT_OF_HOST_MEMORY` / `VK_ERROR_OUT_OF_DEVICE_MEMORY`
  stay under [vulkan-device-loss.md](vulkan-device-loss.md).
- **C++/Qt code** is out of scope: `new` throws, and Qt has its own policy.

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
- **Prerequisite fix 1:** a fault-injection test (an `LD_PRELOAD` shim that
  makes `getline` fail with `ENOMEM` or `EIO` on the Nth line) checks that
  `vela.conf`, `outputs.conf` and `desktop.conf` are left unchanged.
- **Prerequisite fix 2:** a `setenv` failure in the restart child (forced
  with an `LD_PRELOAD` shim in the functional test) never yields an
  unlocked compositor.
- **Recovery with `SIGABRT`:** a variant of
  `test_crash_while_locked_restarts_locked` that sends `SIGABRT` instead of
  `SIGSEGV`.
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

1. Prerequisite fixes 1 and 2, as two separate PRs with their tests.
2. `common/alloc` with its tests; `vela_grow` with its two capacity types;
   `vela_buffer` on the `size_t` core with checked addition; the CI check
   with a full baseline.
3. The unchecked `realloc` sites (`buffer.c`, `config.c`, `icons.c`,
   `output_config.c`) and the `getline` rule.
4. Mechanical conversion of objects and strings, one subsystem per PR
   (render, scene, input/command, config/icons, daemons).
5. The frame path (point 7), measured against the existing benchmarks.
6. Removal of the old fallback branches (`wl_client_post_no_memory` in
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

- Whether `vela_oom` should leave a marker so the supervisor can report
  "out of memory" instead of a generic crash.
- Whether the frame-path counter test can run in the existing headless
  fixture without GPU-specific flakiness.
