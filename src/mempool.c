#include "mempool.h"
#include <string.h>

typedef struct free_node_t {
    struct free_node_t *next;
} free_node_t;

struct mempool {
    void    *pool_buffer_start;
    void    *blocks_start;
    void    *free_list;
    uint8_t *bitmap;
    uint32_t bitmap_bytes;

    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t alignment;

    mempool_stats_t stats;

    mempool_sync_t sync;
    bool           sync_enabled;

    bool           initialized;
};

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(MEMPOOL_STATE_SIZE >= sizeof(struct mempool),
               "MEMPOOL_STATE_SIZE too small for mempool state");
#endif

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

static void mempool_internal_lock(const mempool_t *pool)
{
    if ((pool != NULL) && pool->sync_enabled) {
        if (pool->sync.lock != NULL) {
            pool->sync.lock(pool->sync.user_ctx);
        }
    }
}

static void mempool_internal_unlock(const mempool_t *pool)
{
    if ((pool != NULL) && pool->sync_enabled) {
        if (pool->sync.unlock != NULL) {
            pool->sync.unlock(pool->sync.user_ctx);
        }
    }
}

static uint32_t mempool_block_index(const mempool_t *pool, const void *ptr)
{
    uintptr_t base = (uintptr_t)pool->blocks_start;
    uintptr_t p    = (uintptr_t)ptr;
    uintptr_t off  = p - base;
    uint32_t index = (uint32_t)(off / (uintptr_t)pool->block_size);
    return index;
}

size_t mempool_state_size(void)
{
    return sizeof(struct mempool);
}

mempool_error_t mempool_init(
    void        *state_buffer,
    size_t       state_buffer_size,
    void        *pool_buffer,
    size_t       pool_buffer_size,
    size_t       block_size,
    size_t       alignment,
    mempool_t  **pool_out)
{
    mempool_t *pool;
    mempool_error_t result = MEMPOOL_OK;

    if ((state_buffer == NULL) || (pool_buffer == NULL) || (pool_out == NULL)) {
        return MEMPOOL_ERR_NULL_PTR;
    }

    if (state_buffer_size < sizeof(struct mempool)) {
        return MEMPOOL_ERR_INVALID_SIZE;
    }

    if ((pool_buffer_size == 0U) || (block_size == 0U)) {
        return MEMPOOL_ERR_INVALID_SIZE;
    }

    if (!is_power_of_two(alignment)) {
        return MEMPOOL_ERR_ALIGNMENT;
    }

    if ((((uintptr_t)pool_buffer) & (alignment - 1U)) != 0U) {
        return MEMPOOL_ERR_ALIGNMENT;
    }

    if (block_size < sizeof(free_node_t)) {
        return MEMPOOL_ERR_INVALID_SIZE;
    }

    pool = (mempool_t *)state_buffer;
    memset(pool, 0, sizeof(struct mempool));

    size_t aligned_block_size = align_up(block_size, alignment);
    size_t max_blocks         = pool_buffer_size / aligned_block_size;

    if (max_blocks == 0U) {
        return MEMPOOL_ERR_INVALID_SIZE;
    }

    size_t n             = max_blocks;
    size_t bitmap_bytes  = 0U;
    size_t blocks_offset = 0U;
    bool   found         = false;

    while ((n > 0U) && !found) {
        size_t bitmap_end;
        size_t pad      = 0U;
        size_t required = 0U;

        bitmap_bytes = (n + 7U) / 8U;
        bitmap_end   = bitmap_bytes;

        if (alignment > 1U) {
            size_t mod = bitmap_end & (alignment - 1U);
            if (mod != 0U) {
                pad = alignment - mod;
            }
        }

        blocks_offset = bitmap_end + pad;
        required      = blocks_offset + (n * aligned_block_size);

        if (required <= pool_buffer_size) {
            found = true;
        } else {
            n--;
        }
    }

    if (!found || (n == 0U)) {
        return MEMPOOL_ERR_INVALID_SIZE;
    }

    if ((n > (size_t)UINT32_MAX) || (bitmap_bytes > (size_t)UINT32_MAX)) {
        return MEMPOOL_ERR_INVALID_SIZE;
    }

    pool->pool_buffer_start = pool_buffer;
    pool->bitmap            = (uint8_t *)pool_buffer;
    pool->bitmap_bytes      = (uint32_t)bitmap_bytes;
    pool->blocks_start      = (void *)((uint8_t *)pool_buffer + blocks_offset);

    pool->block_size   = (uint32_t)aligned_block_size;
    pool->total_blocks = (uint32_t)n;
    pool->free_blocks  = (uint32_t)n;
    pool->alignment    = (uint32_t)alignment;

    pool->stats.total_blocks = pool->total_blocks;
    pool->stats.used_blocks  = 0U;
    pool->stats.free_blocks  = pool->total_blocks;
    pool->stats.peak_usage   = 0U;
    pool->stats.alloc_count  = 0U;
    pool->stats.free_count   = 0U;
    pool->stats.block_size   = pool->block_size;

    pool->sync.lock       = NULL;
    pool->sync.unlock     = NULL;
    pool->sync.user_ctx   = NULL;
    pool->sync_enabled    = false;
    pool->initialized     = true;

    memset(pool->bitmap, 0, bitmap_bytes);

    uint8_t    *current   = (uint8_t *)pool->blocks_start;
    free_node_t *prev_node = NULL;
    for (uint32_t i = 0U; i < pool->total_blocks; i++) {
        free_node_t *node = (free_node_t *)current;
        node->next = prev_node;
        prev_node  = node;
        current   += aligned_block_size;
    }
    pool->free_list = (void *)prev_node;

    *pool_out = pool;
    return result;
}

