/*
 * Memory Pool Library - Implementation
 *
 * Core design:
 *  - Fixed-size blocks managed via a singly-linked free list.
 *  - No dynamic allocation inside the library.
 *  - Platform agnostic, with optional synchronization via callbacks.
 *  - Double-free detection via a per-block allocation bitmap.
 */

#include "mempool.h"

#include <stdint.h>
#include <string.h> /* memset */

/* Internal free-list node */
typedef struct free_node_t {
    struct free_node_t *next;
} free_node_t;

static bool is_power_of_two(size_t value)
{
    bool result = false;
    if (value > 0U) {
        result = ((value & (value - 1U)) == 0U);
    }
    return result;
}

static size_t align_up(size_t value, size_t alignment)
{
    size_t mask = alignment - 1U;
    size_t result = (value + mask) & ~mask;
    return result;
}

/* Internal helpers for optional locking.
 * They take const mempool_t * but only read synchronization callbacks.
 */
static void mempool_internal_lock(const mempool_t *pool)
{
    if ((pool != NULL) && (pool->sync_enabled)) {
        if (pool->sync.lock != NULL) {
            pool->sync.lock(pool->sync.user_ctx);
        }
    }
}

static void mempool_internal_unlock(const mempool_t *pool)
{
    if ((pool != NULL) && (pool->sync_enabled)) {
        if (pool->sync.unlock != NULL) {
            pool->sync.unlock(pool->sync.user_ctx);
        }
    }
}

/* Compute block index from a block pointer.
 * Preconditions:
 *  - ptr points to the start of a block
 *  - ptr lies within [blocks_start, blocks_start + total_blocks * block_size)
 */
static uint32_t mempool_block_index(const mempool_t *pool, const void *ptr)
{
    uintptr_t base = (uintptr_t)pool->blocks_start;
    uintptr_t p = (uintptr_t)ptr;
    uintptr_t offset = p - base;
    uint32_t index = (uint32_t)(offset / (uintptr_t)pool->block_size);
    return index;
}

mempool_error_t mempool_init(mempool_t *pool, void *buffer,
                             size_t buffer_size, size_t block_size,
                             size_t alignment)
{
    mempool_error_t result = MEMPOOL_OK;

    if ((pool == NULL) || (buffer == NULL)) {
        result = MEMPOOL_ERR_NULL_PTR;
    } else if ((buffer_size == 0U) || (block_size == 0U)) {
        result = MEMPOOL_ERR_INVALID_SIZE;
    } else if (!is_power_of_two(alignment)) {
        result = MEMPOOL_ERR_ALIGNMENT;
    } else if ((((uintptr_t)buffer) & (alignment - 1U)) != 0U) {
        /* buffer must be aligned to requested alignment */
        result = MEMPOOL_ERR_ALIGNMENT;
    } else if (block_size < sizeof(free_node_t)) {
        result = MEMPOOL_ERR_INVALID_SIZE;
    } else {
        size_t aligned_block_size = align_up(block_size, alignment);
        size_t max_blocks = buffer_size / aligned_block_size;

        if (max_blocks == 0U) {
            result = MEMPOOL_ERR_INVALID_SIZE;
        } else {
            /* Determine number of blocks and bitmap size that fit into buffer.
             * Layout:
             *   [ bitmap (ceil(n/8) bytes) | padding to alignment | blocks... ]
             */
            size_t n = max_blocks;
            size_t bitmap_bytes = 0U;
            size_t blocks_offset = 0U;
            bool found = false;

            while ((n > 0U) && !found) {
                size_t bitmap_end;
                size_t pad = 0U;
                size_t needed;

                bitmap_bytes = (n + 7U) / 8U;
                bitmap_end = bitmap_bytes;

                if (alignment > 1U) {
                    size_t mod = bitmap_end & (alignment - 1U);
                    if (mod != 0U) {
                        pad = alignment - mod;
                    }
                }

                blocks_offset = bitmap_end + pad;
                needed = blocks_offset + (n * aligned_block_size);

                if (needed <= buffer_size) {
                    found = true;
                } else {
                    n--;
                }
            }

            if (!found || (n == 0U)) {
                result = MEMPOOL_ERR_INVALID_SIZE;
            } else if (n > (size_t)UINT32_MAX) {
                /* Internal representation cannot hold this many blocks */
                result = MEMPOOL_ERR_INVALID_SIZE;
            } else if (bitmap_bytes > (size_t)UINT32_MAX) {
                result = MEMPOOL_ERR_INVALID_SIZE;
            } else {
                uint32_t total_blocks = (uint32_t)n;

                /* Initialize structure fields from scratch. */
                pool->buffer_start = buffer;
                pool->blocks_start = (void *)((uint8_t *)buffer + blocks_offset);
                pool->free_list = NULL;
                pool->bitmap = (uint8_t *)buffer;
                pool->bitmap_bytes = (uint32_t)bitmap_bytes;

                pool->block_size = (uint32_t)aligned_block_size;
                pool->total_blocks = total_blocks;
                pool->free_blocks = total_blocks;
                pool->alignment = (uint32_t)alignment;

                pool->stats.total_blocks = total_blocks;
                pool->stats.used_blocks = 0U;
                pool->stats.free_blocks = total_blocks;
                pool->stats.peak_usage = 0U;
                pool->stats.alloc_count = 0U;
                pool->stats.free_count = 0U;
                pool->stats.block_size = (uint32_t)aligned_block_size;

                pool->sync.lock = NULL;
                pool->sync.unlock = NULL;
                pool->sync.user_ctx = NULL;
                pool->sync_enabled = false;

                pool->initialized = true;

                /* Clear bitmap: 0 = free, 1 = allocated. */
                (void)memset(pool->bitmap, 0, bitmap_bytes);

                /* Build free list from blocks region. */
                {
                    uint8_t *current = (uint8_t *)pool->blocks_start;
                    free_node_t *prev_node = NULL;
                    uint32_t i;

                    for (i = 0U; i < total_blocks; i++) {
                        free_node_t *node = (free_node_t *)current;
                        node->next = prev_node;
                        prev_node = node;
                        current = &current[aligned_block_size];
                    }

                    pool->free_list = prev_node;
                }
            }
        }
    }

    return result;
}

