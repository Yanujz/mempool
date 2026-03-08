/**
 * @file mempool_diag.c
 * @brief Diagnostic and optional-feature APIs.
 *
 * Contains: alloc_zero, OOM hook, per-block tags, statistics, strerror,
 * block iteration (walk), and allocated-block query.
 */

#include "mempool_internal.h"

/* ------------------------------------------------------------------ */
/* mempool_alloc_zero                                                  */
/* ------------------------------------------------------------------ */

mempool_error_t mempool_alloc_zero(mempool_t *pool, void **block)
{
    mempool_error_t err;

    if (block == NULL) {
        return MEMPOOL_ERR_NULL_PTR;
    }
    *block = NULL; /* ensure caller never sees a stale pointer on error */

    if (pool == NULL) {
        return MEMPOOL_ERR_NULL_PTR;
    }

    err = mempool_alloc(pool, block);
    if (err == MEMPOOL_OK) {
        /* Zero only the user-visible area (mp_user_size excludes the canary) */
        memset(*block, 0, mp_user_size(pool));
    }
    return err;
}

/* ------------------------------------------------------------------ */
/* OOM hook                                                            */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* Per-block tags                                                      */
/* ------------------------------------------------------------------ */

#if MEMPOOL_ENABLE_TAGS
mempool_error_t mempool_set_block_tag(mempool_t *pool, void *block,
                                      uint32_t tag)
{
    uint32_t idx;

    if ((pool == NULL) || (block == NULL)) { return MEMPOOL_ERR_NULL_PTR; }
    if (pool->magic != MEMPOOL_MAGIC) { return MEMPOOL_ERR_NOT_INITIALIZED; }
    if (mp_validate_block(pool, block) != MEMPOOL_OK) {
        return MEMPOOL_ERR_INVALID_BLOCK;
    }

    idx = mp_block_idx(pool, block);

    MEMPOOL_LOCK();
#if MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
    /* Reject tag writes to freed blocks.  A freed block's bitmap bit is 0;
     * writing a tag to it would make the tag visible to the next allocator. */
    if (!mp_bitmap_is_set(pool, idx)) {
        MEMPOOL_UNLOCK();
        return MEMPOOL_ERR_INVALID_BLOCK;
    }
#endif
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
    if (mp_validate_block(pool, block) != MEMPOOL_OK) {
        return MEMPOOL_ERR_INVALID_BLOCK;
    }

    idx = mp_block_idx(pool, block);

    MEMPOOL_LOCK();
#if MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
    /* Reject tag reads from freed blocks to prevent returning stale data. */
    if (!mp_bitmap_is_set(pool, idx)) {
        MEMPOOL_UNLOCK();
        return MEMPOOL_ERR_INVALID_BLOCK;
    }
#endif
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
        /* Lock to prevent a concurrent mempool_get_block_tag() from reading
         * a partially-written or stale tag value for this block. */
        MEMPOOL_LOCK();
        pool->tags[idx] = tag;
        MEMPOOL_UNLOCK();
    }
    return err;
}
#endif /* MEMPOOL_ENABLE_TAGS */

/* ------------------------------------------------------------------ */
/* Statistics                                                          */
/* ------------------------------------------------------------------ */

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
    pool->stats.alloc_count = 0U;
    pool->stats.free_count  = 0U;
    pool->stats.peak_usage  = pool->stats.used_blocks; /* sticky reset to current */
#if MEMPOOL_ENABLE_GUARD
    pool->stats.guard_violations = 0U;
#endif
    MEMPOOL_UNLOCK();
    return MEMPOOL_OK;
}
#endif /* MEMPOOL_ENABLE_STATS */

/* ------------------------------------------------------------------ */
/* Block iteration (requires allocation bitmap)                       */
/* ------------------------------------------------------------------ */

#if MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
mempool_error_t mempool_walk(const mempool_t   *pool,
                              mempool_walk_fn_t  fn,
                              void              *ctx)
{
    uint32_t i;

    if ((pool == NULL) || (fn == NULL)) {
        return MEMPOOL_ERR_NULL_PTR;
    }
    if (pool->magic != MEMPOOL_MAGIC) {
        return MEMPOOL_ERR_NOT_INITIALIZED;
    }

    /* Hold the lock for the full walk so callers see a consistent snapshot.
     * The callback must NOT call back into the same pool. */
    MEMPOOL_LOCK();
    for (i = 0U; i < pool->total_blocks; i++) {
        if (mp_bitmap_is_set(pool, i)) {
            const void *blk = (const void *)
                ((const uint8_t *)pool->blocks_start +
                 (size_t)i * pool->block_size);
            fn(pool, blk, i, ctx);
        }
    }
    MEMPOOL_UNLOCK();
    return MEMPOOL_OK;
}

int mempool_is_block_allocated(const mempool_t *pool, const void *block)
{
    uint32_t idx;

    if ((pool == NULL) || (block == NULL) || (pool->magic != MEMPOOL_MAGIC)) {
        return 0;
    }
    if (mp_validate_block(pool, block) != MEMPOOL_OK) {
        return 0;
    }

    idx = mp_block_idx(pool, block);
    return mp_bitmap_is_set(pool, idx);
}
#endif /* MEMPOOL_ENABLE_DOUBLE_FREE_CHECK */

int mempool_has_free_block(const mempool_t *pool)
{
    if ((pool == NULL) || (pool->magic != MEMPOOL_MAGIC)) { return 0; }
    return (pool->free_list != NULL) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Error strings                                                       */
/* ------------------------------------------------------------------ */

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
#endif /* MEMPOOL_ENABLE_STRERROR */

/* Suppress empty-TU warning when all optional features are disabled. */
typedef int mempool_diag_placeholder_t;
