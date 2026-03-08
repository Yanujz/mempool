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
#define MEMPOOL_VERSION_MINOR 5
#define MEMPOOL_VERSION_PATCH 0
#define MEMPOOL_VERSION_STRING "0.5.0"

/* -------------------------------------------------------------------------
 * Opaque pool handle (declared early so it can be used in callback types)
 * ---------------------------------------------------------------------- */
typedef struct mempool mempool_t;

/* -------------------------------------------------------------------------
 * Error codes
 * ---------------------------------------------------------------------- */
typedef enum {
    MEMPOOL_OK = 0,
    MEMPOOL_ERR_NULL_PTR        = 1,  /**< Null pointer argument                  */
    MEMPOOL_ERR_INVALID_SIZE    = 2,  /**< Size is 0, too small, or would overflow */
    MEMPOOL_ERR_OUT_OF_MEMORY   = 3,  /**< Pool exhausted                          */
    MEMPOOL_ERR_INVALID_BLOCK   = 4,  /**< Block not in pool or misaligned         */
    MEMPOOL_ERR_ALIGNMENT       = 5,  /**< Alignment requirement violated          */
    MEMPOOL_ERR_DOUBLE_FREE     = 6,  /**< Block freed while already free          */
    MEMPOOL_ERR_NOT_INITIALIZED = 7,  /**< Magic sentinel check failed             */
    MEMPOOL_ERR_GUARD_CORRUPTED = 8,  /**< Post-canary overwritten (buffer overrun)*/
    MEMPOOL_ERR_ISR_QUEUE_FULL  = 9   /**< ISR deferred-free ring buffer full      */
} mempool_error_t;

/* -------------------------------------------------------------------------
 * Statistics
 * ---------------------------------------------------------------------- */
#if MEMPOOL_ENABLE_STATS
typedef struct {
    uint32_t total_blocks;      /**< Total blocks in the pool                    */
    uint32_t used_blocks;       /**< Currently allocated blocks                  */
    uint32_t free_blocks;       /**< Currently free blocks                       */
    uint32_t peak_usage;        /**< High-water mark of used_blocks (sticky)     */
    uint32_t alloc_count;       /**< Lifetime successful allocations             */
    uint32_t free_count;        /**< Lifetime successful frees                   */
    uint32_t block_size;        /**< Per-block stride in bytes (includes 4-byte guard canary when MEMPOOL_ENABLE_GUARD is ON) */
#if MEMPOOL_ENABLE_GUARD
    uint32_t guard_violations;  /**< Canary corruptions detected at free time    */
#endif
} mempool_stats_t;
#endif /* MEMPOOL_ENABLE_STATS */

/* -------------------------------------------------------------------------
 * OOM hook type
 * ---------------------------------------------------------------------- */
#if MEMPOOL_ENABLE_OOM_HOOK
/**
 * Callback invoked by mempool_alloc() when the pool is exhausted.
 * Called with the pool lock held; do not call back into the pool.
 * @param pool      The pool that ran out of memory.
 * @param user_data The opaque pointer supplied to mempool_set_oom_hook().
 */
typedef void (*mempool_oom_hook_t)(mempool_t *pool, void *user_data);
#endif


/**
 * Return the number of bytes a pool buffer must be to hold @p n blocks of
 * @p block_size bytes aligned to @p alignment, accounting for all currently
 * active feature overheads (guard canary, bitmap, tags).
 *
 * This is a runtime companion to MEMPOOL_POOL_BUFFER_SIZE().  It respects
 * the compile-time feature flags and returns the exact minimum size.
 * Returns 0 on invalid arguments (block_size==0, n==0, alignment not a
 * power of two).
 *
 * Complexity: **O(1)** — pure arithmetic with no loops or data-structure
 * access; evaluates in a fixed number of machine instructions regardless
 * of @p n.
 */
size_t mempool_pool_buffer_size(size_t block_size, uint32_t n, size_t alignment);