mempool_error_t mempool_alloc(mempool_t *pool, void **block)
{
    mempool_error_t result = MEMPOOL_OK;
    free_node_t    *node   = NULL;

    if ((pool == NULL) || (block == NULL)) {
        return MEMPOOL_ERR_NULL_PTR;
    }
    if (!pool->initialized) {
        return MEMPOOL_ERR_NOT_INITIALIZED;
    }

    mempool_internal_lock(pool);

    if (pool->free_list == NULL) {
        result = MEMPOOL_ERR_OUT_OF_MEMORY;
    } else if (pool->free_blocks == 0U) {
        result = MEMPOOL_ERR_OUT_OF_MEMORY;
    } else {
        uint32_t index;
        uint32_t byte_index;
        uint32_t bit_index;
        uint8_t  mask;

        node            = (free_node_t *)pool->free_list;
        pool->free_list = node->next;
        pool->free_blocks--;

        pool->stats.alloc_count++;
        pool->stats.used_blocks = pool->total_blocks - pool->free_blocks;
        pool->stats.free_blocks = pool->free_blocks;

        if (pool->stats.used_blocks > pool->stats.peak_usage) {
            pool->stats.peak_usage = pool->stats.used_blocks;
        }

        index      = mempool_block_index(pool, (const void *)node);
        byte_index = index / 8U;
        bit_index  = index % 8U;
        mask       = (uint8_t)(1U << bit_index);

        if (byte_index < pool->bitmap_bytes) {
            pool->bitmap[byte_index] =
                (uint8_t)(pool->bitmap[byte_index] | mask);
        }

        *block = (void *)node;
    }

    mempool_internal_unlock(pool);
    return result;
}

mempool_error_t mempool_free(mempool_t *pool, void *block)
{
    if ((pool == NULL) || (block == NULL)) {
        return MEMPOOL_ERR_NULL_PTR;
    }
    if (!pool->initialized) {
        return MEMPOOL_ERR_NOT_INITIALIZED;
    }

    uintptr_t blocks_start_addr = (uintptr_t)pool->blocks_start;
    uintptr_t blocks_end_addr   = blocks_start_addr +
                                  ((uintptr_t)pool->total_blocks *
                                   (uintptr_t)pool->block_size);
    uintptr_t block_addr        = (uintptr_t)block;

    if ((block_addr < blocks_start_addr) || (block_addr >= blocks_end_addr)) {
        return MEMPOOL_ERR_INVALID_BLOCK;
    }

    uintptr_t offset = block_addr - blocks_start_addr;
    if ((offset % (uintptr_t)pool->block_size) != 0U) {
        return MEMPOOL_ERR_INVALID_BLOCK;
    }

    mempool_internal_lock(pool);

    mempool_error_t result = MEMPOOL_OK;
    uint32_t index      = mempool_block_index(pool, block);
    uint32_t byte_index = index / 8U;
    uint32_t bit_index  = index % 8U;
    uint8_t  mask       = (uint8_t)(1U << bit_index);

    if (byte_index >= pool->bitmap_bytes) {
        result = MEMPOOL_ERR_INVALID_BLOCK;
    } else {
        uint8_t byte = pool->bitmap[byte_index];

        if ((byte & mask) == 0U) {
            result = MEMPOOL_ERR_DOUBLE_FREE;
        } else {
            free_node_t *node = (free_node_t *)block;
            node->next        = (free_node_t *)pool->free_list;
            pool->free_list   = (void *)node;

            if (pool->free_blocks < pool->total_blocks) {
                pool->free_blocks++;
            }

            pool->stats.free_count++;
            pool->stats.used_blocks = pool->total_blocks - pool->free_blocks;
            pool->stats.free_blocks = pool->free_blocks;

            pool->bitmap[byte_index] =
                (uint8_t)(byte & (uint8_t)(~mask));
        }
    }

    mempool_internal_unlock(pool);
    return result;
}

