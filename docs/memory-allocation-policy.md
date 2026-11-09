# Host allocation policy — design draft

> **Status:** proposal, revised 9 November 2026 against `main` at `bb6cc0c`.
> No implementation in this PR. Items marked *to verify* need a measurement
> before they become policy.

## Decision in one paragraph

C code in Vela has exactly one way to obtain heap memory: a small family of
checked helpers that never return `NULL`. Failure, including size
arithmetic overflow, prints the call site and calls `abort()`, and the
process restarts through the crash-recovery path it already has. There is
no fallible tier, because the audit below found no current allocation whose
size an untrusted party controls. Growth goes through one function. The
frame path stops allocating. A CI check keeps raw allocator calls from
coming back.

## Inventory at `bb6cc0c`

156 direct allocation calls in the in-scope C code (`compositor/`,
`bluetooth/`, `brightness/`, `capture/`):

| Shape | Count | Becomes |
|---|---|---|
| `calloc(1, sizeof(*x))`: one object | 72 | `VELA_NEW(type)` |
| `strdup` / `strndup` / `asprintf` | 43 | `vela_strdup` / `vela_strndup` / `vela_asprintf` |
| array or buffer with a computed size | 28 | `VELA_NEW_N(type, n)`, or a scratch array (frame path) |
| `realloc` | 13 | `vela_grow` |

About 30 sites check the result today. They follow three conventions:
- return `false`/`NULL` silently (`capture_watch.c`, `config.c`,
  `output_manager.c`, `path_security.c`);
- log, then return (`render/allocator.c`);
- reply with `wl_client_post_no_memory` (`output_preview.c`).

None of these paths is tested. The rest dereference the result directly.

Defects that exist regardless of policy:

- **`p = realloc(p, n)` without a check** loses the old block and writes
  through `NULL`: `buffer.c` (shared text buffer), `config.c` (`lines_push`),
  `icons.c` (three lists) and `output_config.c`. Each one duplicates
  `vela_grow`.
- **`vela_grow`** doubles an `int` capacity without an overflow guard and
  computes `grown * size` without checked multiplication.
- **`render/pass.c` allocates and frees four arrays on every pass
  submission:** `foreign_textures`, the `acquire`/`release` barriers in
  `record`, and `waits` in `vela_pass_submit`. None of them is checked.

## Audit: where sizes come from

The 28 computed-size sites are the only places where a fallible allocation
could be justified. Each one was traced to the source of its size:

| Size source | Sites |
|---|---|
| Internal counts (views, outputs, draws, waits, argc, theme dirs) | `command.c` `end_task`; `output_manager.c` (3); `snap.c`; `view.c`; `workspace.c` (2); `icons.c` theme dirs; `render/pass.c` (4); `supervisor.c` argv |
| Driver enumerations (Vulkan, DRM formats) | `render/vulkan.c` (6); `render/allocator.c` modifiers |
| Pixel sizes the compositor chooses (scale × design size) | `decoration.c` (2); `icons.c` icon pixels; `render/pixel_buffer.c` |
| Text, bounded by `max_width` (ellipsised) | `text.c` |
| Copies of strings already in memory | `shell.c` IPC line; `session.c` desktop names; `workspace.c` name |

Untrusted inputs that do carry sizes are handled elsewhere:
- **Icon files:** PNG is decoded by cairo and SVG by librsvg, which allocate
  internally and report failure through a status code that the code already
  checks.
- **Wayland strings** such as titles and app IDs are bounded by
  libwayland's message size and arrive already in memory.
- **Client pixel buffers** are shm mappings owned by the client, or dmabufs
  imported into Vulkan, which fall under the Vulkan policy.
- **The command socket** is bounded by `MAX_LINE`.

**Result: no current site has a size controlled by an untrusted party, so
no site qualifies for a fallible path.** The first draft's `vela_try_*`
tier would be infrastructure for zero callers, and fault-injection tests for
code that never runs.

## Policy

### 1. The helpers (`common/alloc.{h,c}`)

