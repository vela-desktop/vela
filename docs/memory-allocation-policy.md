# Memory exhaustion and host allocation policy — design draft

> **Status:** proposal, revised 9 November 2026 against `main` at `bb6cc0c`.
> No implementation in this PR. Sections marked *to verify* are hypotheses
> that need a measurement before they become policy.

This document answers two questions that the first draft treated as one:

1. What should C code do when a host allocation returns `NULL`?
2. What happens to a Vela session when the machine actually runs out of
   memory?

The second question decides how much the first one matters. On a default
Linux desktop the answer to (2) is rarely "malloc returns NULL".

## What actually happens on Linux

- **Overcommit.** With `vm.overcommit_memory=0` (the default, also on the
  development machine) ordinary small allocations do not fail: pages are
  committed lazily and the kernel OOM killer, or systemd-oomd before it, acts
  first. In practice `malloc`/`calloc` return `NULL` in four cases:
  - one request larger than the heuristic allows (RAM + swap), which almost
    always means an arithmetic overflow or a size taken from external data;
  - `RLIMIT_AS` or `RLIMIT_DATA` set on the session;
  - strict accounting (`vm.overcommit_memory=2`), which is rare on desktops;
  - fault injection in tests.
- **systemd-oomd** is active on the default Arch/CachyOS/Fedora setups. On
  the development machine it monitors `/user.slice/user-1000.slice` (memory
  pressure 80 % for 30 s, swap 90 %). When a limit is exceeded it picks a
  **descendant cgroup** and kills every process in it.
- **The Vela session is one cgroup.** `session/vela-session` `exec`s
  `vela-compositor --supervise` inside logind's `session-N.scope`. The shell
  launches applications with `vela::process::startDetached` (QProcess), and
  the compositor launches the terminal with `vela_spawn`, so both stay in
  that same scope. Supervisor, compositor, shell and every application share
  one cgroup. *Expected, to verify:* under sustained memory pressure, oomd
  kills `session-N.scope`, which ends the whole session and loses unsaved
  work in every app. Plasma and GNOME avoid this by putting each application
  in its own transient `app-*.scope`, so oomd kills only the application that
  caused the pressure.
- **Kernel OOM killer.** It scores processes by RSS + swap + page tables.
  Imported GPU buffers are mostly not counted as RSS, so the compositor is an
  unlikely victim; browsers are likely ones. If the compositor is killed, the
  supervisor treats it as a crash.
- **Supervisor restart is the existing crash-recovery path.** A compositor
  crash (any signal, including `SIGABRT`) is restarted with the Wayland
  socket kept. Qt/KDE apps reconnect; other clients close. A locked session
  persists its marker before exposing the locked state, and restarts locked.
  The crash budget is three failures per minute; after that the session ends.

Consequences:

- An explicit abort on a failed host allocation is not a catastrophe. It
  takes the same path as any other crash, and that path is already designed
  and tested.
- The realistic way to get `NULL` is an oversized request. Oversized requests
  must be prevented at the place where the size comes from, not handled
  where the allocation happens.
- The biggest user-visible memory-exhaustion risk is oomd ending the whole
  session. No allocation helper can address that.

## Current inconsistencies (at `bb6cc0c`)

- **About 125 direct allocation calls in `compositor/src`** (`malloc`,
  `calloc`, `realloc`, `strdup`, `strndup`, `asprintf`). About 95 do not check
  the result. Examples: `command.c` (`handle_connection`,
  `vela_commands_listen`, `end_task`), `input.c` (`add_pointer` and three
  more creation functions), `render/renderer.c` (`vela_renderer_create`), and
  every `scene/` node constructor. `bluetooth/src` has 16 unchecked calls out
  of 27; `brightness/src` has 1.
- **Unchecked self-assignment of `realloc`** (`p = realloc(p, n)`), which
  loses the old pointer and then writes through `NULL`: `buffer.c` (shared
  text buffer), `config.c` (`lines_push`), `icons.c` (three lists) and
  `output_config.c`. All of these duplicate `vela_grow`.
- **`vela_grow` itself** aborts on failure, as #90 relies on. It also:
  - doubles an `int` capacity without an overflow guard;
  - computes `grown * size` without checked multiplication.
- **About 30 checked call sites follow at least three conventions:**
  - return `false`/`NULL` silently (`capture_watch.c`, `config.c`,
    `output_manager.c`);
  - log, then return (`render/allocator.c`);
  - reply to the client with `wl_client_post_no_memory` (`output_preview.c`).

  None of these paths is covered by a test.