mempool_error_t mempool_get_stats(const mempool_t *pool,
                                  mempool_stats_t *stats)
{
    if ((pool == NULL) || (stats == NULL)) {
        return MEMPOOL_ERR_NULL_PTR;
    }
    if (!pool->initialized) {
        return MEMPOOL_ERR_NOT_INITIALIZED;
    }

    mempool_internal_lock(pool);
    *stats = pool->stats;
    mempool_internal_unlock(pool);

    return MEMPOOL_OK;
}
/* cppcheck-suppress unusedFunction
 * Public API; may be unused within this translation unit but used by external code.
 */
mempool_error_t mempool_reset(mempool_t *pool)
{
    if (pool == NULL) {
        return MEMPOOL_ERR_NULL_PTR;
    }
    if (!pool->initialized) {
        return MEMPOOL_ERR_NOT_INITIALIZED;
    }

    mempool_internal_lock(pool);

    memset(pool->bitmap, 0, (size_t)pool->bitmap_bytes);

    uint8_t    *current   = (uint8_t *)pool->blocks_start;
    free_node_t *prev_node = NULL;
    for (uint32_t i = 0U; i < pool->total_blocks; i++) {
        free_node_t *node = (free_node_t *)current;
        node->next = prev_node;
        prev_node  = node;
        current   += pool->block_size;
    }
    pool->free_list   = (void *)prev_node;
    pool->free_blocks = pool->total_blocks;

    pool->stats.total_blocks = pool->total_blocks;
    pool->stats.used_blocks  = 0U;
    pool->stats.free_blocks  = pool->total_blocks;
    pool->stats.peak_usage   = 0U;
    pool->stats.alloc_count  = 0U;
    pool->stats.free_count   = 0U;
    pool->stats.block_size   = pool->block_size;

    mempool_internal_unlock(pool);
    return MEMPOOL_OK;
}


/* cppcheck-suppress unusedFunction
 * Public API; may be unused within this translation unit but used by external code.
 */
bool mempool_contains(const mempool_t *pool, const void *ptr)
{
    if ((pool == NULL) || (ptr == NULL) || !pool->initialized) {
        return false;
    }

    uintptr_t blocks_start_addr = (uintptr_t)pool->blocks_start;
    uintptr_t blocks_end_addr   = blocks_start_addr +
                                  ((uintptr_t)pool->total_blocks *
                                   (uintptr_t)pool->block_size);
    uintptr_t p                 = (uintptr_t)ptr;

    return (p >= blocks_start_addr) && (p < blocks_end_addr);
}

const char *mempool_strerror(mempool_error_t error)
{
    const char *msg;

    switch (error) {
        case MEMPOOL_OK:
            msg = "Success";
            break;
        case MEMPOOL_ERR_NULL_PTR:
            msg = "Null pointer";
            break;
        case MEMPOOL_ERR_INVALID_SIZE:
            msg = "Invalid size";
            break;
        case MEMPOOL_ERR_OUT_OF_MEMORY:
            msg = "Out of memory";
            break;
        case MEMPOOL_ERR_INVALID_BLOCK:
            msg = "Invalid block";
            break;
        case MEMPOOL_ERR_ALIGNMENT:
            msg = "Alignment error";
            break;
        case MEMPOOL_ERR_DOUBLE_FREE:
            msg = "Double free detected";
            break;
        case MEMPOOL_ERR_NOT_INITIALIZED:
            msg = "Pool not initialized";
            break;
        default:
            msg = "Unknown error";
            break;
    }

    return msg;
}

/* cppcheck-suppress unusedFunction
 * Public API; may be unused within this translation unit but used by external code.
 */
mempool_error_t mempool_set_sync(mempool_t      *pool,
                                 mempool_lock_fn lock,
                                 mempool_unlock_fn unlock,
                                 void           *user_ctx)
{
    if (pool == NULL) {
        return MEMPOOL_ERR_NULL_PTR;
    }
    if (!pool->initialized) {
        return MEMPOOL_ERR_NOT_INITIALIZED;
    }

    pool->sync.lock     = lock;
    pool->sync.unlock   = unlock;
    pool->sync.user_ctx = user_ctx;

    pool->sync_enabled = (lock != NULL) && (unlock != NULL);
    return MEMPOOL_OK;
}
