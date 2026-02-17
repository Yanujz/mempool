#ifndef MEMPOOL_H
#define MEMPOOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "mempool_cfg.h"

/* Version info */
#define MEMPOOL_VERSION_MAJOR 0
#define MEMPOOL_VERSION_MINOR 3
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

#if MEMPOOL_ENABLE_STATS
typedef struct {
    uint32_t total_blocks;
    uint32_t used_blocks;
    uint32_t free_blocks;
    uint32_t peak_usage;
    uint32_t alloc_count;
    uint32_t free_count;
    uint32_t block_size;
} mempool_stats_t;
#endif

/* Opaque pool handle */
typedef struct mempool mempool_t;

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
mempool_error_t mempool_reset(mempool_t *pool);
int             mempool_contains(const mempool_t *pool, const void *ptr);

#if MEMPOOL_ENABLE_STATS
mempool_error_t mempool_get_stats(const mempool_t *pool,
                                  mempool_stats_t *stats);
#endif

#if MEMPOOL_ENABLE_STRERROR
const char *mempool_strerror(mempool_error_t error);
#endif

#ifdef __cplusplus
}
#endif

#endif /* MEMPOOL_H */