mempool_error_t mempool_alloc(mempool_t *pool, void **block)
{
    mempool_error_t result = MEMPOOL_OK;
    free_node_t *node;

    if ((pool == NULL) || (block == NULL)) {
        result = MEMPOOL_ERR_NULL_PTR;
    } else if (!pool->initialized) {
        result = MEMPOOL_ERR_NOT_INITIALIZED;
    } else {
        mempool_internal_lock(pool);

        if (pool->free_list == NULL) {
            result = MEMPOOL_ERR_OUT_OF_MEMORY;
        } else {
            node = (free_node_t *)pool->free_list;
            pool->free_list = node->next;

            if (pool->free_blocks == 0U) {
                /* Internal inconsistency; treat as out of memory. */
                result = MEMPOOL_ERR_OUT_OF_MEMORY;
                /* Restore free_list */
                pool->free_list = node;
            } else {
                uint32_t index;

                pool->free_blocks--;

                pool->stats.alloc_count++;
                pool->stats.used_blocks = pool->total_blocks - pool->free_blocks;
                pool->stats.free_blocks = pool->free_blocks;

                if (pool->stats.used_blocks > pool->stats.peak_usage) {
                    pool->stats.peak_usage = pool->stats.used_blocks;
                }

                /* Mark block as allocated in bitmap. */
                index = mempool_block_index(pool, (const void *)node);
                if (index < pool->total_blocks) {
                    uint32_t byte_index = index / 8U;
                    uint32_t bit_index = index % 8U;
                    uint8_t mask = (uint8_t)(1U << bit_index);
                    pool->bitmap[byte_index] = (uint8_t)(pool->bitmap[byte_index] | mask);
                }

                *block = (void *)node;
            }
        }

        mempool_internal_unlock(pool);
    }

    return result;
}

mempool_error_t mempool_free(mempool_t *pool, void *block)
{
    mempool_error_t result = MEMPOOL_OK;
    uintptr_t blocks_start_addr;
    uintptr_t blocks_end_addr;
    uintptr_t block_addr;
    uintptr_t offset;

    if ((pool == NULL) || (block == NULL)) {
        result = MEMPOOL_ERR_NULL_PTR;
    } else if (!pool->initialized) {
        result = MEMPOOL_ERR_NOT_INITIALIZED;
    } else {
        blocks_start_addr = (uintptr_t)pool->blocks_start;
        blocks_end_addr = blocks_start_addr +
                          ((uintptr_t)pool->total_blocks *
                           (uintptr_t)pool->block_size);
        block_addr = (uintptr_t)block;

        /* Range check: must lie within blocks region. */
        if ((block_addr < blocks_start_addr) || (block_addr >= blocks_end_addr)) {
            result = MEMPOOL_ERR_INVALID_BLOCK;
        } else {
            offset = block_addr - blocks_start_addr;

            /* Alignment check: must be on block_size boundary. */
            if ((offset % (uintptr_t)pool->block_size) != 0U) {
                result = MEMPOOL_ERR_INVALID_BLOCK;
            } else {
                uint32_t index;
                uint32_t byte_index;
                uint32_t bit_index;
                uint8_t mask;

                index = (uint32_t)(offset / (uintptr_t)pool->block_size);
                if (index >= pool->total_blocks) {
                    result = MEMPOOL_ERR_INVALID_BLOCK;
                } else {
                    byte_index = index / 8U;
                    bit_index = index % 8U;
                    mask = (uint8_t)(1U << bit_index);

                    mempool_internal_lock(pool);

                    if ((pool->bitmap[byte_index] & mask) == 0U) {
                        /* Block is already free (or never allocated). */
                        result = MEMPOOL_ERR_DOUBLE_FREE;
                    } else {
                        free_node_t *node = (free_node_t *)block;

                        /* Push block back on free-list. */
                        node->next = (free_node_t *)pool->free_list;
                        pool->free_list = (void *)node;

                        /* Update counts and bitmap. */
                        if (pool->free_blocks < pool->total_blocks) {
                            pool->free_blocks++;
                        }

                        pool->stats.free_count++;
                        pool->stats.used_blocks = pool->total_blocks - pool->free_blocks;
                        pool->stats.free_blocks = pool->free_blocks;

                        pool->bitmap[byte_index] =
                            (uint8_t)(pool->bitmap[byte_index] & (uint8_t)(~mask));
                    }

                    mempool_internal_unlock(pool);
                }
            }
        }
    }

    return result;
}

