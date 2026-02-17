#include "mempool.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal                                                            */
/* ------------------------------------------------------------------ */

typedef struct free_node {
    struct free_node *next;
} free_node_t;

#define MEMPOOL_MAGIC ((uint16_t)0xA5C3U)

struct mempool {
    uint16_t  magic;
    uint8_t   block_shift;   /* log2(block_size) when power-of-2, else 0 */

    void     *free_list;
    void     *blocks_start;
    void     *blocks_end;    /* one past last valid block byte */

    uint32_t  block_size;
    uint32_t  total_blocks;

#if MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
    uint8_t  *bitmap;
    uint32_t  bitmap_bytes;
#endif

#if MEMPOOL_ENABLE_STATS
    mempool_stats_t stats;
#endif
};

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(MEMPOOL_STATE_SIZE >= sizeof(struct mempool),
               "MEMPOOL_STATE_SIZE too small — increase it");
#endif

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int mp_is_pow2(size_t v)
{
    return (v > 0U) && ((v & (v - 1U)) == 0U);
}

static size_t mp_align_up(size_t v, size_t a)
{
    size_t m = a - 1U;
    return (v + m) & ~m;
}

static uint8_t mp_log2(uint32_t v)
{
    uint8_t s = 0U;
    while (v > 1U) { v >>= 1U; s++; }
    return s;
}

static uint32_t mp_block_idx(const mempool_t *p, const void *blk)
{
    uintptr_t off = (uintptr_t)blk - (uintptr_t)p->blocks_start;
    if (p->block_shift != 0U) {
        return (uint32_t)(off >> p->block_shift);
    }
    return (uint32_t)(off / (uintptr_t)p->block_size);
}

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
    mempool_t *p;
    size_t abs;  /* aligned block size */
    size_t n;

    if ((state_buffer == NULL) || (pool_buffer == NULL) || (pool_out == NULL)) {
        return MEMPOOL_ERR_NULL_PTR;
    }
    if (state_buffer_size < sizeof(struct mempool)) {
        return MEMPOOL_ERR_INVALID_SIZE;
    }
    if ((pool_buffer_size == 0U) || (block_size == 0U)) {
        return MEMPOOL_ERR_INVALID_SIZE;
    }
    if (!mp_is_pow2(alignment)) {
        return MEMPOOL_ERR_ALIGNMENT;
    }
    if ((((uintptr_t)pool_buffer) & (alignment - 1U)) != 0U) {
        return MEMPOOL_ERR_ALIGNMENT;
    }
    if (block_size < sizeof(free_node_t)) {
        return MEMPOOL_ERR_INVALID_SIZE;
    }

    p = (mempool_t *)state_buffer;
    abs = mp_align_up(block_size, alignment);

#if MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
    {
        size_t max_n = pool_buffer_size / abs;
        size_t bm_bytes  = 0U;
        size_t blk_off   = 0U;
        int    found     = 0;

        if (max_n == 0U) { return MEMPOOL_ERR_INVALID_SIZE; }
        n = max_n;

        while ((n > 0U) && !found) {
            size_t bm_end, pad = 0U, req;
            bm_bytes = (n + 7U) / 8U;
            bm_end   = bm_bytes;
            if (alignment > 1U) {
                size_t mod = bm_end & (alignment - 1U);
                if (mod != 0U) { pad = alignment - mod; }
            }
            blk_off = bm_end + pad;
            req     = blk_off + (n * abs);
            if (req <= pool_buffer_size) { found = 1; } else { n--; }
        }
        if (!found || (n == 0U))              { return MEMPOOL_ERR_INVALID_SIZE; }
        if ((n > (size_t)UINT32_MAX) ||
            (bm_bytes > (size_t)UINT32_MAX))  { return MEMPOOL_ERR_INVALID_SIZE; }

        p->bitmap       = (uint8_t *)pool_buffer;
        p->bitmap_bytes = (uint32_t)bm_bytes;
        p->blocks_start = (void *)((uint8_t *)pool_buffer + blk_off);
        memset(p->bitmap, 0, bm_bytes);
    }
