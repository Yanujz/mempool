/**
 * @file mempool_mgr.h
 * @brief Multi-tier pool manager — routes allocations to the right-sized pool.
 *
 * Real embedded systems rarely have a single block size.  mempool_mgr wraps
 * N mempool instances sorted by ascending block_size.  An allocation request
 * is fulfilled by the *smallest* pool whose block_size >= the requested size
 * that still has free blocks; if all fitting pools are exhausted it falls
 * through to the next larger pool.
 *
 * Usage example
 * -------------
 * @code
 *   // Three pools: 32 B, 256 B, 2 KB
 *   alignas(8) uint8_t s0[MEMPOOL_STATE_SIZE], p0[32  * 64];
 *   alignas(8) uint8_t s1[MEMPOOL_STATE_SIZE], p1[256 * 16];
 *   alignas(8) uint8_t s2[MEMPOOL_STATE_SIZE], p2[2048 * 4];
 *
 *   mempool_t *pools[3];
 *   mempool_init(s0, sizeof s0, p0, sizeof p0, 32,   8, &pools[0]);
 *   mempool_init(s1, sizeof s1, p1, sizeof p1, 256,  8, &pools[1]);
 *   mempool_init(s2, sizeof s2, p2, sizeof p2, 2048, 8, &pools[2]);
 *
 *   mempool_mgr_t mgr;
 *   mempool_mgr_init(&mgr, pools, 3);
 *
 *   void *buf; mempool_t *owner;
 *   mempool_mgr_alloc(&mgr, 20, &buf, &owner);   // picks pool[0]
 *   mempool_mgr_free(&mgr, buf);                  // finds pool[0] automatically
 * @endcode
 */

#ifndef MEMPOOL_MGR_H
#define MEMPOOL_MGR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "mempool.h"

/* Maximum number of pools a manager can hold. Override via compiler flag. */
#ifndef MEMPOOL_MGR_MAX_POOLS
#define MEMPOOL_MGR_MAX_POOLS 8U
#endif

/**
 * Pool manager state.  Embed this in your application or allocate statically;
 * no dynamic allocation is performed.
 */
typedef struct {
    mempool_t *pools[MEMPOOL_MGR_MAX_POOLS]; /**< Pool handles, sorted ascending by block_size */
    uint32_t   count;                        /**< Number of active pools                       */
} mempool_mgr_t;

/**
 * Initialise the manager.
 *
 * @param mgr    Manager to initialise.
 * @param pools  Array of @p count already-initialised mempool handles.
 *               The pools do NOT need to be sorted; the manager sorts them
 *               internally by ascending block_size.
 * @param count  Number of pools (1 … MEMPOOL_MGR_MAX_POOLS).
 * @return MEMPOOL_OK, or MEMPOOL_ERR_NULL_PTR / MEMPOOL_ERR_INVALID_SIZE.
 */
mempool_error_t mempool_mgr_init(mempool_mgr_t *mgr,
                                 mempool_t    **pools,
                                 uint32_t       count);

/**
 * Allocate a block of at least @p min_size bytes.
 *
 * The manager tries each pool (in ascending block_size order) whose
 * block_size >= min_size, returning the first successful allocation.
 *
 * @param mgr       Manager.
 * @param min_size  Minimum usable bytes required.
 * @param block     Receives the allocated block pointer.
 * @param pool_out  Optional; receives the owning pool handle (may be NULL).
 * @return MEMPOOL_OK on success.
 *         MEMPOOL_ERR_INVALID_SIZE if no pool can satisfy min_size.
 *         MEMPOOL_ERR_OUT_OF_MEMORY if all fitting pools are exhausted.
 */
mempool_error_t mempool_mgr_alloc(mempool_mgr_t *mgr,
                                  size_t         min_size,
                                  void         **block,
                                  mempool_t    **pool_out);

/**
 * Return a block to the pool that owns it.
 *
 * Uses mempool_contains() to identify the owning pool in O(N) where N is
 * the number of pools (typically 2–4).
 *
 * @return MEMPOOL_OK on success, MEMPOOL_ERR_INVALID_BLOCK if no pool owns
 *         the pointer.
 */
mempool_error_t mempool_mgr_free(mempool_mgr_t *mgr, void *block);

#ifdef __cplusplus
}
#endif

#endif /* MEMPOOL_MGR_H */