- **Allocation in the frame path.** `render/pass.c` allocates and frees four
  arrays for every pass submission: `foreign_textures`, the `acquire` and
  `release` barriers in `record`, and `waits` in `vela_pass_submit`. None of
  them is checked. The renderer already keeps reusable arrays for other
  per-frame data (`vela_grow` on `pass->draws`, `r->staging`, wait arrays).
- PR #90 is consistent with the fail-fast direction: its wait arrays use
  `vela_grow`, and it made the supervisor's shutdown enumeration
  allocation-free. Both behaviours must be kept.

## Options considered

| | Option | For | Against |
|---|---|---|---|
| A | First draft: fatal helpers, plus `vela_try_*` wherever "a feature can cleanly fail" | Keeps graceful degradation where it is cheap | "Can cleanly fail" is a judgement call per site, so the classification drifts. Fallible paths are untested code that almost never runs. |
| B | Abort everywhere through one helper | Simple, enforceable, matches crash-only recovery | A legitimately large allocation (screenshot of a large output, a preview) aborts the compositor. Any size a client controls becomes a way to crash the compositor unless it is capped first. |
| C | Check `NULL` and degrade gracefully at every site | Fails softly in theory | Hundreds of untested error paths, half-initialised objects, and no real benefit under overcommit |
| D | Interpose `malloc` globally (`--wrap`, `LD_PRELOAD`) so every allocation aborts | Covers libraries too | Turns graceful library paths (libwayland's `post_no_memory`, Mesa, FreeType) into aborts. Fragile across toolchains. |
| E | Emergency reserve block freed on OOM, then orderly shutdown | Used by some interpreters | Pointless under overcommit. Orderly shutdown already exists as the crash path. |

None of these addresses the oomd risk. The proposal below takes B as the
default, keeps a narrow and mechanically defined part of A, and adds the
system-level layer.

## Proposal

### 1. Session layout: keep the session alive under real exhaustion

*Separate PR. It has the most user impact, but it is not allocator work.*

- Launch every application in its own transient scope under the user
  manager's `app.slice` (`app-vela-<app-id>-<n>.scope`, created with
  `StartTransientUnit` on the systemd user manager over D-Bus, as Plasma and
  GNOME do). This covers both the shell (`vela::process::startDetached`) and
  the compositor (`vela_spawn`).
- When no systemd user manager is available, keep launching directly. The
  behaviour is the same as today.
- Leave supervisor, compositor and shell in the session scope. *To verify:*
  whether `ManagedOOMPreference=avoid` can be honoured for a user-owned
  session scope, and whether that is still needed once applications are
  separate.
- `common/caller` sandbox detection reads `snap.*` cgroup names and Flatpak
  markers. It is not affected by `app-vela-*` scopes.

### 2. Trust boundaries: cap external sizes before allocating

Every size or count derived from outside the process has an explicit,
documented maximum and is rejected where it enters. External sources are
Wayland or X11 requests, files (icons, config, theme), D-Bus, IPC commands
and environment variables. A rejection takes the form the boundary already
uses:
- a protocol error for the offending client;
- a declined command;
- falling back to a default value.

This is what keeps clients from crashing the compositor. It is not an
allocator responsibility. Some sizes are already bounded, for example text
rendering, which ellipsises to `max_width`. An audit lists the rest.

### 3. Default: one fail-fast allocation family

A new `common/alloc.{h,c}` is shared by the compositor and the C daemons:

```c
void *vela_alloc(size_t size);                          // zeroed
void *vela_alloc_array(size_t count, size_t size);      // zeroed, overflow-checked
void *vela_realloc_array(void *p, size_t count, size_t size);
char *vela_strdup(const char *s);
char *vela_strndup(const char *s, size_t n);
char *vela_asprintf(const char *fmt, ...);
#define VELA_NEW(type) ((type *)vela_alloc(sizeof(type)))
#define VELA_NEW_ARRAY(type, n) ((type *)vela_alloc_array((n), sizeof(type)))
```

- All of these functions zero memory. Nearly every current call site is
  `calloc`, and zeroing removes a class of uninitialised-field bugs. The
  measured exception is the frame path, which must not allocate at all
  (point 5).
- Failure handling:
  - Failure, including multiplication overflow, calls
    `noreturn vela_oom(size, file, line)`.
  - `vela_oom` writes one line with `write(2)` to stderr, which the session
    redirects to `vela.log`. It does not use stdio, `wlr_log` or any other
    allocation.
  - It then calls `abort()`, which produces a core dump through
    systemd-coredump and a supervisor restart.
  - The macros record the call site.
- Zero-size requests return a unique non-`NULL` pointer
  (`vela_alloc(0) == vela_alloc(1)`), so `NULL` is never a valid result.
