#ifndef MEMPOOL_H
#define MEMPOOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Version info */
#define MEMPOOL_VERSION_MAJOR 0
#define MEMPOOL_VERSION_MINOR 1
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

/* Optional synchronization callbacks */
typedef void (*mempool_lock_fn)(void *user_ctx);
typedef void (*mempool_unlock_fn)(void *user_ctx);

typedef struct {
    mempool_lock_fn   lock;
    mempool_unlock_fn unlock;
    void             *user_ctx;
} mempool_sync_t;

/* Statistics */
typedef struct {
    uint32_t total_blocks;
    uint32_t used_blocks;
    uint32_t free_blocks;
    uint32_t peak_usage;
    uint32_t alloc_count;
    uint32_t free_count;
    uint32_t block_size;
} mempool_stats_t;

/* Opaque pool handle */
typedef struct mempool mempool_t;

/* State buffer sizing */
#ifndef MEMPOOL_STATE_SIZE
#define MEMPOOL_STATE_SIZE 128U
#endif

size_t mempool_state_size(void);

mempool_error_t mempool_init(
    void        *state_buffer,
    size_t       state_buffer_size,
    void        *pool_buffer,
    size_t       pool_buffer_size,
    size_t       block_size,
    size_t       alignment,
    mempool_t  **pool_out
);

mempool_error_t mempool_alloc(mempool_t *pool, void **block);
mempool_error_t mempool_free(mempool_t *pool, void *block);
mempool_error_t mempool_get_stats(const mempool_t *pool,
                                  mempool_stats_t *stats);
mempool_error_t mempool_reset(mempool_t *pool);
bool mempool_contains(const mempool_t *pool, const void *ptr);
const char *mempool_strerror(mempool_error_t error);
mempool_error_t mempool_set_sync(mempool_t      *pool,
                                 mempool_lock_fn lock,
                                 mempool_unlock_fn unlock,
                                 void           *user_ctx);

#ifdef __cplusplus
}
#endif

#endif /* MEMPOOL_H */
