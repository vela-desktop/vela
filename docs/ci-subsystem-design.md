# CI architecture: subsystem-oriented parallel validation

Status: proposed (design only)

## Motivation

The current static-analysis workflow deliberately disables VELA_BUILD_SHELL and runs cppcheck and clang-tidy on selected C modules. ASan/UBSan instrument both C and C++, and CodeQL is configured for c-cpp, but native static-analysis coverage is asymmetric. The Memcheck experiment in #324 also highlights different runtime costs and diagnostic profiles between native compositor tests and Qt-heavy tests. Raw defect counts are not comparable measures of code quality.

## Goals and non-goals

- Give C and C++ comparable native analysis coverage, with Qt/QML-specific checks.
- Reduce wall-clock latency through independent runner jobs and isolate failures.
- Preserve all existing meaningful test coverage, integration checks, security checks and GPU validation.
- Keep total runner cost, cache duplication and maintenance measurable.
- This proposal does not implement workflows, change branch protection, suppress existing defects, or merge #324.

## Proposed topology

Keep workflows organized by validation purpose; split jobs within each workflow by subsystem rather than duplicating event triggers and policy.

| Workflow | Independent jobs |
| --- | --- |
| Static Analysis | C/native; C++/Qt; QML lint; existing script and formatting checks |
| Sanitizer Tests | Native/compositor; Qt applications; cross-subsystem CPU tests where applicable |
| Valgrind Memcheck | Native/compositor; Qt applications; optional further Qt shards |
| Code Coverage | Native and Qt test shards, followed by combined coverage publication |
| CodeQL | Retain combined C/C++ analysis and existing Python/Actions jobs |
| Build and integration | Retain complete desktop builds, packaging and functional tests |
| GPU validation | Retain existing AMD/NVIDIA/Intel validation |

Jobs should start independently wherever no actual data dependency exists. Integration jobs need not wait for static analysis unless required by a documented resource constraint.

## Ownership and build boundaries

The native lane includes compositor and applicable native services, tools and common libraries. The Qt lane includes shell, settings, explorer, lock, Qt-backed polkit components and shared Qt code. Do not equate the shell directory with all C++ code. Inventory every translation unit and test, including capture, bluetooth, audio, updates, fuzz replay, packaging and shared modules. Assign cross-subsystem tests to a dedicated integration lane or explicitly retain them in the full build.

Use explicit CMake configurations and target/test manifests. Native builds can disable VELA_BUILD_SHELL; Qt builds may disable VELA_BUILD_COMPOSITOR only after proving required targets and generated artifacts remain available. Otherwise build the required dependencies or retain a full build for that lane. Audit VELA_BUILD_POLKIT, VELA_BUILD_TOOLS and GPU options rather than assuming two flags are sufficient.

Prefer CTest labels with a documented ownership manifest over brittle name regexes. Validate the union of selected tests against the existing full configuration, fail on unexpected empty selections, and account for intentional overlaps and exclusions. Preserve shared-code coverage in both relevant contexts.

## Static analysis

- Native: preserve existing scan-build, cppcheck C17 and clang-tidy checks; progressively expand beyond the current hand-picked files.
- Qt: configure a real Qt-enabled compilation database; run Clang Static Analyzer, cppcheck C++20 and clang-tidy on first-party C++ translation units. Exclude generated moc/QML code and third-party sources from first-party reporting where justified, without hiding defects in callers.
- Add Clazy for Qt-specific ownership, QObject lifetime, signal/slot and API misuse checks. Start with a documented stable check set.
- Add qmllint using the actual QML module/import paths and generated type information. Distinguish missing imports from real lint findings.
- Keep clang-format and ShellCheck policy unchanged. Pin tool versions or container digests consistently.
- Existing findings from newly enabled checks should be triaged before enforcing a clean gate; if a baseline is necessary, make it explicit, reviewable and non-growing. Never silently ignore tool failures.

## Dynamic analysis

ASan/UBSan must instrument both C and C++ and link relevant binaries; preserve leak detection and fail-on-error behavior. Keep CPU-only jobs separate from GPU/RHI tests without losing existing GPU validation.

Coordinate Memcheck changes with #324 rather than creating a competing implementation. Use per-lane timeouts derived from measured runtime; distinguish timeout, infrastructure failure and memory defects. Preserve XML reports, stack traces and logs even on failure. Deduplicate by error kind and normalized stack for triage, not by suppressing failures. Only add narrow, documented upstream suppressions after verifying ownership; do not blanket-suppress Qt. Memcheck and sanitizers use separate builds.

If the Qt lane dominates elapsed time, shard its tests by historical duration, not arbitrary equal counts. Avoid concurrent tests that contend for shared D-Bus, sockets or display resources unless isolated.

## Coverage and reporting

Collect per-lane coverage with consistent source paths and compatible compiler/tool versions. Merge reports with the coverage tool's supported mechanism and publish one authoritative combined report; retain subsystem flags or summaries where useful. Do not double-count shared code or replace whole-project coverage with averages of percentages.

Publish separate JUnit, analyzer and Memcheck artifacts with unique names. Summaries should include test counts, skipped tests, failures, timeouts and actionable defects. Keep existing required check names stable during migration or explicitly coordinate branch-protection changes; an aggregate gate must fail when a required lane fails or unexpectedly skips.

## Performance and cost

Use separate runner jobs with lane-specific dependencies and ccache namespaces keyed by toolchain, instrumentation and configuration. Reuse safe caches across revisions; avoid transferring large build artifacts unless measurement demonstrates a benefit. Keep current concurrency cancellation semantics and avoid overloading self-hosted GPU runners.

Measure queue time, dependency setup, build time, test time, critical-path wall time, total runner minutes, cache hit rate and artifact size. Compare cold and warm runs on representative changes and full builds. Parallelization reduces critical path only when runner capacity is available; duplicated setup can increase total cost. Do not promise a speedup before measurement.

Initially run all relevant lanes for non-documentation changes. Consider path-based skipping only after dependency ownership is established, with shared code, CMake, workflows and toolchain changes conservatively triggering both lanes. Required checks must still report a valid outcome.

## Rollout

1. Inventory existing workflows, required checks, targets, CTest labels, coverage and dependencies; capture baseline timings and test manifests.
2. Add the Qt/C++ and QML static-analysis lanes alongside existing C checks; triage initial findings and establish enforcement.
3. Split sanitizer jobs, compare test manifests and results against the existing full job, then migrate required checks.
4. Coordinate Memcheck split with #324; calibrate timeouts and triage defects before enforcement.
5. Split coverage execution only if combined reporting and source coverage parity are demonstrated.
6. Optimize caching and shard slow lanes based on measurements. Preserve full integration, packaging and GPU checks throughout.

Each phase should be independently reviewable and reversible. Keep old checks until replacements demonstrate parity; do not weaken protection during transition.

## Acceptance criteria

- First-party C++ and QML have explicit analysis coverage; no unexplained translation-unit or test omissions.
- All existing meaningful test cases remain covered, including cross-subsystem tests and shared libraries.
- Independent jobs run concurrently when runners are available; failures and timeouts identify the affected lane.
- Combined coverage and required-check semantics remain intact.
- Representative cold/warm runs report critical-path and total runner costs, with regressions explained.
- No broad suppressions, silent failures, or unrelated product-code changes.

## Open decisions

Exact check sets and initial baselines; whether Qt-only CMake configuration is viable without refactoring; optimal Memcheck shard count; coverage merge tooling and required-check migration details; runner-minute budget. Resolve these during implementation using measurements rather than assumptions.
