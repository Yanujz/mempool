# Safety Manual (Integration Notes)

This document describes assumptions, intended use, and integration responsibilities
for users of the `mempool` library in safety-related systems.

## Intended Use

- Deterministic allocation of fixed-size blocks from a pre-allocated buffer.
- Use in embedded or real-time systems where dynamic memory allocation is restricted
  or forbidden.
- Integration in both single-threaded and multi-threaded environments, with
  synchronization provided by the caller where required.

The library is **not** a general-purpose allocator and does not support variable-sized
allocations or reallocation.

## Non-Intended Use

- Managing variable-sized allocations.
- Use as a drop-in replacement for `malloc` / `free` without adaptation.
- Use without checking return codes.
- Use in systems where memory availability is assumed unlimited or unbounded.

---

## Safety Assumptions

The following assumptions are made about the environment and integration:

- The caller provides **valid, non-overlapping** buffers for state and pool memory.
- The caller checks the return value of all public functions and handles error
  codes appropriately.
- The caller does not use pointers after they have been freed, or after a
  pool has been reset.
- For multi-threaded use, the caller configures appropriate synchronization
  via `mempool_set_sync` or otherwise ensures exclusive access.
- The compiler and toolchain are configured with reasonable warnings enabled
  and any reported issues are addressed.

---

## Failure Modes and Expected Behavior

### Out of Memory

- Condition: No more blocks are available in the pool.
- Behavior: `mempool_alloc` returns `MEMPOOL_ERR_OUT_OF_MEMORY` and does not
  modify the internal state beyond what is necessary to detect the condition.
- Integration note: The caller must handle this error; typical actions are to
  shed load, log an error, or switch to a degraded mode.

### Invalid Block

- Condition: `mempool_free` is called with a pointer that does not belong to
  the pool or is misaligned.
- Behavior: The function returns `MEMPOOL_ERR_INVALID_BLOCK`. Internal state
  is not modified.
- Integration note: This indicates a programming error or memory corruption.
  The system should treat this as a serious fault.

### Double Free

- Condition: `mempool_free` is called with a block that has already been freed
  or with a pointer that was never allocated.
- Behavior: The function returns `MEMPOOL_ERR_DOUBLE_FREE`. Internal state is
  not modified.
- Integration note: This indicates a programming error and may warrant a
  failsafe reaction in safety-critical systems.

### Not Initialized

- Condition: Any API except `mempool_state_size` or `mempool_init` is called
  on an uninitialized pool handle.
- Behavior: The function returns `MEMPOOL_ERR_NOT_INITIALIZED`.
- Integration note: Ensure proper initialization order and error handling
  in the calling system.

---

## Thread Safety

- By default, the library does **not** perform any locking.
- When `mempool_set_sync` is used, the library calls user-provided `lock` /
  `unlock` callbacks around critical sections.
- The correctness and performance of these callbacks are the responsibility
  of the integrator.
- In systems where interrupts may access the pool, it is the integrator’s
  responsibility to ensure interrupt-safe usage (e.g. via critical sections).

---

## Recommended Integration Practices

- Perform initialization at startup and treat any failure as a configuration
  or resource error.
- Avoid calling `mempool_reset` while other parts of the system may still hold
  pointers into the pool.
- Use statistics (`mempool_get_stats`) to monitor pool usage and detect
  near-exhaustion early.
- Consider separate pools for different classes of objects (e.g. packets,
  control structures) to avoid interference.

---

## Known Limitations

- Only supports fixed-size blocks.
- No built-in integration with RTOS-specific primitives; must be provided
  by the caller.
- Does not protect against misuse of returned pointers beyond the checks
  described above.

---

## Verification and Validation

The repository includes:

- A C-based test harness covering core behaviors and error paths.
- A GoogleTest-based C++ test suite, including multithreaded stress tests
  when running on host platforms.
- CI configurations for:
  - static analysis (`cppcheck`),
  - sanitizer-instrumented builds (ASan, UBSan, optional TSAN),
  - unit test execution.

Users integrating this library into a larger system remain responsible for:

- verifying behavior in their specific environment,
- performing additional testing necessary for their safety goals,
- and deciding how to incorporate this component into their safety case.
