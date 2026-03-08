/**
 * @file mempool_internal.h
 * @brief Private types, struct definition, and shared helpers.
 *
 * This header is NOT part of the public API.  Never include it from user code.
 * It is included by mempool_core.c, mempool_diag.c, and mempool_isr.c.
 */

#ifndef MEMPOOL_INTERNAL_H
#define MEMPOOL_INTERNAL_H

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
    uint8_t   block_shift;   /* log2(block_size) when stride is a power of 2, else 0 */
    uint8_t   _pad0;

    void     *free_list;
    void     *blocks_start;
    void     *blocks_end;    /* one-past-end pointer of the block array */

    uint32_t  block_size;    /* per-block stride in bytes (includes guard canary when GUARD ON) */
    uint32_t  user_block_size; /* caller-visible bytes per block (= block_size when GUARD OFF;
                                  = block_size - 4 when GUARD ON; always set at init) */
    uint32_t  total_blocks;

#if MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
    uint8_t  *bitmap;        /* allocation bitmap; lives in pool_buffer */
    uint32_t  bitmap_bytes;  /* byte length of bitmap */
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
#if MEMPOOL_ENABLE_ISR_FREE
_Static_assert(MEMPOOL_ISR_QUEUE_CAPACITY <= 255U,
               "MEMPOOL_ISR_QUEUE_CAPACITY exceeds 255 — isr_head/isr_tail/isr_count "
               "are uint8_t and would silently overflow. Reduce the capacity or widen "
               "the index fields.");
#endif
#endif

/* ------------------------------------------------------------------ */
/* Math helpers                                                        */
/* ------------------------------------------------------------------ */

static inline int mp_is_pow2(size_t v)
{
    return (v > 0U) && ((v & (v - 1U)) == 0U);
}

static inline size_t mp_align_up(size_t v, size_t a)
{
    size_t m = a - 1U;
    return (v + m) & ~m;
}

static inline uint8_t mp_log2(uint32_t v)
{
#if defined(__GNUC__) || defined(__clang__)
    /* __builtin_ctz gives the number of trailing zero bits, which equals
     * log2 for power-of-two values (the only inputs this function receives).
     * This resolves to a single BSF/CTZ instruction on x86/ARM. */
    return (v == 0U) ? 0U : (uint8_t)__builtin_ctz(v);
#else
    uint8_t s = 0U;
    while (v > 1U) { v >>= 1U; s++; }
    return s;
#endif
}

/* ------------------------------------------------------------------ */
/* Block geometry helpers                                              */
/* ------------------------------------------------------------------ */

/**
 * Return the 0-based index of @p blk within the pool's block array.
 * The caller must have already validated that blk lies within the pool.
 */
static inline uint32_t mp_block_idx(const mempool_t *p, const void *blk)
{
    uintptr_t off = (uintptr_t)blk - (uintptr_t)p->blocks_start;
    if (p->block_shift != 0U) {
        return (uint32_t)(off >> p->block_shift);
    }
    return (uint32_t)(off / (uintptr_t)p->block_size);
}

/** Return the caller-visible bytes per block (user area, excluding canary). */
static inline size_t mp_user_size(const mempool_t *p)
{
#if MEMPOOL_ENABLE_GUARD
    return (size_t)p->user_block_size;
#else
    return (size_t)p->block_size;
#endif
}

/* ------------------------------------------------------------------ */
/* Block validation                                                    */
/* ------------------------------------------------------------------ */

/**
 * Verify that @p block lies inside the pool's block region and is aligned
 * to the per-block stride.
 *
 * Safe to call from ISR context because pool geometry is immutable after init.
 *
 * @return MEMPOOL_OK or MEMPOOL_ERR_INVALID_BLOCK.
 */
static inline mempool_error_t mp_validate_block(const mempool_t *pool,
                                         const void       *block)
{
    uintptr_t ba     = (uintptr_t)block;
    uintptr_t offset;

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
    return MEMPOOL_OK;
}

/* ------------------------------------------------------------------ */
/* Bitmap helpers (only present when DOUBLE_FREE_CHECK is ON)         */
/* ------------------------------------------------------------------ */

#if MEMPOOL_ENABLE_DOUBLE_FREE_CHECK

/** Mark block @p idx as allocated (set bit = 1). */
static inline void mp_bitmap_set(mempool_t *pool, uint32_t idx)
{
    uint32_t bi = idx >> 3U;
    uint8_t  m  = (uint8_t)(1U << (idx & 7U));
    if (bi < pool->bitmap_bytes) {
        pool->bitmap[bi] = (uint8_t)(pool->bitmap[bi] | m);
    }
}

/** Mark block @p idx as free (clear bit = 0). */
static inline void mp_bitmap_clear(mempool_t *pool, uint32_t idx)
{
    uint32_t bi = idx >> 3U;
    uint8_t  m  = (uint8_t)(1U << (idx & 7U));
    if (bi < pool->bitmap_bytes) {
        pool->bitmap[bi] = (uint8_t)(pool->bitmap[bi] & (uint8_t)(~m));
    }
}

/** Return non-zero if block @p idx is marked allocated. */
static inline int mp_bitmap_is_set(const mempool_t *pool, uint32_t idx)
{
    uint32_t bi = idx >> 3U;
    uint8_t  m  = (uint8_t)(1U << (idx & 7U));
    if (bi >= pool->bitmap_bytes) { return 0; }
    return (pool->bitmap[bi] & m) != 0;
}

#endif /* MEMPOOL_ENABLE_DOUBLE_FREE_CHECK */

/* ------------------------------------------------------------------ */
/* Cross-unit function declarations                                    */
/* ------------------------------------------------------------------ */

/* Defined in mempool_core.c — used by mempool_init and mempool_reset */
void mp_build_free_list(mempool_t *pool);

/* Defined in mempool_core.c — used by mempool_init, mempool_diag.c, mempool_isr.c */
int mp_layout(size_t pool_buffer_size, size_t stride, size_t alignment,
              uint32_t *n_out, size_t *blk_off_out);

#if MEMPOOL_ENABLE_ISR_FREE
/* Defined in mempool_isr.c — called from mempool_alloc (mempool_core.c)
 * with the task-level lock already held. */
void mp_flush_isr_queue(mempool_t *pool);
#endif

#endif /* MEMPOOL_INTERNAL_H */
