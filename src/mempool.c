#include "mempool.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal types                                                      */
/* ------------------------------------------------------------------ */

typedef struct free_node {
    struct free_node *next;
} free_node_t;

#define MEMPOOL_MAGIC ((uint16_t)0xA5C3U)

struct mempool {
    uint16_t  magic;
    uint8_t   block_shift;   /* log2(block_size) when power-of-2, else 0 */
    uint8_t   _pad0;

    void     *free_list;
    void     *blocks_start;
    void     *blocks_end;    /* one past last valid block byte */

    uint32_t  block_size;    /* aligned per-block stride (includes canary if GUARD ON) */
    uint32_t  total_blocks;

#if MEMPOOL_ENABLE_GUARD
    uint32_t  user_block_size; /* user-visible bytes per block (before canary) */
#endif

#if MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
    uint8_t  *bitmap;
    uint32_t  bitmap_bytes;
#endif

#if MEMPOOL_ENABLE_TAGS
    uint32_t *tags;          /* per-block 32-bit annotation; lives in pool_buffer */
#endif

#if MEMPOOL_ENABLE_OOM_HOOK
    mempool_oom_hook_t oom_hook;
    void              *oom_user_data;
#endif

#if MEMPOOL_ENABLE_ISR_FREE
    void    *isr_queue[MEMPOOL_ISR_QUEUE_CAPACITY];
    uint8_t  isr_head;
    uint8_t  isr_tail;
    uint8_t  isr_count;
#endif

#if MEMPOOL_ENABLE_STATS
    mempool_stats_t stats;
#endif
};

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(MEMPOOL_STATE_SIZE >= sizeof(struct mempool),
               "MEMPOOL_STATE_SIZE too small — increase it in mempool_cfg.h");
#endif

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
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

/* Return the number of user-visible bytes per block. */
static size_t mp_user_size(const mempool_t *p)
{
#if MEMPOOL_ENABLE_GUARD
    return (size_t)p->user_block_size;
#else
    return (size_t)p->block_size;
#endif
}

/* ------------------------------------------------------------------ */
/* Pool buffer layout calculation                                      */
/* ------------------------------------------------------------------ */

/*
 * Compute the offset of the block array start within pool_buffer and the
 * maximum number of blocks that fit, given a per-block stride of `stride`.
 *
 * Pool buffer layout (each section only present when the feature is ON):
 *
 *   [bitmap: ceil(n/8) bytes]  [alignment padding]
 *   [tags:   n * 4 bytes    ]  [alignment padding]
 *   [blocks: n * stride     ]
 *
 * Because both bitmap and tags overhead depend on n, we iterate downward
 * from max_n until a consistent solution is found.  In practice this loop
 * almost always terminates in 0–1 iterations.
 */
