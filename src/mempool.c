#include "mempool.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal types                                                      */
/* ------------------------------------------------------------------ */

typedef struct free_node_t {
    struct free_node_t *next;
} free_node_t;

#define MEMPOOL_MAGIC 0xA5C3U

struct mempool {
    uint16_t magic;

    void    *pool_buffer_start;
    void    *blocks_start;
    void    *free_list;

    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t alignment;
    uint8_t  block_size_shift; /* log2(block_size) if power-of-2, else 0 */

#if MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
    uint8_t *bitmap;
    uint32_t bitmap_bytes;
#endif

#if MEMPOOL_ENABLE_STATS
    mempool_stats_t stats;
#endif

#if MEMPOOL_ENABLE_SYNC
    mempool_sync_t sync;
    bool           sync_enabled;
#endif
};

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(MEMPOOL_STATE_SIZE >= sizeof(struct mempool),
               "MEMPOOL_STATE_SIZE too small for mempool state");
#endif

/* ------------------------------------------------------------------ */
/* Fast helpers                                                        */
/* ------------------------------------------------------------------ */

static bool is_power_of_two(size_t value)
{
    return (value > 0U) && ((value & (value - 1U)) == 0U);
}

static size_t align_up(size_t value, size_t alignment)
{
    size_t mask = alignment - 1U;
    return (value + mask) & ~mask;
}

static uint8_t compute_shift(uint32_t value)
{
    uint8_t s = 0U;
    uint32_t v = value;
    while (v > 1U) {
        v >>= 1U;
        s++;
    }
    return s;
}

/* Block index via shift when possible, division as fallback */
static uint32_t mempool_block_index(const mempool_t *pool, const void *ptr)
{
    uintptr_t off = (uintptr_t)ptr - (uintptr_t)pool->blocks_start;
    if (pool->block_size_shift != 0U) {
        return (uint32_t)(off >> pool->block_size_shift);
    }
    return (uint32_t)(off / (uintptr_t)pool->block_size);
}

/* ------------------------------------------------------------------ */
/* Sync helpers — compiled out when MEMPOOL_ENABLE_SYNC == 0           */
/* ------------------------------------------------------------------ */

#if MEMPOOL_ENABLE_SYNC
static void mempool_lock(const mempool_t *pool)
{
    if (pool->sync_enabled && (pool->sync.lock != NULL)) {
        pool->sync.lock(pool->sync.user_ctx);
    }
}

static void mempool_unlock(const mempool_t *pool)
{
    if (pool->sync_enabled && (pool->sync.unlock != NULL)) {
        pool->sync.unlock(pool->sync.user_ctx);
    }
}
#else
#define mempool_lock(p)   ((void)0)
#define mempool_unlock(p) ((void)0)
#endif

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

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

#if MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
    /* Bitmap lives at the start of pool_buffer, blocks follow */
    size_t max_blocks   = pool_buffer_size / aligned_block_size;
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

    pool->bitmap       = (uint8_t *)pool_buffer;
    pool->bitmap_bytes = (uint32_t)bitmap_bytes;
    pool->blocks_start = (void *)((uint8_t *)pool_buffer + blocks_offset);
    memset(pool->bitmap, 0, bitmap_bytes);
#else
    /* No bitmap — entire pool_buffer is block storage */
    size_t n = pool_buffer_size / aligned_block_size;
    if (n == 0U) {
        return MEMPOOL_ERR_INVALID_SIZE;
    }
    if (n > (size_t)UINT32_MAX) {
        return MEMPOOL_ERR_INVALID_SIZE;
    }
    pool->blocks_start = pool_buffer;
#endif

    pool->pool_buffer_start = pool_buffer;
    pool->block_size   = (uint32_t)aligned_block_size;
    pool->total_blocks = (uint32_t)n;
    pool->free_blocks  = (uint32_t)n;
    pool->alignment    = (uint32_t)alignment;
    pool->magic        = MEMPOOL_MAGIC;

    /* Pre-compute shift for power-of-2 block sizes */
    if (is_power_of_two(aligned_block_size)) {
        pool->block_size_shift = compute_shift((uint32_t)aligned_block_size);
    } else {
        pool->block_size_shift = 0U;
    }