```c
// Zeroed memory for `count` elements of `size` bytes. Never NULL.
void *vela_alloc(size_t count, size_t size);
#define VELA_NEW(type) ((type *)vela_alloc(1, sizeof(type)))
#define VELA_NEW_N(type, n) ((type *)vela_alloc((size_t)(n), sizeof(type)))

// Grows `items` to at least `needed` elements (capacity doubles) and zeroes
// the new tail. Never NULL.
void *vela_grow(void *items, int *capacity, int needed, size_t size);

char *vela_strdup(const char *s);
char *vela_strndup(const char *s, size_t n);
char *vela_asprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
```

- **One way to allocate, one way to grow.** There is no public
  `vela_realloc`. Every resize knows its capacity, and only `vela_grow` can
  zero the new tail correctly. `vela_buffer` moves onto `vela_grow` with an
  element size of 1.
- **Always zeroed.** 139 of the 156 sites already zero their memory or fill
  every byte (`calloc`, string copies, `malloc` followed by a full `memcpy`). Large zeroed blocks come from `mmap` and cost nothing extra.
  The remaining `malloc` sites are on paths where zeroing is cheap next to
  the work around them. The one hot path stops allocating instead (point 4).
- **Never `NULL`.** `count == 0` or `size == 0` allocates one byte, so a
  `NULL` result never means "empty". Callers may therefore drop the
  `count ? count : 1` workarounds.
- **Overflow** in `count * size`, in capacity doubling, or in `needed`
  beyond `INT_MAX / 2` is treated exactly like allocation failure.
- `vela_grow`'s capacity stays `int`, because every caller counts in `int`.
  The guard makes the limit explicit.
- The macros take the type, so a size mismatch with the pointer is a
  compile-time warning, not a silent bug.

### 2. Failure: `vela_oom`

`noreturn void vela_oom(size_t count, size_t size, const char *file, int line)`:
- writes one line to stderr with `write(2)`, for example
  `vela: out of memory (3 x 4096 bytes) at render/pass.c:616`;
- uses no stdio, no `wlr_log` and no heap;
- calls `abort()`.

Every public name is a thin macro that passes `__FILE__`/`__LINE__` down to
`vela_oom`. The session already redirects stderr to `vela.log`, and
`SIGABRT` produces a core dump through systemd-coredump.

### 3. Recovery is per process, and every path already exists

| Process | After `abort()` |
|---|---|
| `vela-compositor` | The supervisor restarts it and keeps the Wayland socket. Qt/KDE apps reconnect; others close. A locked session restarts locked. Budget: three failures per minute, then the session ends. |
| `vela-bluetooth`, `vela-brightness` | Restarted by the compositor's child supervision (`vela_child_*`), which stops restarting after three exits within 5 s of start. |
| Supervisor | Only its startup allocations (argv) use the helpers. Its critical shutdown path stays allocation-free, as #90 established. |
| Child after `fork()` | Does not allocate. It only calls `exec*` or `_exit`. |

### 4. The frame path does not allocate

Scene diffing, pass building, recording and submission make no host
allocations in steady state. The four per-pass arrays in `render/pass.c`
become renderer-owned scratch arrays that grow with `vela_grow` and reset
for each pass, like `pass->draws` and the wait arrays already do. Any
frame-time effect is measured with the existing benchmarks, not assumed.

### 5. Checked sites become fail-fast

Every existing `if (!p)` after one of our own allocations is removed
together with its fallback. This includes `wl_client_post_no_memory` in
`output_preview.c`: under this policy the allocation cannot return `NULL`,
so that branch is dead. Each conversion is reviewed for partial state that
the old early-return protected. For example, `output_manager.c` allocates
before it changes any output, so aborting there leaves nothing
half-applied.

### 6. If a fallible case ever appears

If a future site has a size controlled by an untrusted party:
1. **Cap first.** Validate the size where it enters (protocol request, file,
   D-Bus, IPC), against a documented maximum, and reject it in that
   boundary's terms: a protocol error, a declined command, a default value.
2. **Only if a capped size can still legitimately fail** (for example a
   user-requested multi-gigabyte buffer), revise this policy with that
   concrete case: a fallible helper, its test, and a CI allowlist entry.
   Not before.

### 7. Scope

- **In scope:** the C code in `compositor/`, `bluetooth/`, `brightness/` and
  `capture/`.
- **Not covered by these helpers, but in the same spirit:** `NULL` from
  third-party constructors (`wlr_*_create`, FreeType, cairo, GBM, sd-bus).
  Their results must be checked at the call site and never dereferenced.
  These sites are not audited yet; that is a follow-up with its own
  inventory.
