# CI lanes: native and Qt

> **Status:** Accepted. **Checked:** 26 November 2026, `889acefa`.
>
> **Remaining:** stage 3, Memcheck per lane, after #324; see [Stage 3](#stage-3-memcheck-per-lane-after-324).

## Decision in one paragraph

The CPU checks in CI treat Vela's code as two families with different tools,
costs and failure profiles: **native** code (no Qt) and **Qt** code. A
script assigns every test and every first-party translation unit to one
lane, mechanically, by whether it is linked or compiled against Qt. Every
analyser then covers all the code of its lane. Before this plan, cppcheck and
clang-tidy read 20 of 353 C and C++ files, Clazy did not run, and qmllint
could not resolve most of Vela's own QML types. Findings that existed when an
analyser was extended are held to a per-check **ceiling** that can only come
down: a check with more findings than its ceiling fails CI. The ceiling holds a
count, not a list, so a change that fixes one finding of a check and adds
another passes; see [Ceilings](#ceilings). The aim is coverage and
attribution, not speed; see [Wall time](#wall-time).

## The lanes

Counts are at `889acefa`, from a full Debug build with the GPU test tools.

| Lane | Tests | Translation units |
|---|---:|---:|
| Native | 369 | 223 (177 C, 46 C++) |
| Qt | 80 | 132 (130 C++, 2 C also compiled natively) |
| Scripts | 11 | – |
| GPU (`gpu`, `rhi` labels; in no CPU lane) | 28 | – |

### Rule for tests

[`scripts/ci-lanes.py`](../scripts/ci-lanes.py) reads
`ctest --show-only=json-v1`. For each test, it looks at every command
argument that is an ELF file inside the build tree, wrappers included
(`dbus-run-session -- <test>`, `tests/audio/fixture.py -- <test>`). It
follows `DT_NEEDED` through the build tree's shared libraries.

- **GPU**: the test has the `gpu` or `rhi` label.
- **Qt**: one of those programs needs a `libQt6*` library.
- **Native**: there is such a program, and none needs Qt.
- **Scripts**: the test runs no program of the build tree.

The rule follows linkage, not directories: `vela-bluetooth-daemon-test` links
QtDBus and is in the Qt lane; the polkit agent's GoogleTest suite is native.
Labels written by hand would drift: 456 of the 488 tests have none. With the
rule, a new test lands in its lane when it is built.

### Rule for translation units

An entry of `compile_commands.json` is first-party when its file is in the
source tree, outside the build directory and not under `*_autogen`. It is
**Qt** when its include paths hold Qt 6's headers, and **native** otherwise.

`ci-lanes.py check BUILD` fails when a test is in no lane or in two, when a
lane is empty, or when the CPU lanes together do not hold exactly what
`ctest -LE "gpu|rhi"` selects. With `--sources` it also fails when a tracked
`.c` or `.cpp` file is missing from the compilation database, because an
analyser would then never see it.

## Stage 1: sanitizers per lane (done)

`asan-ubsan` keeps one instrumented build and runs one ctest step per lane,
each with its own JUnit file. Every step runs even when an earlier one
failed, so a native failure does not hide the Qt results. Runner time stays
the same.

Summed test times under ASan/UBSan, from the JUnit report of the `main`
push of `889acefa` (run 45840870212):

| Lane | Tests | Time |
|---|---:|---:|
| Native | 368 | 41 s |
| Qt | 80 | 474 s |
| Scripts | 10 | 60 s |

## Stage 2: every analyser over all of its lane (done)

[`scripts/static-analysis.sh`](../scripts/static-analysis.sh) runs one lane
over a full Clang build, and the same command reproduces a job locally. In
the Static Analysis workflow:

- `static-analysis` runs the native lane and keeps the name the `Main
  protection` ruleset requires;
- `static-analysis-qt-part` runs the Qt lane in four parts on four runners:
  clang-tidy split in three shards (`--only clang-tidy --shard I/3`, every
  third file by name) and the other analysers (`--only tools`);
- `static-analysis-qt` gathers the parts' logs and compares the whole lane
  with its ceilings. It fails if a shard's log is missing.

| Analyser | Native lane | Qt lane |
|---|---|---|
| Clang Static Analyzer, through clang-tidy | scan-build's default checkers, plus `bugprone-sizeof-expression` | the same, without `cplusplus.NewDeleteLeaks` |
| cppcheck (`--enable=warning`) | every translation unit | every translation unit, with `--library=qt` |
| Clazy (`level0`, `level1`) | – | every translation unit, without `connect-not-normalized` |
| qmllint (`all_qmllint`) | – | every QML module |

The exceptions, and why:

- **`cplusplus.NewDeleteLeaks` in Qt code.** A QObject's parent deletes it,
  which the analyzer cannot see. All 8 of its findings were parented
  `QDBusPendingCallWatcher`s and similar. Leaks at run time are
  LeakSanitizer's job.
- **Clazy's `connect-not-normalized`.** clang-format puts back the spaces in
  `SLOT(f(A, B))`, and Qt normalizes the signature when it connects.
- **cppcheck and system headers.** cppcheck reads each file with the
  definitions and project include paths of its compile command, but not
  system headers.
  [`scripts/cppcheck/vela.cfg`](../scripts/cppcheck/vela.cfg) spells out the
  system macros that change control flow: Wayland's list macros, Vulkan's
  calling-convention macros and Qt Test's checks. Without them, cppcheck
  reported 129 uninitialized variables in `wl_list_for_each` loops. In the
  Qt lane, the script passes Qt's moc revision; without it, 59 files that
  include their `.moc` stopped at an `#error` before analysis. cppcheck
  messages that mean a file was not analysed (`syntaxError`,
  `preprocessorErrorDirective`, `unknownMacro` and the internal errors)
  count wherever they point, generated files included.
- **Tests.** cppcheck's `assertWithSideEffect`, `assignmentInAssert` and
  `nullPointerOutOfMemory` are off under `tests/`: Vela's tests `#undef NDEBUG`
  so their asserts always run, and a test that cannot allocate may crash.

### What extending the analysers found

Fixed in this change:

- **The analyzer.** Two `getenv` calls for one value in `vela-testlock`.
  `unlinkat(-1, …)` in two failure-injection preloads. A test file stream
  that a failed `ASSERT` left open. A `CMSG_FIRSTHDR` result used unchecked.
  A fuzz replay reading a stream already at its end. A dead variable in the
  shell's caffeine command. A screen-sharing test that leaked its object on
  failure. Test fixtures with fields a real object always has.
  `alloc.h`'s helpers now say `returns_nonnull`.
- **cppcheck.** Tests that read `back()` of a possibly empty list, an array
  index that could be −1, two `realloc` calls that lost the block on failure,
  a pointer dereferenced before the test checked it, 32 struct members
  without initializers, and a class owning descriptors that could be copied.
- **Clazy, level 0 at zero.** 8 temporary `QRegularExpression`s, 10 copied
  key lists, 4 mixed iterators, 3 `<QtTest>` module includes, an empty
  `QStringLiteral`, and a lambda that captured a local by reference past its
  scope. From level 1: 4 connections without a context object, 5 properties
  without `NOTIFY` (one is now `CONSTANT`; the D-Bus ones are excluded inline,
  with the reason), and 3 range loops copying strings.
- **qmllint.** The QML modules did not pass qmllint the build directories of
  `Vela.Controls` and `Vela.Bluetooth.Ui`. With `IMPORT_PATH`, `[import]` went
  from 515 to 14 warnings, `[unresolved-type]` from 434 to 1, `[required]`
  from 233 to 0 and `[unqualified]` from 3,363 to 1,709. Each of those
  warnings had been a place where qmllint could check nothing.

Suppressions are inline, with the reason next to them: three analyzer paths
that exist only through a test's partial fixtures, and one cppcheck memory
leak where the callee frees the block.

### Ceilings

[`scripts/ci-findings.py`](../scripts/ci-findings.py) counts first-party
findings per tool and check, and compares each count with
[`scripts/ci-findings-ceilings.json`](../scripts/ci-findings-ceilings.json).
A check without a ceiling has a ceiling of 0. A count above its ceiling
fails and prints that check's findings. A count below it also fails, so that
the ceiling comes down in the same change as the fix; `--update` writes the
new counts.

A finding is its check, file, line, column and message. Two findings that
differ only in the column count twice; a header's finding that several
translation units report counts once. The comparison is by count: in a
check with a ceiling above 0, a new finding goes unnoticed if the same change
fixes another. Comparing lists instead would fail on every edit that moves
the lines above a finding, since its location is the only identity the
analysers give. Checks at 0, which are
all checks of the native lane and all but a few of the Qt lane, catch every
new finding.

`qmllint` exits with status 0 when it only warns. Any other status fails the
job whatever the counts: a module that could not be linted, or a finding at
error level.

After stage 2, the native lane has no findings and no ceilings. The Qt lane
has ceilings for three Clazy checks about copies and statics
(`range-loop-detach` 137, `detaching-temporary` 145,
`non-pod-global-static` 87). It also has ceilings for qmllint's remaining
categories, mostly `unqualified` (2,043) and `missing-property` (276). These
counts tell findings apart by column; the qmllint figures in
[What extending the analysers found](#what-extending-the-analysers-found)
were counted by line, before the column was part of a finding. The 14
remaining `[import]` warnings are modules registered from C++ at run time
(`Vela.Audio`, `Vela.Bluetooth`, `Vela.Updates`, `Vela.Files.Backend`,
`Vela.Shell.Test`). qmllint can only see them once they are declared with
`qt_add_qml_module`.

The workflow installs the current Arch packages. A new release of an
analyser can therefore move a count without any change in Vela. The job on
`main` shows that first; the fix is to adjust the code or the ceiling in a
separate change.

## Stage 3: Memcheck per lane (after #324)

The first full run of the Memcheck workflow proposed in #324 (run
45841386812) selected every ELF test of the build tree:

| Lane | Tests | Summed time | Failures |
|---|---:|---:|---:|
| Native | 366 | 852 s | 8 |
| Qt | 68 | 2,365 s | 45, including 13 timeouts |

Once #324 is merged, its selection becomes `ci-lanes.py tests --lane`. The
native lane fails on defects once its 8 failures are triaged. The Qt lane
runs as a nightly report, with per-test timeouts set from measured times.
The Memcheck build should use ccache: 18 of its 45 minutes were compilation.

## Wall time

Durations of successful jobs over the last ten or so runs of each workflow,
before this change:

| Job | Warm cache | Cold cache |
|---|---|---|
| Code coverage | 6–7 min | 23–27 min |
| Fedora 45 tests | 4–6 min | 16–20 min |
| Ubuntu 26.10 compile and test | 4–5 min | 23 min |
| `asan-ubsan` | 6–8 min | – |
| `static-analysis` | 1–2 min | – |

`asan-ubsan` is not on a PR's critical path, so separate runners per lane
would save about 20 s, at the cost of a second build. They are not part of
this plan.

The static-analysis jobs build everything with Clang. Their first run on a
hosted runner, before the Qt lane was split, with a cold compiler cache:

| Job | Build | Analysis | Total |
|---|---:|---:|---:|
| `static-analysis` (native) | 7 min | 3 min (clang-tidy; cppcheck in seconds) | 11 min |
| `static-analysis-qt` | 15 min | 16.5 min (clang-tidy 11.6, Clazy and qmllint 4.8, cppcheck 7 s) | 33 min |

The counts were the same as on a local machine. With a warm cache, on the
next push, the builds took under a minute and the jobs 6 (native) and 11 (Qt)
minutes, of which the Qt lane's clang-tidy took most. That run is now split
over three runners, each with its own build from the shared compiler cache.
On the first run after the split, with a warm cache:

| Job | Build | Analysis |
|---|---:|---:|
| `static-analysis` (native) | 1 min | 6.4 min |
| `static-analysis-qt-part`, each clang-tidy shard | 0.6–0.9 min | 2.1–3.2 min |
| `static-analysis-qt-part`, cppcheck, Clazy, qmllint | 0.6 min | 2.5 min |
| `static-analysis-qt`, the comparison | – | seconds |

From its first job's start to its comparison, the Qt lane took 6 minutes.
The native lane, in one job, is now the longer of the two. A cold cache costs
one build per runner, in parallel.

## Not in this plan

- **Splitting coverage.** A split would add a second build, and Codecov
  already merges the reports uploaded for a commit.
- **Skipping lanes by path.** `common/`, CMake and the workflows feed both
  lanes.
- **Documentation-only PRs still run Code coverage and the Ubuntu build.**
  That is a separate fix.