- `vela_grow` is rebuilt on `vela_realloc_array` with a capacity overflow
  guard. The hand-written `realloc` loops move onto it. Its capacity stays
  `int` for now, because every caller uses `int` counts.

### 4. The only exception: a closed rule for fallible allocation

`vela_try_alloc` / `vela_try_alloc_array` may be used **only if both**
conditions hold:

1. the size scales with external data (pixels, file content, a count chosen
   by a client), **and**
2. the result serves one optional operation that already has a failure path
   visible to the user or client: a screenshot, a preview, an icon, a
   thumbnail.

Structural allocations always use point 3. These are fixed-size objects,
scene nodes, per-output and per-device state, and anything whose loss
leaves the compositor in a half-built state. This includes protocol objects
created on a client's request: under the default policy,
`wl_client_post_no_memory` for a 200-byte struct is dead code.

Every `vela_try_*` call site is listed in `common/alloc-fallible-sites.txt`
(outside `docs/`, because CI reads it) with the test that injects its failure.
CI rejects unlisted uses. Fewer than
ten sites are expected.

### 5. Fewer allocations in hot paths

The frame path (scene diff, pass building, recording, submission) makes no
host allocations in steady state. Per-pass temporary arrays become
renderer-owned scratch arrays grown with `vela_grow` and reset per pass.
This removes four allocation pairs per pass (see the inventory above) and
their unchecked failure points. It may also reduce frame-time jitter, which
must be measured with the existing benchmarks before it is claimed.

### 6. Scope and explicit non-goals

- **In scope:** C code in `compositor/`, `bluetooth/`, `brightness/` and
  `capture/`.
- **Supervisor critical paths and code after `fork()` stay allocation-free**,
  as #90 established. They must not call these helpers.
- **Third-party constructors** (`wlr_*_create`, FreeType, pixman, GBM) keep
  their own contracts. A `NULL` result is checked and handled at the call
  site, never dereferenced. Whether that site aborts or declines follows
  points 3 and 4.
- **Vulkan** `VK_ERROR_OUT_OF_HOST_MEMORY` / `VK_ERROR_OUT_OF_DEVICE_MEMORY`
  stay under `docs/vulkan-device-loss.md`.
- **C++/Qt code** (shell, Settings, daemons in C++) is out of scope: `new`
  already throws and Qt's own policy applies.
- **No promise to keep rendering** under system-wide exhaustion. The goal is
  that the session survives an application that exhausts memory (point 1),
  and that the compositor fails deterministically and diagnosably otherwise.

### 7. Enforcement

A CI check (alongside the existing quality checks) rejects direct `malloc`,
`calloc`, `realloc`, `reallocarray`, `strdup`, `strndup` and `asprintf` in the
in-scope C sources, except in `common/alloc.c`. During migration, a baseline
file next to it (`common/alloc-legacy-sites.txt`) lists the remaining legacy
sites. The check fails on additions, and the
list only shrinks. The fallible-site allowlist from point 4 is checked in the
same job.

## Tests

- **Death test.** `vela_oom` aborts and prints size, file and line, for real
  failure, multiplication overflow and `vela_grow` capacity overflow.
- **Fault injection.** A test-build hook in `alloc.c` fails the Nth
  allocation. Each fallible site from point 4 has a test that injects its
  failure and checks a clean decline.
- **Crash-path regression.** Inject an allocation failure while the session
  is locked, and check that the supervisor restarts the compositor locked.
  *To verify* whether an existing lock test already covers an abort rather
  than a clean exit.
- **oomd experiment.** In a disposable VM or a headless session, never on the
  daily-use desktop, run a memory hog launched from the shell. Compare which
  cgroup oomd kills before and after point 1.
- ASan/UBSan, plus the CPU and GPU suites affected by each migration step.

## Migration order

1. Update this policy and get sign-off (this PR).
2. Add `common/alloc` with its tests, rebuild `vela_grow`, add the CI check
   with the legacy baseline.
3. Migrate by subsystem, shrinking the baseline in each step: render, scene,
   input/command/IPC, config/icons, daemons.
4. Convert frame-path temporaries to scratch arrays (point 5), with
   benchmarks.
5. Separately: per-application scopes (point 1), with the oomd experiment.

## Open questions

- The cap values for each external size (point 2). They need an audit of each
  boundary.
- Whether `vela_oom` should leave a marker so the supervisor can report
  "out of memory" instead of a generic crash. The core dump and the log line
  may be enough.
- The feasibility of `ManagedOOMPreference` for the session scope (point 1).