- **Vulkan** `VK_ERROR_OUT_OF_HOST_MEMORY` / `VK_ERROR_OUT_OF_DEVICE_MEMORY`
  stay under [vulkan-device-loss.md](vulkan-device-loss.md).
- **C++/Qt code** is out of scope: `new` throws, and Qt has its own policy.

### 8. Enforcement

A CI check rejects `malloc`, `calloc`, `realloc`, `reallocarray`, `strdup`,
`strndup` and `asprintf` in the in-scope C sources, except in
`common/alloc.c`. During migration, a baseline of legacy sites lives in
`common/alloc-legacy-sites.txt` (outside `docs/`, because CI reads it). The
check fails on any addition, and the baseline only shrinks, down to empty.

## Tests

The payoff of having no fallible tier is that tests concentrate on the
helpers instead of on hundreds of call sites:
- **Death tests:** `vela_oom` aborts with size, file and line, for real
  failure, `count * size` overflow and `vela_grow` capacity overflow.
- **`vela_grow`:** the tail is zeroed, the capacity doubles, a smaller
  `needed` is a no-op, and existing elements are preserved.
- **Zero-size requests:** they return a non-`NULL`, freeable pointer.
- **Frame path:** an allocation counter (test-build hook in `alloc.c`)
  stays flat across N frames of a steady scene.
- **Crash path:** an injected allocation failure while locked causes the
  supervisor to restart the compositor locked. *To verify* whether an
  existing lock test already covers abort rather than a clean exit.
- ASan/UBSan, plus the CPU and GPU suites touched by each step.

## Migration order

1. `common/alloc` with its tests, `vela_grow` rebuilt on top of it, and the
   CI check with a full baseline.
2. The unchecked `realloc` sites (real bugs): `buffer.c`, `config.c`,
   `icons.c` and `output_config.c`. The Bluetooth daemon's `realloc` calls
   are already checked; they move to `vela_grow` in step 3.
3. Mechanical conversion of the 72 objects and 43 strings, one subsystem
   per PR (render, scene, input/command, config/icons, daemons).
4. The 28 computed-size sites, with the frame-path scratch arrays measured
   against the benchmarks.
5. Removal of the old fallback branches (point 5), with the baseline at
   zero.

## Options considered

| | Option | Outcome |
|---|---|---|
| A | First draft: fatal helpers plus `vela_try_*` wherever "a feature can cleanly fail" | Rejected on evidence: the audit found no candidate site, and the classification was a judgement call that drifts. Point 6 keeps the door open, tied to a concrete case. |
| B | Graceful `NULL` handling at every site | Hundreds of untested branches and half-built objects, with no benefit, because recovery by restart already exists. |
| C | Interpose `malloc` globally (`--wrap`, `LD_PRELOAD`) | Would turn graceful library paths (libwayland, Mesa, FreeType) into aborts. Fragile across toolchains. |
| D | Emergency reserve block, freed on OOM for an orderly shutdown | Useless under overcommit. Orderly shutdown already exists as the crash path. |
| E | **One fail-fast family (this proposal)** | Matches the crash-only recovery Vela already has. Enforceable by grep. Tested in one place. |

## Related, separate work: exhaustion at session level

With `vm.overcommit_memory=0`, ordinary allocations rarely fail. Real
exhaustion is handled by the kernel OOM killer or by systemd-oomd, which
kills a whole cgroup. Supervisor, compositor, shell and every application
currently share logind's `session-N.scope`:
- `vela-session` `exec`s the supervisor;
- the shell launches apps with `vela::process::startDetached`;
- the compositor launches the terminal with `vela_spawn`.

*Expected, to verify in a disposable VM:* oomd then ends the whole session
instead of the application that caused the pressure. Plasma and GNOME avoid
this with one transient `app-*.scope` per application. This is independent
of the allocator and gets its own PR and document. It is recorded here only
because it is the memory failure users would actually see.

## Open questions

- Whether `vela_oom` should leave a marker so the supervisor can say "out
  of memory" instead of a generic crash. The log line and the core dump may
  be enough.
- Whether the frame-path counter test can run in the existing headless
  fixture without GPU-specific flakiness.
