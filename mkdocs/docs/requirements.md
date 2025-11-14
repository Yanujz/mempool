# Requirements

This document captures functional and non-functional requirements for the `mempool`
library. Identifiers (`R-xxx`) are used in traceability.

## Functional Requirements

- **R-001 – No Dynamic Allocation**  
  The library must not call `malloc`, `free`, `realloc`, or any equivalent
  dynamic memory API. All memory must be provided by the caller.

- **R-002 – Deterministic Allocation and Free**  
  `mempool_alloc` and `mempool_free` shall execute in O(1) time with respect
  to the number of blocks in the pool.

- **R-003 – Fixed-Size Blocks**  
  Each pool instance shall manage fixed-size blocks of a single block size.

- **R-004 – Alignment Control**  
  The caller shall be able to specify an alignment (power of two) for blocks
  and the pool buffer. Misaligned buffers or invalid alignment values shall
  be rejected.

- **R-005 – Explicit Initialization**  
  All pool operations shall fail with `MEMPOOL_ERR_NOT_INITIALIZED` if the
  pool has not been successfully initialized by `mempool_init`.

- **R-006 – Error Reporting**  
  Every public function shall return a `mempool_error_t` that indicates
  success or the reason for failure.

- **R-007 – Double-Free Detection**  
  The library shall detect an attempt to free a block that has already been
  freed and return `MEMPOOL_ERR_DOUBLE_FREE`.

- **R-008 – Invalid Block Detection**  
  The library shall detect pointers that do not belong to the pool or are
  incorrectly aligned, and return `MEMPOOL_ERR_INVALID_BLOCK`.

- **R-009 – Statistics Reporting**  
  The library shall maintain usage statistics (total, used, free, peak,
  allocation count, free count, block size) and provide them via
  `mempool_get_stats`.

- **R-010 – Reset Support**  
  The library shall provide a way to reset a pool to its initial state,
  returning all blocks to the free list and resetting statistics.

- **R-011 – Contains Check**  
  The library shall provide a way to check whether a pointer lies within
  the address range managed by a given pool.

- **R-012 – Human-Readable Error Strings**  
  The library shall provide a function `mempool_strerror` to translate
  error codes to human-readable strings.

- **R-013 – Optional Synchronization Hooks**  
  The library shall allow the caller to configure synchronization callbacks
  (`mempool_set_sync`) so that operations can be made thread-safe when needed.

## Non-Functional Requirements

- **N-001 – Portability**  
  The code shall be written in portable C11 and not rely on OS-specific APIs
  in the core implementation.

- **N-002 – Predictable Behavior on Error**  
  Functions shall never leave the pool in an inconsistent state if they
  return an error.

- **N-003 – Testability**  
  The code base shall be structured so that unit tests can run on host
  toolchains without special hardware.

- **N-004 – Diagnostic Support**  
  The repository shall include tests, example programs, and diagnostic
  building blocks (statistics, error strings) to ease integration and
  troubleshooting.

- **N-005 – Static and Dynamic Analysis Friendly**  
  The project shall be compatible with static analysis and sanitizers on
  host builds, without requiring proprietary tools.