#if MEMPOOL_ENABLE_STATS
    pool->stats.total_blocks = pool->total_blocks;
    pool->stats.used_blocks  = 0U;
    pool->stats.free_blocks  = pool->total_blocks;
    pool->stats.peak_usage   = 0U;
    pool->stats.alloc_count  = 0U;
    pool->stats.free_count   = 0U;
    pool->stats.block_size   = pool->block_size;
#endif

#if MEMPOOL_ENABLE_SYNC
    pool->sync.lock       = NULL;
    pool->sync.unlock     = NULL;
    pool->sync.user_ctx   = NULL;
    pool->sync_enabled    = false;
#endif

    /* Build free list (LIFO — last block is head for cache-friendly pops) */
    {
        uint8_t     *current   = (uint8_t *)pool->blocks_start;
        free_node_t *prev_node = NULL;
        uint32_t i;
        for (i = 0U; i < pool->total_blocks; i++) {
            free_node_t *node = (free_node_t *)(void *)current;
            node->next = prev_node;
            prev_node  = node;
            current   += aligned_block_size;
        }
        pool->free_list = (void *)prev_node;
    }

    *pool_out = pool;
    return MEMPOOL_OK;
}

mempool_error_t mempool_alloc(mempool_t *pool, void **block)
{
    free_node_t *node;

    if ((pool == NULL) || (block == NULL)) {
        return MEMPOOL_ERR_NULL_PTR;
    }
    if (pool->magic != MEMPOOL_MAGIC) {
        return MEMPOOL_ERR_NOT_INITIALIZED;
    }

    mempool_lock(pool);

    if (pool->free_list == NULL) {
        mempool_unlock(pool);
        return MEMPOOL_ERR_OUT_OF_MEMORY;
    }

    node            = (free_node_t *)pool->free_list;
    pool->free_list = node->next;
    pool->free_blocks--;

#if MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
    {
        uint32_t index      = mempool_block_index(pool, (const void *)node);
        uint32_t byte_index = index >> 3U;
        uint8_t  mask       = (uint8_t)(1U << (index & 7U));
        if (byte_index < pool->bitmap_bytes) {
            pool->bitmap[byte_index] =
                (uint8_t)(pool->bitmap[byte_index] | mask);
        }
    }
#endif

#if MEMPOOL_ENABLE_STATS
    {
        uint32_t used = pool->total_blocks - pool->free_blocks;
        pool->stats.alloc_count++;
        pool->stats.used_blocks = used;
        pool->stats.free_blocks = pool->free_blocks;
        if (used > pool->stats.peak_usage) {
            pool->stats.peak_usage = used;
        }
    }
#endif

    *block = (void *)node;
    mempool_unlock(pool);
    return MEMPOOL_OK;
}

mempool_error_t mempool_free(mempool_t *pool, void *block)
{
    uintptr_t blocks_start_addr;
    uintptr_t blocks_end_addr;
    uintptr_t block_addr;
    uintptr_t offset;

    if ((pool == NULL) || (block == NULL)) {
        return MEMPOOL_ERR_NULL_PTR;
    }
    if (pool->magic != MEMPOOL_MAGIC) {
        return MEMPOOL_ERR_NOT_INITIALIZED;
    }

    blocks_start_addr = (uintptr_t)pool->blocks_start;
    blocks_end_addr   = blocks_start_addr +
                        ((uintptr_t)pool->total_blocks *
                         (uintptr_t)pool->block_size);
    block_addr        = (uintptr_t)block;

    if ((block_addr < blocks_start_addr) || (block_addr >= blocks_end_addr)) {
        return MEMPOOL_ERR_INVALID_BLOCK;
    }

    offset = block_addr - blocks_start_addr;
    if (pool->block_size_shift != 0U) {
        /* Fast check: low bits must be zero */
        if ((offset & (((uintptr_t)1U << pool->block_size_shift) - 1U)) != 0U) {
            return MEMPOOL_ERR_INVALID_BLOCK;
        }
    } else {
        if ((offset % (uintptr_t)pool->block_size) != 0U) {
            return MEMPOOL_ERR_INVALID_BLOCK;
        }
    }

    mempool_lock(pool);

#if MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
    {
        uint32_t index      = mempool_block_index(pool, block);
        uint32_t byte_index = index >> 3U;
        uint8_t  mask       = (uint8_t)(1U << (index & 7U));

        if (byte_index >= pool->bitmap_bytes) {
            mempool_unlock(pool);
            return MEMPOOL_ERR_INVALID_BLOCK;
        }

        if ((pool->bitmap[byte_index] & mask) == 0U) {
            mempool_unlock(pool);
            return MEMPOOL_ERR_DOUBLE_FREE;
        }

        pool->bitmap[byte_index] =
            (uint8_t)(pool->bitmap[byte_index] & (uint8_t)(~mask));
    }
#endif

    {
        free_node_t *node = (free_node_t *)block;
        node->next        = (free_node_t *)pool->free_list;
        pool->free_list   = (void *)node;
    }

    if (pool->free_blocks < pool->total_blocks) {
        pool->free_blocks++;
    }

#if MEMPOOL_ENABLE_STATS
    pool->stats.free_count++;
    pool->stats.used_blocks = pool->total_blocks - pool->free_blocks;
    pool->stats.free_blocks = pool->free_blocks;
#endif

    mempool_unlock(pool);
    return MEMPOOL_OK;
}