static int mp_layout(size_t pool_buffer_size, size_t stride, size_t alignment,
                     uint32_t *n_out, size_t *blk_off_out)
{
    size_t max_n;
    size_t n;

    if (stride == 0U) { return 0; }
    max_n = pool_buffer_size / stride;
    if (max_n == 0U || max_n > (size_t)UINT32_MAX) { return 0; }

    for (n = max_n; n > 0U; n--) {
        size_t off = 0U;

#if MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
        off = mp_align_up(off + (n + 7U) / 8U, alignment);
#endif
#if MEMPOOL_ENABLE_TAGS
        off = mp_align_up(off + n * sizeof(uint32_t), alignment);
#endif
        if (off + n * stride <= pool_buffer_size) {
            *n_out       = (uint32_t)n;
            *blk_off_out = off;
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* ISR queue helper (called with task lock already held)              */
/* ------------------------------------------------------------------ */

#if MEMPOOL_ENABLE_ISR_FREE
static void mp_flush_isr_queue(mempool_t *pool)
{
    while (pool->isr_count > 0U) {
        free_node_t *nd;
        void        *blk;

        MEMPOOL_ISR_LOCK();
        blk             = pool->isr_queue[pool->isr_head];
        pool->isr_head  = (uint8_t)((pool->isr_head + 1U) %
                                    MEMPOOL_ISR_QUEUE_CAPACITY);
        pool->isr_count--;
        MEMPOOL_ISR_UNLOCK();

        nd       = (free_node_t *)blk;
        nd->next = (free_node_t *)pool->free_list;
        pool->free_list = (void *)nd;

#if MEMPOOL_ENABLE_STATS
        pool->stats.free_count++;
        if (pool->stats.used_blocks > 0U) { pool->stats.used_blocks--; }
        pool->stats.free_blocks++;
#endif
    }
}
#endif /* MEMPOOL_ENABLE_ISR_FREE */

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
    size_t     stride;
    size_t     blk_off = 0U;
    uint32_t   n;

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

    /* Per-block stride: user area + optional post-canary, aligned up */
#if MEMPOOL_ENABLE_GUARD
    stride = mp_align_up(block_size + sizeof(uint32_t), alignment);
#else
    stride = mp_align_up(block_size, alignment);
#endif

    if (!mp_layout(pool_buffer_size, stride, alignment, &n, &blk_off)) {
        return MEMPOOL_ERR_INVALID_SIZE;
    }

    p = (mempool_t *)state_buffer;

    /* --- Set up overhead areas in pool_buffer --- */
    {
        size_t cur_off = 0U;

#if MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
        {
            size_t bm_bytes = (n + 7U) / 8U;
            p->bitmap       = (uint8_t *)pool_buffer;
            p->bitmap_bytes = (uint32_t)bm_bytes;
            memset(p->bitmap, 0, bm_bytes);
            cur_off = mp_align_up(bm_bytes, alignment);
        }
#endif

#if MEMPOOL_ENABLE_TAGS
        p->tags = (uint32_t *)((uint8_t *)pool_buffer + cur_off);
        memset(p->tags, 0, (size_t)n * sizeof(uint32_t));
        cur_off = mp_align_up(cur_off + (size_t)n * sizeof(uint32_t), alignment);
#endif
        (void)cur_off; /* suppress warning when neither feature is active */
    }

    /* --- Fill in pool state --- */
    p->magic        = MEMPOOL_MAGIC;
    p->block_size   = (uint32_t)stride;
    p->total_blocks = n;
    p->blocks_start = (void *)((uint8_t *)pool_buffer + blk_off);
    p->blocks_end   = (void *)((uint8_t *)p->blocks_start + (size_t)n * stride);
    p->block_shift  = mp_is_pow2(stride) ? mp_log2((uint32_t)stride) : 0U;

#if MEMPOOL_ENABLE_GUARD
    p->user_block_size = (uint32_t)block_size;
#endif

#if MEMPOOL_ENABLE_OOM_HOOK
    p->oom_hook      = NULL;
    p->oom_user_data = NULL;
#endif

#if MEMPOOL_ENABLE_ISR_FREE
    memset(p->isr_queue, 0, sizeof(p->isr_queue));
    p->isr_head  = 0U;
    p->isr_tail  = 0U;
    p->isr_count = 0U;
#endif

#if MEMPOOL_ENABLE_STATS
    p->stats.total_blocks    = n;
    p->stats.used_blocks     = 0U;
    p->stats.free_blocks     = n;
    p->stats.peak_usage      = 0U;
    p->stats.alloc_count     = 0U;
    p->stats.free_count      = 0U;
    p->stats.block_size      = (uint32_t)stride;
#if MEMPOOL_ENABLE_GUARD
    p->stats.guard_violations = 0U;
#endif
#endif

    /* Build free list (LIFO: last block at head) */
    {
        uint8_t     *cur  = (uint8_t *)p->blocks_start;
        free_node_t *prev = NULL;
        uint32_t     i;
        for (i = 0U; i < n; i++) {
            free_node_t *nd = (free_node_t *)(void *)cur;
            nd->next        = prev;
            prev            = nd;
            cur            += stride;
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

#if MEMPOOL_ENABLE_ISR_FREE
    /* Drain any blocks queued by ISR handlers before checking free list */
    if (pool->isr_count > 0U) {
        mp_flush_isr_queue(pool);
    }
#endif

    if (pool->free_list == NULL) {
#if MEMPOOL_ENABLE_OOM_HOOK
        if (pool->oom_hook != NULL) {
            pool->oom_hook(pool, pool->oom_user_data);
        }
#endif
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

#if MEMPOOL_ENABLE_GUARD
    /* Write post-canary immediately after the user area */
    {
        uint32_t *canary = (uint32_t *)(void *)
                           ((uint8_t *)nd + pool->user_block_size);
        *canary = (uint32_t)MEMPOOL_CANARY_VALUE;
    }
#endif

#if MEMPOOL_ENABLE_POISON
    /* Overwrite user area with alloc-pattern so uninitialised reads stand out */
    memset(nd, (int)(uint8_t)MEMPOOL_ALLOC_POISON_BYTE, mp_user_size(pool));
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

mempool_error_t mempool_alloc_zero(mempool_t *pool, void **block)
{
    mempool_error_t err = mempool_alloc(pool, block);
    if (err == MEMPOOL_OK) {
        memset(*block, 0, mp_user_size(pool));
    }
    return err;
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

    /* Alignment / stride check */
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

#if MEMPOOL_ENABLE_GUARD
    /* Validate post-canary before anything else touches the block */
    {
        const uint32_t *canary = (const uint32_t *)(const void *)
                                 ((const uint8_t *)block + pool->user_block_size);
        if (*canary != (uint32_t)MEMPOOL_CANARY_VALUE) {
#if MEMPOOL_ENABLE_STATS
            pool->stats.guard_violations++;
#endif
            MEMPOOL_UNLOCK();
            return MEMPOOL_ERR_GUARD_CORRUPTED;
        }
    }
#endif

#if MEMPOOL_ENABLE_POISON
    /* Overwrite user area with free-pattern to catch use-after-free */
    memset(block, (int)(uint8_t)MEMPOOL_FREE_POISON_BYTE, mp_user_size(pool));
#endif

    {
        free_node_t *nd = (free_node_t *)block;
        nd->next        = (free_node_t *)pool->free_list;
        pool->free_list = (void *)nd;
    }

#if MEMPOOL_ENABLE_STATS
    pool->stats.free_count++;
    if (pool->stats.used_blocks > 0U) { pool->stats.used_blocks--; }
    pool->stats.free_blocks++;
#endif

    MEMPOOL_UNLOCK();
    return MEMPOOL_OK;
}

#if MEMPOOL_ENABLE_OOM_HOOK
mempool_error_t mempool_set_oom_hook(mempool_t          *pool,
                                     mempool_oom_hook_t  hook,
                                     void               *user_data)
{
    if (pool == NULL) { return MEMPOOL_ERR_NULL_PTR; }
    if (pool->magic != MEMPOOL_MAGIC) { return MEMPOOL_ERR_NOT_INITIALIZED; }

    MEMPOOL_LOCK();
    pool->oom_hook      = hook;
    pool->oom_user_data = user_data;
    MEMPOOL_UNLOCK();
    return MEMPOOL_OK;
}
#endif /* MEMPOOL_ENABLE_OOM_HOOK */

#if MEMPOOL_ENABLE_TAGS
mempool_error_t mempool_set_block_tag(mempool_t *pool, void *block,
                                      uint32_t tag)
{
    uint32_t idx;

    if ((pool == NULL) || (block == NULL)) { return MEMPOOL_ERR_NULL_PTR; }
    if (pool->magic != MEMPOOL_MAGIC) { return MEMPOOL_ERR_NOT_INITIALIZED; }
    if (((uintptr_t)block < (uintptr_t)pool->blocks_start) ||
        ((uintptr_t)block >= (uintptr_t)pool->blocks_end)) {
        return MEMPOOL_ERR_INVALID_BLOCK;
    }

    idx = mp_block_idx(pool, block);

    MEMPOOL_LOCK();
    pool->tags[idx] = tag;
    MEMPOOL_UNLOCK();
    return MEMPOOL_OK;
}

mempool_error_t mempool_get_block_tag(const mempool_t *pool, const void *block,
                                      uint32_t *tag_out)
{
    uint32_t idx;

    if ((pool == NULL) || (block == NULL) || (tag_out == NULL)) {
        return MEMPOOL_ERR_NULL_PTR;
    }
    if (pool->magic != MEMPOOL_MAGIC) { return MEMPOOL_ERR_NOT_INITIALIZED; }
    if (((uintptr_t)block < (uintptr_t)pool->blocks_start) ||
        ((uintptr_t)block >= (uintptr_t)pool->blocks_end)) {
        return MEMPOOL_ERR_INVALID_BLOCK;
    }

    idx = mp_block_idx(pool, block);

    MEMPOOL_LOCK();
    *tag_out = pool->tags[idx];
    MEMPOOL_UNLOCK();
    return MEMPOOL_OK;
}

mempool_error_t mempool_alloc_tagged(mempool_t *pool, void **block,
                                     uint32_t tag)
{
    mempool_error_t err = mempool_alloc(pool, block);
    if (err == MEMPOOL_OK) {
        uint32_t idx = mp_block_idx(pool, *block);
        /* No extra lock needed: caller owns the block, tag array is per-block */
        pool->tags[idx] = tag;
    }
    return err;
}
#endif /* MEMPOOL_ENABLE_TAGS */

#if MEMPOOL_ENABLE_ISR_FREE
mempool_error_t mempool_free_from_isr(mempool_t *pool, void *block)
{
    if ((pool == NULL) || (block == NULL)) { return MEMPOOL_ERR_NULL_PTR; }
    if (pool->magic != MEMPOOL_MAGIC)      { return MEMPOOL_ERR_NOT_INITIALIZED; }

    MEMPOOL_ISR_LOCK();

    if (pool->isr_count >= (uint8_t)MEMPOOL_ISR_QUEUE_CAPACITY) {
        MEMPOOL_ISR_UNLOCK();
        return MEMPOOL_ERR_ISR_QUEUE_FULL;
    }

    pool->isr_queue[pool->isr_tail] = block;
    pool->isr_tail = (uint8_t)((pool->isr_tail + 1U) %
                                MEMPOOL_ISR_QUEUE_CAPACITY);
    pool->isr_count++;

    MEMPOOL_ISR_UNLOCK();
    return MEMPOOL_OK;
}

mempool_error_t mempool_drain_isr_queue(mempool_t *pool)
{
    if (pool == NULL)                 { return MEMPOOL_ERR_NULL_PTR; }
    if (pool->magic != MEMPOOL_MAGIC) { return MEMPOOL_ERR_NOT_INITIALIZED; }

    MEMPOOL_LOCK();
    mp_flush_isr_queue(pool);
    MEMPOOL_UNLOCK();
    return MEMPOOL_OK;
}
#endif /* MEMPOOL_ENABLE_ISR_FREE */

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

mempool_error_t mempool_reset_stats(mempool_t *pool)
{
    if (pool == NULL)                 { return MEMPOOL_ERR_NULL_PTR; }
    if (pool->magic != MEMPOOL_MAGIC) { return MEMPOOL_ERR_NOT_INITIALIZED; }

    MEMPOOL_LOCK();
    pool->stats.alloc_count  = 0U;
    pool->stats.free_count   = 0U;
    pool->stats.peak_usage   = pool->stats.used_blocks; /* reset to current */
#if MEMPOOL_ENABLE_GUARD
    pool->stats.guard_violations = 0U;
#endif
    MEMPOOL_UNLOCK();
    return MEMPOOL_OK;
}
#endif /* MEMPOOL_ENABLE_STATS */

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

#if MEMPOOL_ENABLE_TAGS
    memset(pool->tags, 0, (size_t)pool->total_blocks * sizeof(uint32_t));
#endif

#if MEMPOOL_ENABLE_ISR_FREE
    pool->isr_head  = 0U;
    pool->isr_tail  = 0U;
    pool->isr_count = 0U;
#endif

    {
        uint8_t     *cur  = (uint8_t *)pool->blocks_start;
        free_node_t *prev = NULL;
        uint32_t     i;
        for (i = 0U; i < pool->total_blocks; i++) {
            free_node_t *nd = (free_node_t *)(void *)cur;
            nd->next = prev;
            prev     = nd;
            cur     += pool->block_size;
        }
        pool->free_list = (void *)prev;
    }

#if MEMPOOL_ENABLE_STATS
    pool->stats.total_blocks  = pool->total_blocks;
    pool->stats.used_blocks   = 0U;
    pool->stats.free_blocks   = pool->total_blocks;
    pool->stats.peak_usage    = 0U;
    pool->stats.alloc_count   = 0U;
    pool->stats.free_count    = 0U;
    pool->stats.block_size    = pool->block_size;
#if MEMPOOL_ENABLE_GUARD
    pool->stats.guard_violations = 0U;
#endif
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
        case MEMPOOL_OK:                  return "Success";
        case MEMPOOL_ERR_NULL_PTR:        return "Null pointer";
        case MEMPOOL_ERR_INVALID_SIZE:    return "Invalid size";
        case MEMPOOL_ERR_OUT_OF_MEMORY:   return "Out of memory";
        case MEMPOOL_ERR_INVALID_BLOCK:   return "Invalid block";
        case MEMPOOL_ERR_ALIGNMENT:       return "Alignment error";
        case MEMPOOL_ERR_DOUBLE_FREE:     return "Double free detected";
        case MEMPOOL_ERR_NOT_INITIALIZED: return "Pool not initialized";
        case MEMPOOL_ERR_GUARD_CORRUPTED: return "Guard canary corrupted (buffer overrun)";
        case MEMPOOL_ERR_ISR_QUEUE_FULL:  return "ISR deferred-free queue full";
        default:                          return "Unknown error";
    }
}
#endif
