# Mempool Library – Requirements

This document captures the functional and safety-related requirements for the
mempool library. Requirement IDs are referenced from tests and design notes.

## Context

The mempool library provides deterministic fixed-size memory block allocation
using a pre-allocated buffer and an opaque internal state object. It is intended
for use in embedded and safety-related systems where dynamic memory allocation
is undesirable or prohibited.

## Requirements

### R-001 – No dynamic allocation

The library must not call any dynamic memory allocation functions (such as
`malloc`, `calloc`, `realloc`, or `free`). All memory used by the mempool must
be provided by the caller via the state and pool buffers.

### R-002 – Deterministic, bounded-time operations

`mempool_init`, `mempool_alloc`, `mempool_free`, `mempool_reset`,
`mempool_get_stats`, `mempool_contains`, and `mempool_set_sync` must execute in
bounded time independent of the number of allocations performed. Allocation and
free must be O(1) with respect to the number of blocks in the pool.

### R-003 – Initialization validation

The library shall validate all initialization parameters and reject invalid
configurations with well-defined error codes, including:
- null pointers,
- insufficient state buffer size,
- non-power-of-two alignment,
- misaligned pool buffer,
- zero or too-small block size,
- pool buffers that cannot hold at least one block.

### R-004 – Out-of-memory handling

`mempool_alloc` must indicate pool exhaustion via `MEMPOOL_ERR_OUT_OF_MEMORY`
and must not corrupt existing allocations or internal state. No previously
allocated blocks may be invalidated when exhaustion occurs.

### R-005 – Double-free detection

`mempool_free` must detect attempts to free the same block twice and return
`MEMPOOL_ERR_DOUBLE_FREE` without corrupting internal data structures.

### R-006 – Invalid pointer detection

`mempool_free` must detect pointers that do not belong to the pool address
range, or that are not aligned to a block boundary, and return
`MEMPOOL_ERR_INVALID_BLOCK`.

### R-007 – Statistics correctness

`mempool_get_stats` shall provide consistent and accurate statistics:
- total number of blocks,
- number of used and free blocks,
- peak usage (maximum used blocks),
- allocation and free counts,
- effective block size.

After `mempool_reset` all statistical counters (except `block_size` and
`total_blocks`) shall be reset to zero.

### R-008 – Reset behavior

`mempool_reset` shall restore the pool to the initial state where all blocks
are free and available. Any previously returned pointers are no longer valid.
Freeing such pointers after reset must be detected and prevented from corrupting
the pool.

### R-009 – Thread-safety hook

The library is not inherently thread-safe, but shall provide a mechanism via
`mempool_set_sync` to install user-provided lock/unlock callbacks. When
configured, all operations that modify internal state must invoke these
callbacks to serialize access.

### R-010 – Pointer range query

`mempool_contains` must return `true` for any pointer returned by
`mempool_alloc` (while it is still allocated) and `false` for any pointer
outside of the pool buffer or a null pointer.

### R-011 – Error reporting

`mempool_strerror` must return a non-null, human-readable string describing
each defined `mempool_error_t` value, and a generic string for unknown values.

### R-012 – Opaque state and fixed upper bound

The type `mempool_t` shall be opaque to users. Callers must reserve at least
`MEMPOOL_STATE_SIZE` bytes for the internal state buffer. The implementation
must ensure, using compile-time checks where possible, that the real state size
is less than or equal to `MEMPOOL_STATE_SIZE`, and must expose
`mempool_state_size()` so that callers can perform runtime assertions if
desired.