mempool_error_t mempool_get_stats(const mempool_t *pool,
                                  mempool_stats_t *stats)
{
    mempool_error_t result = MEMPOOL_OK;

    if ((pool == NULL) || (stats == NULL)) {
        result = MEMPOOL_ERR_NULL_PTR;
    } else if (!pool->initialized) {
        result = MEMPOOL_ERR_NOT_INITIALIZED;
    } else {
        mempool_internal_lock(pool);
        *stats = pool->stats;
        mempool_internal_unlock(pool);
    }

    return result;
}

mempool_error_t mempool_reset(mempool_t *pool)
{
    mempool_error_t result = MEMPOOL_OK;

    if (pool == NULL) {
        result = MEMPOOL_ERR_NULL_PTR;
    } else if (!pool->initialized) {
        result = MEMPOOL_ERR_NOT_INITIALIZED;
    } else {
        uint8_t *current;
        free_node_t *prev_node;
        uint32_t i;

        mempool_internal_lock(pool);

        /* Clear bitmap: all blocks become free. */
        (void)memset(pool->bitmap, 0, (size_t)pool->bitmap_bytes);

        /* Rebuild free-list over the existing blocks region. */
        current = (uint8_t *)pool->blocks_start;
        prev_node = NULL;
        for (i = 0U; i < pool->total_blocks; i++) {
            free_node_t *node = (free_node_t *)current;
            node->next = prev_node;
            prev_node = node;
            current = &current[pool->block_size];
        }
        pool->free_list = prev_node;

        pool->free_blocks = pool->total_blocks;

        /* Reset statistics. */
        pool->stats.total_blocks = pool->total_blocks;
        pool->stats.used_blocks = 0U;
        pool->stats.free_blocks = pool->total_blocks;
        pool->stats.peak_usage = 0U;
        pool->stats.alloc_count = 0U;
        pool->stats.free_count = 0U;
        pool->stats.block_size = pool->block_size;

        mempool_internal_unlock(pool);
    }

    return result;
}

bool mempool_contains(const mempool_t *pool, const void *ptr)
{
    bool result = false;

    if ((pool != NULL) && (ptr != NULL) && pool->initialized) {
        uintptr_t blocks_start_addr = (uintptr_t)pool->blocks_start;
        uintptr_t blocks_end_addr = blocks_start_addr +
                                    ((uintptr_t)pool->total_blocks *
                                     (uintptr_t)pool->block_size);
        uintptr_t p = (uintptr_t)ptr;

        if ((p >= blocks_start_addr) && (p < blocks_end_addr)) {
            result = true;
        }
    }

    return result;
}

const char *mempool_strerror(mempool_error_t error)
{
    const char *result;

    switch (error) {
        case MEMPOOL_OK:
            result = "Success";
            break;
        case MEMPOOL_ERR_NULL_PTR:
            result = "Null pointer";
            break;
        case MEMPOOL_ERR_INVALID_SIZE:
            result = "Invalid size";
            break;
        case MEMPOOL_ERR_OUT_OF_MEMORY:
            result = "Out of memory";
            break;
        case MEMPOOL_ERR_INVALID_BLOCK:
            result = "Invalid block";
            break;
        case MEMPOOL_ERR_ALIGNMENT:
            result = "Alignment error";
            break;
        case MEMPOOL_ERR_DOUBLE_FREE:
            result = "Double free detected";
            break;
        case MEMPOOL_ERR_NOT_INITIALIZED:
            result = "Pool not initialized";
            break;
        default:
            result = "Unknown error";
            break;
    }

    return result;
}

mempool_error_t mempool_set_sync(mempool_t *pool,
                                 mempool_lock_fn lock,
                                 mempool_unlock_fn unlock,
                                 void *user_ctx)
{
    mempool_error_t result = MEMPOOL_OK;

    if (pool == NULL) {
        result = MEMPOOL_ERR_NULL_PTR;
    } else if (!pool->initialized) {
        result = MEMPOOL_ERR_NOT_INITIALIZED;
    } else {
        pool->sync.lock = lock;
        pool->sync.unlock = unlock;
        pool->sync.user_ctx = user_ctx;
        if ((lock != NULL) && (unlock != NULL)) {
            pool->sync_enabled = true;
        } else {
            pool->sync_enabled = false;
        }
    }

    return result;
}