#if MEMPOOL_ENABLE_STATS
mempool_error_t mempool_get_stats(const mempool_t *pool,
                                  mempool_stats_t *stats)
{
    if ((pool == NULL) || (stats == NULL)) {
        return MEMPOOL_ERR_NULL_PTR;
    }
    if (pool->magic != MEMPOOL_MAGIC) {
        return MEMPOOL_ERR_NOT_INITIALIZED;
    }

    mempool_lock(pool);
    *stats = pool->stats;
    mempool_unlock(pool);

    return MEMPOOL_OK;
}
#endif

/* cppcheck-suppress unusedFunction
 * Public API; may be unused within this translation unit but used by external code.
 */
mempool_error_t mempool_reset(mempool_t *pool)
{
    if (pool == NULL) {
        return MEMPOOL_ERR_NULL_PTR;
    }
    if (pool->magic != MEMPOOL_MAGIC) {
        return MEMPOOL_ERR_NOT_INITIALIZED;
    }

    mempool_lock(pool);

#if MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
    memset(pool->bitmap, 0, (size_t)pool->bitmap_bytes);
#endif

    {
        uint8_t     *current   = (uint8_t *)pool->blocks_start;
        free_node_t *prev_node = NULL;
        uint32_t i;
        for (i = 0U; i < pool->total_blocks; i++) {
            free_node_t *node = (free_node_t *)(void *)current;
            node->next = prev_node;
            prev_node  = node;
            current   += pool->block_size;
        }
        pool->free_list   = (void *)prev_node;
        pool->free_blocks = pool->total_blocks;
    }

#if MEMPOOL_ENABLE_STATS
    pool->stats.total_blocks = pool->total_blocks;
    pool->stats.used_blocks  = 0U;
    pool->stats.free_blocks  = pool->total_blocks;
    pool->stats.peak_usage   = 0U;
    pool->stats.alloc_count  = 0U;
    pool->stats.free_count   = 0U;
    pool->stats.block_size   = pool->block_size;
#endif

    mempool_unlock(pool);
    return MEMPOOL_OK;
}


/* cppcheck-suppress unusedFunction
 * Public API; may be unused within this translation unit but used by external code.
 */
bool mempool_contains(const mempool_t *pool, const void *ptr)
{
    uintptr_t blocks_start_addr;
    uintptr_t blocks_end_addr;
    uintptr_t p;

    if ((pool == NULL) || (ptr == NULL) || (pool->magic != MEMPOOL_MAGIC)) {
        return false;
    }

    blocks_start_addr = (uintptr_t)pool->blocks_start;
    blocks_end_addr   = blocks_start_addr +
                        ((uintptr_t)pool->total_blocks *
                         (uintptr_t)pool->block_size);
    p                 = (uintptr_t)ptr;

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

#if MEMPOOL_ENABLE_SYNC
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
    if (pool->magic != MEMPOOL_MAGIC) {
        return MEMPOOL_ERR_NOT_INITIALIZED;
    }

    pool->sync.lock     = lock;
    pool->sync.unlock   = unlock;
    pool->sync.user_ctx = user_ctx;

    pool->sync_enabled = (lock != NULL) && (unlock != NULL);
    return MEMPOOL_OK;
}
#endif
