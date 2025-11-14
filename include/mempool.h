#ifndef MEMPOOL_H
#define MEMPOOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Library version */
#define MEMPOOL_VERSION_MAJOR 2
#define MEMPOOL_VERSION_MINOR 0
#define MEMPOOL_VERSION_PATCH 0

/* Error codes */
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

/* Optional synchronization callbacks.
 *
 * The library itself remains platform agnostic: it does not depend on any
 * particular threading API. Users provide lock/unlock callbacks that wrap
 * pthreads, C11 threads, std::mutex, RTOS primitives, etc.
 */
typedef void (*mempool_lock_fn)(void *user_ctx);
typedef void (*mempool_unlock_fn)(void *user_ctx);

typedef struct {
    mempool_lock_fn lock;
    mempool_unlock_fn unlock;
    void *user_ctx;
} mempool_sync_t;

/* Pool statistics for monitoring */
typedef struct {
    uint32_t total_blocks;
    uint32_t used_blocks;
    uint32_t free_blocks;
    uint32_t peak_usage;
    uint32_t alloc_count;
    uint32_t free_count;
    uint32_t block_size;
} mempool_stats_t;

/* Memory pool control structure.
 *
 * Treat this structure as opaque from application code. Its layout may change
 * between releases. Only the documented API functions should be used.
 */
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

/* Core API */
mempool_error_t mempool_init(mempool_t *pool, void *buffer,
                             size_t buffer_size, size_t block_size,
                             size_t alignment);
mempool_error_t mempool_alloc(mempool_t *pool, void **block);
mempool_error_t mempool_free(mempool_t *pool, void *block);
mempool_error_t mempool_get_stats(const mempool_t *pool,
                                  mempool_stats_t *stats);
mempool_error_t mempool_reset(mempool_t *pool);
bool mempool_contains(const mempool_t *pool, const void *ptr);
const char *mempool_strerror(mempool_error_t error);

/* Configure optional synchronization for a pool.
 *
 * Passing non-NULL lock/unlock pointers enables synchronization; passing
 * NULL for either disables it.
 *
 * This function must be called after mempool_init and before the pool
 * is shared with other threads.
 */
mempool_error_t mempool_set_sync(mempool_t *pool,
                                 mempool_lock_fn lock,
                                 mempool_unlock_fn unlock,
                                 void *user_ctx);

#endif /* MEMPOOL_H */
