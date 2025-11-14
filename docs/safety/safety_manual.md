# Mempool Library – Safety Manual (Draft)

> Status: template. Integrators MUST adapt this to their system safety concept.

## 1. Purpose

This safety manual describes the intended use, assumptions, and integration
guidelines for the mempool library when used in safety-related contexts (such
as systems developed in accordance with ISO 26262).

The mempool library provides a deterministic, fixed-size memory allocation
mechanism operating on caller-provided buffers with no dynamic memory
allocation.

## 2. Intended Use

- Use in embedded / real-time systems where:
  - dynamic allocation is prohibited or discouraged,
  - deterministic and bounded-time memory allocation is required.

- Typical roles:
  - buffer pools for communication stacks,
  - message or packet buffers,
  - small object pools in control loops.

### Non-intended use

- Managing variable-size allocations (heap replacement).
- Allocating extremely large numbers of tiny blocks without sufficient static
  memory.
- Use without error checking or without reacting appropriately to error codes.

## 3. Assumptions and Preconditions

- Callers provide:
  - a state buffer of at least `MEMPOOL_STATE_SIZE` bytes,
  - a pool buffer with sufficient size and alignment.
- Callers must:
  - check and handle the return codes of all API functions,
  - ensure that the pool is initialized exactly once before use,
  - not reuse pointers after `mempool_reset` or after a failing `mempool_init`.

- Thread safety:
  - The library is not inherently thread-safe.
  - For multi-threaded use, the caller must install synchronization callbacks
    using `mempool_set_sync` and ensure that **all** pool access goes through
    the mempool API.

## 4. Failure Modes and Expected Behavior

- **Out of memory (`MEMPOOL_ERR_OUT_OF_MEMORY`)**
  - No new block can be allocated.
  - Existing allocations remain valid.
  - Caller must treat this as a controlled failure and choose a safe fallback
    (e.g. drop messages, enter degraded mode).

- **Double free (`MEMPOOL_ERR_DOUBLE_FREE`)**
  - Indicates a violation of API usage.
  - Internal pool remains consistent.
  - Caller should treat this as a software fault; system reaction depends on
    ASIL and safety concept (e.g. safe state or fault handling).

- **Invalid block (`MEMPOOL_ERR_INVALID_BLOCK`)**
  - Pointer does not belong to pool or is misaligned.
  - Internal state must not be corrupted.
  - Caller should treat as a programming error.

- **Not initialized (`MEMPOOL_ERR_NOT_INITIALIZED`)**
  - Any operation on a pool that has not been successfully initialized.
  - Application should not allow this in released configurations; may be
    caught in development/testing.

## 5. Integration Guidelines

- Always check the result of `mempool_init` and every subsequent call.
- Do not use any pointer returned by `mempool_alloc` after:
  - the pool has been reset, or
  - `mempool_init` has been re-run on the same buffers.

- Do not mix:
  - mempool-managed pointers and raw pointer arithmetic that escapes the
    allocator’s view.

- For multi-threaded use:
  - install `mempool_set_sync` callbacks bound to a mutex or equivalent,
  - ensure that all threads use the same pool handle and obey the locking
    policy.

## 6. Diagnostic Coverage and Verification

The following measures are applied at library level (see `docs/dev_plan.md`):

- Extensive unit and concurrency tests (C test harness + GoogleTest).
- Static analysis with free tools (compiler warnings, cppcheck, clang-tidy).
- Dynamic analysis with sanitizers (ASan/UBSan/TSan) on supported platforms.
- Structural coverage measurement (gcov/lcov or llvm-cov) aiming at very high
  coverage of the small code base.

## 7. Residual Risks and Limitations

- The library does not enforce real-time deadlines; integrators must ensure
  execution time is acceptable on their target hardware.
- The provided tests and tooling are host-based; final integration should
  include hardware-in-the-loop or target-specific verification as appropriate.
- This manual does not by itself constitute an ISO 26262 safety case; it is a
  building block to be integrated into a system-level safety argument.
