/**
 * @file mempool_isr.c
 * @brief ISR-safe deferred-free queue.
 *
 * mempool_free_from_isr() enqueues a block into a fixed-capacity ring buffer
 * using only the lightweight ISR lock.  mempool_drain_isr_queue() (or the
 * lazy drain inside mempool_alloc()) processes the queue under the task lock.
 */

#include "mempool_internal.h"

#if MEMPOOL_ENABLE_ISR_FREE

/* ------------------------------------------------------------------ */
/* mp_flush_isr_queue — drain helper (called with task lock held)     */
/* ------------------------------------------------------------------ */

/*
 * Processing order per block (matches the invariant in mempool_free):
 *
 *   1. GUARD check   — if canary bad: quarantine (bitmap bit stays SET),
 *                      decrement used_blocks, skip to next entry.
 *   2. DOUBLE_FREE   — if bitmap bit is already 0, the block was queued
 *                      twice (ISR double-free); discard silently.
 *                      Otherwise clear the bit.
 *   3. POISON fill   — overwrite user area with free-pattern.
 *   4. Free list     — prepend block to free list (overwrites first
 *                      sizeof(void*) bytes of the poison pattern — expected).
 *   5. Stats update  — decrement used_blocks, increment free_blocks.
 */
void mp_flush_isr_queue(mempool_t *pool)
{
    while (pool->isr_count > 0U) {
        free_node_t *nd;
        void        *blk;

        MEMPOOL_ISR_LOCK();
        blk            = pool->isr_queue[pool->isr_head];
        pool->isr_head = (uint8_t)((pool->isr_head + 1U) %
                                   MEMPOOL_ISR_QUEUE_CAPACITY);
        pool->isr_count--;
        MEMPOOL_ISR_UNLOCK();

#if MEMPOOL_ENABLE_GUARD
        /* Validate the post-canary before any state is modified.
         * A corrupted canary means a buffer overrun reached the ISR-free path.
         * Quarantine the block: leave bitmap bit SET (normal free() will then
         * return DOUBLE_FREE instead of silently corrupting the free list).
         * Decrement used_blocks so stats remain accurate. */
        {
            const uint32_t *canary = (const uint32_t *)(const void *)
                                     ((const uint8_t *)blk + pool->user_block_size);
            if (*canary != (uint32_t)MEMPOOL_CANARY_VALUE) {
#if MEMPOOL_ENABLE_STATS
                pool->stats.guard_violations++;
                if (pool->stats.used_blocks > 0U) { pool->stats.used_blocks--; }
#endif
                continue; /* quarantined — do not add to free list */
            }
        }
#endif /* MEMPOOL_ENABLE_GUARD */

#if MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
        /* Check the bitmap BEFORE clearing the bit.  If the bit is already 0
         * the same block was queued twice via mempool_free_from_isr() (ISR
         * double-free).  Discard the duplicate silently. */
        {
            uint32_t idx = mp_block_idx(pool, blk);
            uint32_t bi  = idx >> 3U;
            uint8_t  m   = (uint8_t)(1U << (idx & 7U));

            if (bi < pool->bitmap_bytes) {
                if ((pool->bitmap[bi] & m) == 0U) {
                    continue; /* duplicate ISR free — discard */
                }
                pool->bitmap[bi] = (uint8_t)(pool->bitmap[bi] & (uint8_t)(~m));
            }
        }
#endif

#if MEMPOOL_ENABLE_POISON
        /* Overwrite user area with free-poison; free-list pointer written
         * below will overwrite the first sizeof(void*) bytes. */
        memset(blk, (int)(uint8_t)MEMPOOL_FREE_POISON_BYTE, mp_user_size(pool));
#endif

        nd              = (free_node_t *)blk;
        nd->next        = (free_node_t *)pool->free_list;
        pool->free_list = (void *)nd;

#if MEMPOOL_ENABLE_STATS
        pool->stats.free_count++;
        if (pool->stats.used_blocks > 0U) { pool->stats.used_blocks--; }
        pool->stats.free_blocks++;
#endif
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

mempool_error_t mempool_free_from_isr(mempool_t *pool, void *block)
{
    uintptr_t ba, offset;

    if ((pool == NULL) || (block == NULL)) { return MEMPOOL_ERR_NULL_PTR; }
    if (pool->magic != MEMPOOL_MAGIC)      { return MEMPOOL_ERR_NOT_INITIALIZED; }

    /* Validate block before acquiring the ISR lock.  Pool geometry is
     * immutable after init so these reads are safe in ISR context. */
    ba = (uintptr_t)block;
    if ((ba < (uintptr_t)pool->blocks_start) ||
        (ba >= (uintptr_t)pool->blocks_end)) {
        return MEMPOOL_ERR_INVALID_BLOCK;
    }
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

#else
/* Suppress empty-TU warning when ISR_FREE is disabled. */
typedef int mempool_isr_disabled_t;
#endif /* MEMPOOL_ENABLE_ISR_FREE */
