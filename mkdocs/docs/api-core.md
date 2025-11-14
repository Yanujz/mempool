# API Reference – Core Functions

This page lists the core functions exposed by the library.

All functions return a `mempool_error_t` unless documented otherwise. Callers should
check the return value of every function.

---

## `size_t mempool_state_size(void);`

Returns the number of bytes required to hold the internal `mempool_t` state.

```c
size_t mempool_state_size(void);
```

Typical usage:

```c
size_t needed = mempool_state_size();
if (needed > MEMPOOL_STATE_SIZE) {
    /* Configuration error: increase MEMPOOL_STATE_SIZE */
}
```

---

## `mempool_init`

Initializes a memory pool in caller-provided storage.

```c
mempool_error_t mempool_init(
    void        *state_buffer,
    size_t       state_buffer_size,
    void        *pool_buffer,
    size_t       pool_buffer_size,
    size_t       block_size,
    size_t       alignment,
    mempool_t  **pool_out);
```

### Parameters

- `state_buffer`  
  Pointer to the state buffer (must be at least `mempool_state_size()` bytes).

- `state_buffer_size`  
  Size of `state_buffer` in bytes.

- `pool_buffer`  
  Pointer to the pool buffer that will hold all blocks.

- `pool_buffer_size`  
  Size of `pool_buffer` in bytes.

- `block_size`  
  Size of each allocation block in bytes (must be at least `sizeof(void*)`, typically larger).

- `alignment`  
  Alignment requirement for blocks and `pool_buffer` (must be a power of two).

- `pool_out`  
  Output parameter; on success, set to a valid `mempool_t*` handle.

### Returns

- `MEMPOOL_OK` on success.
- `MEMPOOL_ERR_NULL_PTR` if any required pointer is `NULL`.
- `MEMPOOL_ERR_INVALID_SIZE` if sizes are invalid or no block can fit.
- `MEMPOOL_ERR_ALIGNMENT` if alignment is invalid or the pool buffer is misaligned.

---

## `mempool_alloc`

Allocates a single block from the pool.

```c
mempool_error_t mempool_alloc(mempool_t *pool, void **block);
```

### Parameters

- `pool` – Pool handle returned by `mempool_init`.
- `block` – Output pointer to receive the allocated block.

### Returns

- `MEMPOOL_OK` on success; `*block` will be non-`NULL`.
- `MEMPOOL_ERR_OUT_OF_MEMORY` if the pool is exhausted.
- `MEMPOOL_ERR_NULL_PTR` if `pool` or `block` is `NULL`.
- `MEMPOOL_ERR_NOT_INITIALIZED` if the pool was not properly initialized.

The operation is O(1).

---

## `mempool_free`

Frees a previously allocated block back to the pool.

```c
mempool_error_t mempool_free(mempool_t *pool, void *block);
```

### Parameters

- `pool` – Pool handle returned by `mempool_init`.
- `block` – Pointer to a block obtained from `mempool_alloc`.

### Returns

- `MEMPOOL_OK` on success.
- `MEMPOOL_ERR_NULL_PTR` if `pool` or `block` is `NULL`.
- `MEMPOOL_ERR_NOT_INITIALIZED` if the pool is not initialized.
- `MEMPOOL_ERR_INVALID_BLOCK` if `block` does not belong to this pool or is misaligned.
- `MEMPOOL_ERR_DOUBLE_FREE` if the block has already been freed.

The library performs bounds and alignment checks and uses a bitmap to detect
double-free attempts.

---

## `mempool_get_stats`

Retrieves current usage statistics.

```c
mempool_error_t mempool_get_stats(const mempool_t *pool,
                                  mempool_stats_t *stats);
```

### Parameters

- `pool` – Pool handle.
- `stats` – Output structure to receive statistics.

### Returns

- `MEMPOOL_OK` on success.
- `MEMPOOL_ERR_NULL_PTR` if `pool` or `stats` is `NULL`.
- `MEMPOOL_ERR_NOT_INITIALIZED` if the pool is not initialized.

---

## `mempool_reset`

Resets a pool to its initial state.

```c
mempool_error_t mempool_reset(mempool_t *pool);
```

### Behavior

- All blocks become free.
- Statistics are reset (usage, peak, alloc/free counts).
- All existing pointers previously returned by `mempool_alloc` become invalid.
  Passing them to `mempool_free` after a reset results in `MEMPOOL_ERR_DOUBLE_FREE`.

### Returns

- `MEMPOOL_OK` on success.
- `MEMPOOL_ERR_NULL_PTR` if `pool` is `NULL`.
- `MEMPOOL_ERR_NOT_INITIALIZED` if the pool is not initialized.

---

## `mempool_contains`

Checks whether a pointer lies within the pool’s managed block region.

```c
bool mempool_contains(const mempool_t *pool, const void *ptr);
```

### Returns

- `true` if `ptr` is within the range of blocks managed by `pool`.
- `false` otherwise, or if `pool` is `NULL` or not initialized.

This function does not check whether the block is currently allocated or free,
only whether it belongs to the pool.

---

## `mempool_strerror`

Returns a human-readable string for a `mempool_error_t` value.

```c
const char *mempool_strerror(mempool_error_t error);
```

The returned pointer refers to a static string that must not be modified or freed.

Example:

```c
mempool_error_t err = mempool_alloc(pool, &block);
if (err != MEMPOOL_OK) {
    printf("mempool_alloc failed: %s\n", mempool_strerror(err));
}
```
