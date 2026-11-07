# Host allocation / OOM contract — design draft

> **Status:** proposal only. This PR adds no implementation. Review policy and scope before refactoring.

## Current inconsistency

PR #90 made specific renderer wait-array growth fail explicitly and removed heap allocation from shutdown's supervisor-child enumeration, but did **not** unify host-memory handling. `compositor/src/util.c:vela_grow()` aborts on `realloc` failure, while other sites directly dereference potentially null `calloc` results: examples include `compositor/src/command.c` in `handle_connection()` and `vela_commands_listen()`, `compositor/src/input.c:add_pointer()`, and `compositor/src/render/renderer.c:vela_renderer_create()`.

`vela_grow()` also doubles an `int` capacity without an overflow guard and multiplies capacity by element size without checked arithmetic. These are separate correctness bugs even when OOM is rare.

## Proposed contract (needs sign-off)

1. **Compositor core, unrecoverable host allocation:** a small set of typed checked allocation helpers, e.g. `vela_alloc`, `vela_calloc`, `vela_realloc` and `vela_reallocarray`, fail *explicitly* and consistently with an allocation-site diagnostic before aborting. Do not dereference null.
2. **Recoverable resource allocation:** where a feature can cleanly fail without invalid state, retain a visibly named fallible path (`vela_try_*`) and a documented return-value check. Do not silently convert expected optional allocations to fatal errors.
3. Preserve the supervisor's allocation-free critical shutdown path from #90; do not require a heap allocation in OOM cleanup/diagnostics.
4. Check `size_t` multiplication/addition and `int` capacity growth for overflow before reallocating. Define semantics for zero-sized allocation.
5. Migrate allocations by subsystem with tests; do not wholesale replace third-party library allocations or unrelated Qt allocations.
6. Keep Vulkan device memory failures (`VkResult` such as `VK_ERROR_OUT_OF_DEVICE_MEMORY`) distinct from host allocator failures. Their handling belongs in the Vulkan error policy, not the host fail-fast helper.

## Test / audit checklist

- Enumerate `malloc`, `calloc`, `realloc`, `strdup`, `reallocarray` and size arithmetic in compositor + supervisor; classify each allocation.
- Fault injection: fail the first and Nth host allocation and assert expected abort/clean decline; exercise representative command socket, pointer hotplug, startup and renderer creation.
- Overflow tests for `vela_grow` capacities and `size_t` products; keep existing #90 wait-array OOM regression tests.
- Run ASan/UBSan, static analysis, and the existing CPU + GPU tests affected by the change.

## Non-goals

No promise to continue rendering under system-wide OOM. A deliberate, observable fail-fast policy is preferable to a null dereference; graceful recovery is only required at explicitly classified recoverable sites.