/* -------------------------------------------------------------------------
 * Compile-time pool buffer sizing (conservative upper bound)
 *
 * MEMPOOL_POOL_BUFFER_SIZE(block_size, n, alignment)
 *   Expands to a constant expression for the maximum pool buffer size
 *   assuming ALL optional features (guard, bitmap, tags) are active.
 *   Safe to use as a static array size.  alignment must be a power of two.
 *   Example:
 *     static uint8_t pool_buf[MEMPOOL_POOL_BUFFER_SIZE(64, 32, 8)];
 * ---------------------------------------------------------------------- */
#define MEMPOOL_POOL_BUFFER_SIZE(block_size_, n_, alignment_)  \
    (  /* bitmap: ceil(n/8) rounded up to alignment */          \
       ((((size_t)(n_) + 7U) / 8U + (size_t)(alignment_) - 1U) \
            & ~((size_t)(alignment_) - 1U))                     \
       /* tags: n * 4 bytes rounded up */                       \
     + ((((size_t)(n_) * 4U) + (size_t)(alignment_) - 1U)      \
            & ~((size_t)(alignment_) - 1U))                     \
       /* blocks: n * stride where stride includes 4-byte guard */ \
     + (size_t)(n_) * (((size_t)(block_size_) + 4U               \
            + (size_t)(alignment_) - 1U) & ~((size_t)(alignment_) - 1U)) \
    )


size_t mempool_state_size(void);

/**
 * Initialise a memory pool.
 *
 * Complexity: **O(N)** where N = number of blocks that fit in
 * @p pool_buffer_size.  Dominated by bitmap/tag `memset` and free-list
 * construction.  Call once at startup; not suitable for hot paths.
 *
 * @param state_buffer      Caller-provided storage for pool state; must be at
 *                          least mempool_state_size() bytes.
 * @param state_buffer_size Byte size of state_buffer.
 * @param pool_buffer       Memory region that backs the blocks.  Must already
 *                          be aligned to @p alignment.
 * @param pool_buffer_size  Byte size of pool_buffer.
 * @param block_size        Usable bytes per block (>= sizeof(void *)).
 * @param alignment         Block alignment; must be a power of two.
 * @param pool_out          Receives the initialised pool handle.
 * @return MEMPOOL_OK on success, or an error code.
 */
mempool_error_t mempool_init(
    void        *state_buffer,
    size_t       state_buffer_size,
    void        *pool_buffer,
    size_t       pool_buffer_size,
    size_t       block_size,
    size_t       alignment,
    mempool_t  **pool_out
);

/**
 * Allocate one block.
 *
 * Complexity: **O(1)** — pops the head of the free-list (one pointer read and
 * one pointer write).  No search, no traversal.  When
 * `MEMPOOL_ENABLE_ISR_FREE` is set, pending deferred frees are drained first
 * (at most `MEMPOOL_ISR_QUEUE_CAPACITY` entries, a compile-time constant;
 * still O(1) with that constant as the WCET multiplier).
 *
 * @return MEMPOOL_OK on success, MEMPOOL_ERR_OUT_OF_MEMORY if the pool is
 *         exhausted, MEMPOOL_ERR_NULL_PTR if either argument is NULL,
 *         MEMPOOL_ERR_NOT_INITIALIZED if the pool handle is invalid.
 */
mempool_error_t mempool_alloc(mempool_t *pool, void **block);

/**
 * Allocate one block and zero-fill the user area.
 *
 * Complexity: **O(block_size)** — O(1) allocation followed by a
 * `memset(0, block_size)` over the user area.
 */
mempool_error_t mempool_alloc_zero(mempool_t *pool, void **block);

/**
 * Return a block to the pool.
 *
 * Complexity: **O(1)** — pointer range check, optional bitmap bit read/clear
 * (array subscript + bitmask; no loop), optional canary read (one load),
 * optional `memset` (O(block_size)), and free-list prepend (two pointer
 * writes).  Every individual operation is bounded by a compile-time constant
 * unrelated to the number of blocks in the pool.
 *
 * @return MEMPOOL_OK on success.  MEMPOOL_ERR_GUARD_CORRUPTED if the
 *         post-canary was overwritten (block is quarantined, not returned
 *         to the free list).  MEMPOOL_ERR_DOUBLE_FREE if the block was
 *         already free.  MEMPOOL_ERR_INVALID_BLOCK if the pointer is outside
 *         the pool or misaligned.
 */
