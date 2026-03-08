/**
 * @file mempool_core.c
 * @brief Core pool operations: init, alloc, free, reset, and query functions.
 *
 * Contains the pool buffer layout algorithm, free-list management, and the
 * primary alloc/free/reset paths.  Optional-feature code is all in separate
 * translation units (mempool_diag.c, mempool_isr.c).
 */

#include "mempool_internal.h"

/* ------------------------------------------------------------------ */
/* Pool buffer layout                                                  */
/* ------------------------------------------------------------------ */

/*
 * Compute the offset of the block array within pool_buffer and the maximum
 * number of blocks that fit, given a per-block stride.
 *
 * Buffer layout (each section present only when the feature is enabled):
 *
 *   [bitmap: ceil(n/8) bytes]  [alignment padding]
 *   [tags:   n × 4 bytes    ]  [alignment padding]
 *   [blocks: n × stride     ]
 *
 * Both bitmap and tags overhead depend on n, so the loop iterates downward
 * from max_n.  In practice it terminates in 0–1 iterations.
 */
int mp_layout(size_t pool_buffer_size, size_t stride, size_t alignment,
              uint32_t *n_out, size_t *blk_off_out)
{
    size_t max_n;
    size_t n;

    if (stride == 0U) { return 0; }
    max_n = pool_buffer_size / stride;
    if ((max_n == 0U) || (max_n > (size_t)UINT32_MAX)) { return 0; }

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
/* Free-list construction (shared by init and reset)                  */
/* ------------------------------------------------------------------ */

void mp_build_free_list(mempool_t *pool)
{
    uint8_t     *cur  = (uint8_t *)pool->blocks_start;
    free_node_t *prev = NULL;
    uint32_t     i;

    for (i = 0U; i < pool->total_blocks; i++) {
        free_node_t *nd = (free_node_t *)(void *)cur;
        nd->next        = prev;
        prev            = nd;
        cur            += pool->block_size;
    }
    pool->free_list = (void *)prev;
}

/* ------------------------------------------------------------------ */
/* Stats reset helper (shared by init and reset)                      */
/* ------------------------------------------------------------------ */

#if MEMPOOL_ENABLE_STATS
static void mp_stats_init(mempool_t *p)
{
    p->stats.total_blocks     = p->total_blocks;
    p->stats.used_blocks      = 0U;
    p->stats.free_blocks      = p->total_blocks;
    p->stats.peak_usage       = 0U;
    p->stats.alloc_count      = 0U;
    p->stats.free_count       = 0U;
    p->stats.block_size       = p->block_size;
#if MEMPOOL_ENABLE_GUARD
    p->stats.guard_violations = 0U;
#endif
}
#endif /* MEMPOOL_ENABLE_STATS */

/* ------------------------------------------------------------------ */
/* mempool_init                                                        */
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

    /* Per-block stride: user area + optional post-canary, rounded up */
#if MEMPOOL_ENABLE_GUARD
    stride = mp_align_up(block_size + sizeof(uint32_t), alignment);
#else
    stride = mp_align_up(block_size, alignment);
#endif

    if (!mp_layout(pool_buffer_size, stride, alignment, &n, &blk_off)) {
        return MEMPOOL_ERR_INVALID_SIZE;
    }

    p = (mempool_t *)state_buffer;

    /* Set up overhead areas inside pool_buffer */
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
        (void)cur_off;
    }

    /* Fill in pool state */
    p->magic           = MEMPOOL_MAGIC;
    p->block_size      = (uint32_t)stride;
    p->user_block_size = (uint32_t)block_size; /* usable bytes: stride - 4 when GUARD ON */
    p->total_blocks    = n;
    p->blocks_start    = (void *)((uint8_t *)pool_buffer + blk_off);
    p->blocks_end      = (void *)((uint8_t *)p->blocks_start + (size_t)n * stride);
    p->block_shift     = mp_is_pow2(stride) ? mp_log2((uint32_t)stride) : 0U;

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
    mp_stats_init(p);
#endif

    mp_build_free_list(p);

    *pool_out = p;
    return MEMPOOL_OK;
}

/* ------------------------------------------------------------------ */
/* mempool_alloc                                                       */
/* ------------------------------------------------------------------ */

