# Memory Pool Library - API Documentation

## Overview

The Memory Pool Library provides deterministic, fixed-size block allocation
with O(1) performance characteristics. It is written in C11 with a
MISRA-friendly style and is suitable for embedded and real-time systems where
dynamic allocation is prohibited or undesirable.

The core is platform agnostic and can be made thread-safe via user-provided
synchronization callbacks. Double-free detection is implemented using a
per-block bitmap.

## Data Structures

### `mempool_t`

```c
typedef struct mempool_t {
    void *buffer_start;     /* start of full buffer */
    void *blocks_start;     /* start of block region (aligned) */
    void *free_list;        /* head of free-list (singly-linked) */
    uint8_t *bitmap;        /* allocation bitmap (1 bit per block) */
    uint32_t bitmap_bytes;  /* size of bitmap in bytes */

    uint32_t block_size;    /* aligned block size */
    uint32_t total_blocks;  /* total number of blocks */
    uint32_t free_blocks;   /* currently free blocks */
    uint32_t alignment;     /* alignment in bytes (power of two) */

    mempool_stats_t stats;  /* usage statistics */
    mempool_sync_t sync;    /* synchronization callbacks */

    bool initialized;       /* initialization status */
    bool sync_enabled;      /* synchronization enabled flag */
} mempool_t;
```

Treat this structure as **opaque** in application code; fields may change
between versions. Always use the public API.

### `mempool_stats_t`

```c
typedef struct {
    uint32_t total_blocks;
    uint32_t used_blocks;
    uint32_t free_blocks;
    uint32_t peak_usage;
    uint32_t alloc_count;
    uint32_t free_count;
    uint32_t block_size;
} mempool_stats_t;
```

### `mempool_error_t`

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

### Synchronization Types

```c
typedef void (*mempool_lock_fn)(void *user_ctx);
typedef void (*mempool_unlock_fn)(void *user_ctx);

typedef struct {
    mempool_lock_fn lock;
    mempool_unlock_fn unlock;
    void *user_ctx;
} mempool_sync_t;
```

These callbacks are optional. If configured, operations that mutate or
inspect pool state invoke `lock`/`unlock` around critical sections.

## Functions

### `mempool_init`

```c
mempool_error_t mempool_init(
    mempool_t *pool,
    void *buffer,
    size_t buffer_size,
    size_t block_size,
    size_t alignment
);
```

Initializes a memory pool over a pre-allocated, properly aligned buffer.

- `pool` and `buffer` must be non-NULL.
- `buffer_size` and `block_size` must be non-zero.
- `alignment` must be a power of 2.
- `buffer` must be aligned to `alignment`.
- `block_size` must be at least `sizeof(void*)` (internally, `sizeof(free_node_t)`).

Internally, the buffer layout is:

```text
[ bitmap (ceil(n/8) bytes) | padding to alignment | blocks... ]
```

Where `n` is the number of blocks that fit in the buffer.

On success, the free-list and statistics are initialized, and any previous
contents of `pool` are ignored.

### `mempool_alloc`

```c
mempool_error_t mempool_alloc(mempool_t *pool, void **block);
```

Allocates one block from the pool.

- Returns `MEMPOOL_OK` and sets `*block` on success.
- Returns `MEMPOOL_ERR_OUT_OF_MEMORY` if there are no free blocks.

If synchronization is enabled via `mempool_set_sync`, the allocation is
performed under the user-provided lock.

### `mempool_free`

```c
mempool_error_t mempool_free(mempool_t *pool, void *block);
```

Frees a block back to the pool. Performs:

- Null checks
- Range checks (block must lie within the pool buffer region)
- Alignment checks (block must be on `block_size` boundary)
- Double-free detection via bitmap (block must be currently allocated)

If the pointer is valid but the corresponding bitmap bit is already clear,
`MEMPOOL_ERR_DOUBLE_FREE` is returned. This also covers the case of pointers
into the pool region that were never allocated by this pool instance.

### `mempool_get_stats`

```c
mempool_error_t mempool_get_stats(
    const mempool_t *pool,
    mempool_stats_t *stats
);
```

Copies the current statistics into `*stats`. If synchronization is enabled,
the read is performed under the lock.

### `mempool_reset`

```c
mempool_error_t mempool_reset(mempool_t *pool);
```

Resets the pool in place:

- Clears the bitmap (all blocks become free)
- Rebuilds the free-list over the same block region
- Resets statistics to their initial values

> **Warning:** Any previously allocated blocks become invalid after reset.
> Do not call `mempool_reset` while other threads might still use existing
> block pointers.

### `mempool_contains`

```c
bool mempool_contains(const mempool_t *pool, const void *ptr);
```

Returns `true` if `ptr` lies within the pool's block region, `false`
otherwise. This does not indicate whether `ptr` is currently allocated or
free, only that it lies in the managed range.

### `mempool_strerror`

```c
const char *mempool_strerror(mempool_error_t error);
```

Returns a constant string describing the given error code.

### `mempool_set_sync`

```c
mempool_error_t mempool_set_sync(
    mempool_t *pool,
    mempool_lock_fn lock,
    mempool_unlock_fn unlock,
    void *user_ctx
);
```

Configures optional synchronization callbacks for a pool.

- Must be called after `mempool_init`.
- If both `lock` and `unlock` are non-NULL, `sync_enabled` is set to `true`.
- If either is NULL, synchronization is disabled.

This function should be called once before the pool is shared with other
threads. Modifying synchronization configuration while other threads are
using the pool is not supported.

## Error Handling

All functions return `mempool_error_t` to indicate success or failure.
Always check return values in production code.

```c
mempool_error_t err;

err = mempool_init(&pool, buf, size, block_sz, align);
if (err != MEMPOOL_OK) {
    // Handle initialization failure
}

err = mempool_alloc(&pool, &block);
if (err == MEMPOOL_ERR_OUT_OF_MEMORY) {
    // Handle exhaustion
} else if (err != MEMPOOL_OK) {
    // Handle other errors
}
```

## Best Practices

- Treat `mempool_t` as opaque; use only the public API.
- Configure synchronization via `mempool_set_sync` once, then share the pool.
- Use `mempool_get_stats` to size buffers appropriately and track high-water marks.
- Avoid calling `mempool_reset` in the presence of concurrent users.
- For safety-critical use, run static analysis tools and add project-specific
  checks and assertions as needed.