mempool_error_t mempool_free(mempool_t *pool, void *block);

/**
 * Reset pool to initial state (all blocks free, stats zeroed).
 *
 * Complexity: **O(N)** where N = `total_blocks`, dominated by `memset` over
 * the bitmap and/or tag array followed by O(N) free-list construction.
 * Not suitable for use in a real-time interrupt handler.
 */
mempool_error_t mempool_reset(mempool_t *pool);

/**
 * Test whether @p ptr lies within the pool's block region.
 * @return 1 if yes, 0 if no (or on invalid arguments).
 */
int mempool_contains(const mempool_t *pool, const void *ptr);

/**
 * Return 1 if @p pool points to a successfully initialised pool, 0 otherwise.
 *
 * This is a lightweight magic-sentinel check.  Use it to validate pool handles
 * before passing them to other APIs (e.g. mempool_mgr_init()).
 *
 * @return 1 if initialised, 0 if NULL / uninitialised / corrupted.
 */
int mempool_is_initialized(const mempool_t *pool);

/**
 * Return the per-block stride in bytes.
 *
 * When MEMPOOL_ENABLE_GUARD is ON the stride includes the 4-byte post-canary
 * padding; the caller-visible area is correspondingly smaller.
 * This function is always available regardless of MEMPOOL_ENABLE_STATS.
 *
 * @return Block stride, or 0 on invalid arguments.
 */
uint32_t mempool_block_size(const mempool_t *pool);

/**
 * Return the total number of blocks (pool capacity) established at init time.
 *
 * This function is always available regardless of MEMPOOL_ENABLE_STATS.
 *
 * @return Block count, or 0 on invalid arguments.
 */
uint32_t mempool_capacity(const mempool_t *pool);

/* -------------------------------------------------------------------------
 * Diagnostic block iteration
 * ---------------------------------------------------------------------- */
#if MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
/**
 * Callback type for mempool_walk().
 * @param pool   The pool being walked.
 * @param block  Pointer to the currently-allocated block.
 * @param idx    0-based block index within the pool.
 * @param ctx    Opaque pointer passed through from mempool_walk().
 */
typedef void (*mempool_walk_fn_t)(const mempool_t *pool,
                                  const void       *block,
                                  uint32_t          idx,
                                  void             *ctx);

/**
 * Iterate over every currently-allocated block and invoke @p fn for each.
 *
 * Complexity: **O(N)** where N = `total_blocks` (all bitmap bytes are scanned
 * regardless of how many blocks are allocated).  The task-level lock is held
 * for the entire walk; @p fn must not call back into this pool, and long
 * callbacks will block all other pool users for O(N) time.
 *
 * Requires MEMPOOL_ENABLE_DOUBLE_FREE_CHECK=1 (the bitmap identifies which
 * blocks are allocated).
 *
 * @param pool  Pool to walk.
 * @param fn    Callback invoked for each allocated block.
 * @param ctx   Opaque context pointer passed to @p fn unchanged.
 * @return MEMPOOL_OK, MEMPOOL_ERR_NULL_PTR, or MEMPOOL_ERR_NOT_INITIALIZED.
 */
mempool_error_t mempool_walk(const mempool_t   *pool,
                              mempool_walk_fn_t  fn,
                              void              *ctx);

/**
 * Return 1 if @p block is currently allocated, 0 if it is free or if
 * @p block does not address a valid block in @p pool.
 * Requires MEMPOOL_ENABLE_DOUBLE_FREE_CHECK=1.
 *
 * Complexity: **O(1)** — three sequential constant-time steps:
 *   1. Range check: two pointer comparisons.
 *   2. Index computation: one pointer subtraction followed by a single
 *      right-shift (power-of-two stride) or integer divide (non-power-of-two
 *      stride).  Either way: no loop, no data traversal.
 *   3. Bitmap bit read: one array subscript (index >> 3) plus a bitmask
 *      (1 << (index & 7)) and a single AND.  One byte of memory touched.
 * Total: a fixed number of machine instructions, independent of pool size.
 */
int mempool_is_block_allocated(const mempool_t *pool, const void *block);
#endif /* MEMPOOL_ENABLE_DOUBLE_FREE_CHECK */

