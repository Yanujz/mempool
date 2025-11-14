# API Reference – Types and Error Codes

This page describes the public types exposed by the `mempool` header.

## Error Codes – `mempool_error_t`

The library uses an enum for all return codes:

```c
typedef enum {
    MEMPOOL_OK = 0,
    MEMPOOL_ERR_NULL_PTR = 1,
    MEMPOOL_ERR_INVALID_SIZE = 2,
    MEMPOOL_ERR_OUT_OF_MEMORY = 3,
    MEMPOOL_ERR_INVALID_BLOCK = 4,
    MEMPOOL_ERR_ALIGNMENT = 5,
    MEMPOOL_ERR_DOUBLE_FREE = 6,
    MEMPOOL_ERR_NOT_INITIALIZED = 7
} mempool_error_t;
```

### Summary

- `MEMPOOL_OK` – Operation completed successfully.
- `MEMPOOL_ERR_NULL_PTR` – One or more pointer arguments were `NULL`.
- `MEMPOOL_ERR_INVALID_SIZE` – Buffer sizes or block size were invalid.
- `MEMPOOL_ERR_OUT_OF_MEMORY` – Pool is exhausted; no more blocks can be allocated.
- `MEMPOOL_ERR_INVALID_BLOCK` – Pointer does not belong to this pool or is misaligned.
- `MEMPOOL_ERR_ALIGNMENT` – Alignment parameter or buffer alignment is invalid.
- `MEMPOOL_ERR_DOUBLE_FREE` – Block has already been freed or was never allocated.
- `MEMPOOL_ERR_NOT_INITIALIZED` – Operation attempted on a non-initialized pool.

---

## Opaque Pool Handle – `mempool_t`

The actual pool control structure is private to the implementation.

The header exposes:

```c
typedef struct mempool mempool_t;
```

Callers never allocate `mempool_t` directly. Instead, they provide a **state buffer** to
`mempool_init`, which constructs an internal `struct mempool` in-place and returns a
`mempool_t*` handle.

---

## Statistics – `mempool_stats_t`

The library tracks usage statistics to support monitoring and debugging:

```c
typedef struct {
    uint32_t total_blocks;  /* Configured total blocks in the pool      */
    uint32_t used_blocks;   /* Currently allocated blocks              */
    uint32_t free_blocks;   /* Currently free blocks                   */
    uint32_t peak_usage;    /* Maximum simultaneously used blocks      */
    uint32_t alloc_count;   /* Cumulative number of successful allocs  */
    uint32_t free_count;    /* Cumulative number of successful frees   */
    uint32_t block_size;    /* Size of each block in bytes             */
} mempool_stats_t;
```

These values are available via `mempool_get_stats`.

---

## Synchronization Hooks

The library does not perform any locking internally by default, but it exposes a simple
mechanism to integrate with platform-specific synchronization.

### Lock/Unlock Callback Types

```c
typedef void (*mempool_lock_fn)(void *ctx);
typedef void (*mempool_unlock_fn)(void *ctx);
```

### Synchronization Descriptor

```c
typedef struct {
    mempool_lock_fn   lock;
    mempool_unlock_fn unlock;
    void             *user_ctx;
} mempool_sync_t;
```

Callers configure synchronization using `mempool_set_sync`, described in
[API – Synchronization & Thread Safety](api-sync.md).

When enabled, the library calls these hooks around critical sections that modify
the pool's internal state.