#else
    n = pool_buffer_size / abs;
    if (n == 0U)                { return MEMPOOL_ERR_INVALID_SIZE; }
    if (n > (size_t)UINT32_MAX) { return MEMPOOL_ERR_INVALID_SIZE; }
    p->blocks_start = pool_buffer;
#endif

    p->magic        = MEMPOOL_MAGIC;
    p->block_size   = (uint32_t)abs;
    p->total_blocks = (uint32_t)n;
    p->blocks_end   = (void *)((uint8_t *)p->blocks_start +
                                (n * abs));
    p->block_shift  = mp_is_pow2(abs) ? mp_log2((uint32_t)abs) : 0U;

#if MEMPOOL_ENABLE_STATS
    p->stats.total_blocks = (uint32_t)n;
    p->stats.used_blocks  = 0U;
    p->stats.free_blocks  = (uint32_t)n;
    p->stats.peak_usage   = 0U;
    p->stats.alloc_count  = 0U;
    p->stats.free_count   = 0U;
    p->stats.block_size   = (uint32_t)abs;
#endif

    /* Build free list */
    {
        uint8_t     *cur  = (uint8_t *)p->blocks_start;
        free_node_t *prev = NULL;
        uint32_t i;
        for (i = 0U; i < (uint32_t)n; i++) {
            free_node_t *nd = (free_node_t *)(void *)cur;
            nd->next = prev;
            prev     = nd;
            cur     += abs;
        }
        p->free_list = (void *)prev;
    }

    *pool_out = p;
    return MEMPOOL_OK;
}

mempool_error_t mempool_alloc(mempool_t *pool, void **block)
{
    free_node_t *nd;

    if ((pool == NULL) || (block == NULL)) {
        return MEMPOOL_ERR_NULL_PTR;
    }
    if (pool->magic != MEMPOOL_MAGIC) {
        return MEMPOOL_ERR_NOT_INITIALIZED;
    }

    MEMPOOL_LOCK();

    if (pool->free_list == NULL) {
        MEMPOOL_UNLOCK();
        return MEMPOOL_ERR_OUT_OF_MEMORY;
    }

    nd              = (free_node_t *)pool->free_list;
    pool->free_list = nd->next;

#if MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
    {
        uint32_t idx = mp_block_idx(pool, (const void *)nd);
        uint32_t bi  = idx >> 3U;
        uint8_t  m   = (uint8_t)(1U << (idx & 7U));
        if (bi < pool->bitmap_bytes) {
            pool->bitmap[bi] = (uint8_t)(pool->bitmap[bi] | m);
        }
    }
#endif

#if MEMPOOL_ENABLE_STATS
    pool->stats.alloc_count++;
    pool->stats.used_blocks++;
    pool->stats.free_blocks--;
    if (pool->stats.used_blocks > pool->stats.peak_usage) {
        pool->stats.peak_usage = pool->stats.used_blocks;
    }
#endif

    *block = (void *)nd;
    MEMPOOL_UNLOCK();
    return MEMPOOL_OK;
}