/* -------------------------------------------------------------------------
 * Statistics
 * ---------------------------------------------------------------------- */
#if MEMPOOL_ENABLE_STATS
/** Snapshot the pool's statistics into @p stats (thread-safe). */
mempool_error_t mempool_get_stats(const mempool_t *pool,
                                  mempool_stats_t *stats);

/**
 * Reset only the statistical counters (alloc_count, free_count, peak_usage)
 * without disturbing the pool's block state.  Use this to implement a
 * "sticky high-watermark" pattern: call mempool_reset_stats() at the start
 * of a measurement window without discarding live allocations.
 */
mempool_error_t mempool_reset_stats(mempool_t *pool);
#endif

/* -------------------------------------------------------------------------
 * OOM hook
 * ---------------------------------------------------------------------- */
#if MEMPOOL_ENABLE_OOM_HOOK
/**
 * Register a callback that is invoked whenever mempool_alloc() finds the pool
 * exhausted.  Pass @p hook = NULL to clear a previously installed hook.
 */
mempool_error_t mempool_set_oom_hook(mempool_t          *pool,
                                     mempool_oom_hook_t  hook,
                                     void               *user_data);
#endif

/* -------------------------------------------------------------------------
 * Block tags (per-block 32-bit caller annotation)
 * ---------------------------------------------------------------------- */
#if MEMPOOL_ENABLE_TAGS
/**
 * Attach a 32-bit tag to an allocated block (e.g. task ID, source-line hash).
 * The tag is stored in the pool buffer and is readable until the block is freed.
 */
mempool_error_t mempool_set_block_tag(mempool_t *pool, void *block,
                                      uint32_t tag);

/**
 * Read the tag previously attached to @p block.
 * @param tag_out  Receives the tag; set to 0 if the block was never tagged.
 */
mempool_error_t mempool_get_block_tag(const mempool_t *pool, const void *block,
                                      uint32_t *tag_out);

/**
 * Allocate a block and immediately set its tag in a single call.
 *
 * Equivalent to mempool_alloc() followed by mempool_set_block_tag() but
 * avoids a second index computation.  The allocation and the tag write use
 * two separate critical sections (not a single atomic operation); the tag is
 * set before this function returns, so callers that respect ownership (i.e.
 * do not share the fresh block pointer before this call returns) observe a
 * consistent tag.
 */
mempool_error_t mempool_alloc_tagged(mempool_t *pool, void **block,
                                     uint32_t tag);

/** Convenience macro that captures __FILE__ hash + __LINE__ as the tag. */
#define MEMPOOL_ALLOC_TAGGED(pool, block_ptr, tag) \
    mempool_alloc_tagged((pool), (block_ptr), (tag))
#endif /* MEMPOOL_ENABLE_TAGS */

/* -------------------------------------------------------------------------
 * ISR-safe deferred free
 * ---------------------------------------------------------------------- */
#if MEMPOOL_ENABLE_ISR_FREE
/**
 * Queue a block for deferred release from interrupt context.
 * This function does NOT take the task-level lock; it uses only
 * MEMPOOL_ISR_LOCK / MEMPOOL_ISR_UNLOCK.  Returns MEMPOOL_ERR_ISR_QUEUE_FULL
 * if MEMPOOL_ISR_QUEUE_CAPACITY is exceeded.
 *
 * The queued block is returned to the pool when either:
 *   - mempool_drain_isr_queue() is called from task context, or
 *   - the next mempool_alloc() runs on the same pool.
 */
mempool_error_t mempool_free_from_isr(mempool_t *pool, void *block);

/**
 * Drain all blocks queued via mempool_free_from_isr() back into the pool.
 * Must be called from task context (acquires the task-level lock).
 */
mempool_error_t mempool_drain_isr_queue(mempool_t *pool);
#endif /* MEMPOOL_ENABLE_ISR_FREE */

/* -------------------------------------------------------------------------
 * Error strings
 * ---------------------------------------------------------------------- */
#if MEMPOOL_ENABLE_STRERROR
/** Translate an error code to a human-readable C string. */
const char *mempool_strerror(mempool_error_t error);
#endif

#ifdef __cplusplus
}
#endif

#endif /* MEMPOOL_H */