mempool_error_t mempool_alloc(mempool_t *pool, void **block)
{
    free_node_t *nd;

    if (block == NULL) {
        return MEMPOOL_ERR_NULL_PTR;
    }
    *block = NULL; /* always clear; caller must never see a stale pointer */

    if (pool == NULL) {
        return MEMPOOL_ERR_NULL_PTR;
    }
    if (pool->magic != MEMPOOL_MAGIC) {
        return MEMPOOL_ERR_NOT_INITIALIZED;
    }

    MEMPOOL_LOCK();

#if MEMPOOL_ENABLE_ISR_FREE
    /* Drain any ISR-queued frees before consulting the free list. */
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
    mp_bitmap_set(pool, mp_block_idx(pool, (const void *)nd));
#endif

#if MEMPOOL_ENABLE_GUARD
    {
        /* Use memcpy for the 4-byte canary write to avoid a potential
         * misaligned uint32_t store when user_block_size % 4 != 0.
         * memcpy is well-defined for any alignment and compiles to a
         * single store instruction on any reasonable target. */
        const uint32_t cv = (uint32_t)MEMPOOL_CANARY_VALUE;
        memcpy((uint8_t *)nd + pool->user_block_size, &cv, sizeof(cv));
    }
#endif

#if MEMPOOL_ENABLE_POISON
    /* Fill user area with alloc-poison so uninitialised reads stand out.
     * Written after the canary to avoid clobbering it. */
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

/* ------------------------------------------------------------------ */
/* mempool_free                                                        */
/* ------------------------------------------------------------------ */

mempool_error_t mempool_free(mempool_t *pool, void *block)
{
    if ((pool == NULL) || (block == NULL)) {
        return MEMPOOL_ERR_NULL_PTR;
    }
    if (pool->magic != MEMPOOL_MAGIC) {
        return MEMPOOL_ERR_NOT_INITIALIZED;
    }

    {
        mempool_error_t verr = mp_validate_block(pool, block);
        if (verr != MEMPOOL_OK) { return verr; }
    }

    MEMPOOL_LOCK();

    /*
     * Invariant: GUARD must be checked BEFORE the double-free bitmap is
     * touched.  If the canary is bad we quarantine the block: the bitmap bit
     * stays SET so the next free() on the same block returns DOUBLE_FREE
     * rather than silently re-adding it to the free list.
     */
#if MEMPOOL_ENABLE_GUARD
    {
        /* Use memcpy for canary read to avoid misaligned uint32_t load
         * when user_block_size % 4 != 0.  Safe on any alignment. */
        uint32_t cv = 0U;
        memcpy(&cv, (const uint8_t *)block + pool->user_block_size, sizeof(cv));
        if (cv != (uint32_t)MEMPOOL_CANARY_VALUE) {
#if MEMPOOL_ENABLE_STATS
            pool->stats.guard_violations++;
            if (pool->stats.used_blocks > 0U) { pool->stats.used_blocks--; }
#endif
            MEMPOOL_UNLOCK();
            return MEMPOOL_ERR_GUARD_CORRUPTED;
        }
    }
#endif

#if MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
    {
        uint32_t idx = mp_block_idx(pool, block);
        uint32_t bi  = idx >> 3U;

        if (bi >= pool->bitmap_bytes) {
            MEMPOOL_UNLOCK();
            return MEMPOOL_ERR_INVALID_BLOCK;
        }
        if (!mp_bitmap_is_set(pool, idx)) {
            MEMPOOL_UNLOCK();
            return MEMPOOL_ERR_DOUBLE_FREE;
        }
        mp_bitmap_clear(pool, idx);
    }
#endif

#if MEMPOOL_ENABLE_POISON
    /* Overwrite user area with free-poison to surface use-after-free.
     * The free-list next pointer written immediately below overwrites the
     * first sizeof(void*) bytes — this is expected and documented. */
    memset(block, (int)(uint8_t)MEMPOOL_FREE_POISON_BYTE, mp_user_size(pool));
#endif

#if MEMPOOL_ENABLE_TAGS
    /* Clear the tag so realloc callers do not see a stale annotation from
     * the previous owner.  Must be inside the lock because get_block_tag()
     * also acquires it. */
    pool->tags[mp_block_idx(pool, block)] = 0U;
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

/* ------------------------------------------------------------------ */
/* mempool_reset                                                       */
/* ------------------------------------------------------------------ */

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
    /* Clear the ISR queue under both locks to prevent a concurrent ISR from
     * observing a half-reset queue (e.g. isr_count=0 while isr_tail≠0). */
    MEMPOOL_ISR_LOCK();
    pool->isr_head  = 0U;
    pool->isr_tail  = 0U;
    pool->isr_count = 0U;
    MEMPOOL_ISR_UNLOCK();
#endif

    mp_build_free_list(pool);

#if MEMPOOL_ENABLE_STATS
    mp_stats_init(pool);
#endif

    MEMPOOL_UNLOCK();
    return MEMPOOL_OK;
}

/* ------------------------------------------------------------------ */
/* Query functions                                                     */
/* ------------------------------------------------------------------ */

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

int mempool_is_initialized(const mempool_t *pool)
{
    return (pool != NULL) && (pool->magic == MEMPOOL_MAGIC);
}

uint32_t mempool_block_size(const mempool_t *pool)
{
    if ((pool == NULL) || (pool->magic != MEMPOOL_MAGIC)) { return 0U; }
    return pool->block_size;
}

uint32_t mempool_user_block_size(const mempool_t *pool)
{
    if ((pool == NULL) || (pool->magic != MEMPOOL_MAGIC)) { return 0U; }
    return pool->user_block_size;
}

uint32_t mempool_capacity(const mempool_t *pool)
{
    if ((pool == NULL) || (pool->magic != MEMPOOL_MAGIC)) { return 0U; }
    return pool->total_blocks;
}

size_t mempool_pool_buffer_size(size_t block_size, uint32_t n, size_t alignment)
{
    size_t stride;
    size_t off = 0U;

    if ((block_size == 0U) || (n == 0U) || !mp_is_pow2(alignment)) {
        return 0U;
    }

#if MEMPOOL_ENABLE_GUARD
    stride = mp_align_up(block_size + sizeof(uint32_t), alignment);
#else
    stride = mp_align_up(block_size, alignment);
#endif

#if MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
    off = mp_align_up(off + ((size_t)n + 7U) / 8U, alignment);
#endif
#if MEMPOOL_ENABLE_TAGS
    off = mp_align_up(off + (size_t)n * sizeof(uint32_t), alignment);
#endif

    return off + (size_t)n * stride;
}