mempool_error_t mempool_free(mempool_t *pool, void *block)
{
    uintptr_t ba, offset;

    if ((pool == NULL) || (block == NULL)) {
        return MEMPOOL_ERR_NULL_PTR;
    }
    if (pool->magic != MEMPOOL_MAGIC) {
        return MEMPOOL_ERR_NOT_INITIALIZED;
    }

    ba = (uintptr_t)block;

    /* Range check using pre-computed blocks_end — no multiply */
    if ((ba < (uintptr_t)pool->blocks_start) ||
        (ba >= (uintptr_t)pool->blocks_end)) {
        return MEMPOOL_ERR_INVALID_BLOCK;
    }

    /* Alignment check */
    offset = ba - (uintptr_t)pool->blocks_start;
    if (pool->block_shift != 0U) {
        if ((offset & (((uintptr_t)1U << pool->block_shift) - 1U)) != 0U) {
            return MEMPOOL_ERR_INVALID_BLOCK;
        }
    } else {
        if ((offset % (uintptr_t)pool->block_size) != 0U) {
            return MEMPOOL_ERR_INVALID_BLOCK;
        }
    }

    MEMPOOL_LOCK();

#if MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
    {
        uint32_t idx = mp_block_idx(pool, block);
        uint32_t bi  = idx >> 3U;
        uint8_t  m   = (uint8_t)(1U << (idx & 7U));

        if (bi >= pool->bitmap_bytes) {
            MEMPOOL_UNLOCK();
            return MEMPOOL_ERR_INVALID_BLOCK;
        }
        if ((pool->bitmap[bi] & m) == 0U) {
            MEMPOOL_UNLOCK();
            return MEMPOOL_ERR_DOUBLE_FREE;
        }
        pool->bitmap[bi] = (uint8_t)(pool->bitmap[bi] & (uint8_t)(~m));
    }
#endif

    {
        free_node_t *nd = (free_node_t *)block;
        nd->next        = (free_node_t *)pool->free_list;
        pool->free_list = (void *)nd;
    }

#if MEMPOOL_ENABLE_STATS
    pool->stats.free_count++;
    if (pool->stats.used_blocks > 0U) {
        pool->stats.used_blocks--;
    }
    pool->stats.free_blocks++;
#endif

    MEMPOOL_UNLOCK();
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

    MEMPOOL_LOCK();
    *stats = pool->stats;
    MEMPOOL_UNLOCK();
    return MEMPOOL_OK;
}
#endif

mempool_error_t mempool_reset(mempool_t *pool)
{
    if (pool == NULL) {
        return MEMPOOL_ERR_NULL_PTR;
    }
    if (pool->magic != MEMPOOL_MAGIC) {
        return MEMPOOL_ERR_NOT_INITIALIZED;
    }

    MEMPOOL_LOCK();

#if MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
    memset(pool->bitmap, 0, (size_t)pool->bitmap_bytes);
#endif

    {
        uint8_t     *cur  = (uint8_t *)pool->blocks_start;
        free_node_t *prev = NULL;
        uint32_t i;
        for (i = 0U; i < pool->total_blocks; i++) {
            free_node_t *nd = (free_node_t *)(void *)cur;
            nd->next = prev;
            prev     = nd;
            cur     += pool->block_size;
        }
        pool->free_list = (void *)prev;
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

    MEMPOOL_UNLOCK();
    return MEMPOOL_OK;
}

int mempool_contains(const mempool_t *pool, const void *ptr)
{
    uintptr_t p;
    if ((pool == NULL) || (ptr == NULL) || (pool->magic != MEMPOOL_MAGIC)) {
        return 0;
    }
    p = (uintptr_t)ptr;
    return (p >= (uintptr_t)pool->blocks_start) &&
           (p <  (uintptr_t)pool->blocks_end);
}

#if MEMPOOL_ENABLE_STRERROR
const char *mempool_strerror(mempool_error_t error)
{
    switch (error) {
        case MEMPOOL_OK:                 return "Success";
        case MEMPOOL_ERR_NULL_PTR:       return "Null pointer";
        case MEMPOOL_ERR_INVALID_SIZE:   return "Invalid size";
        case MEMPOOL_ERR_OUT_OF_MEMORY:  return "Out of memory";
        case MEMPOOL_ERR_INVALID_BLOCK:  return "Invalid block";
        case MEMPOOL_ERR_ALIGNMENT:      return "Alignment error";
        case MEMPOOL_ERR_DOUBLE_FREE:    return "Double free detected";
        case MEMPOOL_ERR_NOT_INITIALIZED:return "Pool not initialized";
        default:                         return "Unknown error";
    }
}
#endif
