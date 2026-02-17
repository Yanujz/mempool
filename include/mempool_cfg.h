/**
 * @file mempool_cfg.h
 * @brief Compile-time configuration for the mempool library.
 *
 * Override any of these defaults by defining the macros before including
 * mempool.h, or via compiler flags (-DMEMPOOL_ENABLE_STATS=0).
 */

#ifndef MEMPOOL_CFG_H
#define MEMPOOL_CFG_H

/**
 * MEMPOOL_ENABLE_STATS
 *   1 = track alloc/free counts, peak usage, used/free block counts.
 *   0 = omit all statistics (saves RAM and cycles).
 * Default: 1
 */
#ifndef MEMPOOL_ENABLE_STATS
#define MEMPOOL_ENABLE_STATS 1
#endif

/**
 * MEMPOOL_ENABLE_SYNC
 *   1 = compile in lock/unlock callback support (mempool_set_sync).
 *   0 = omit synchronization entirely (saves RAM, removes branch).
 * Default: 1
 */
#ifndef MEMPOOL_ENABLE_SYNC
#define MEMPOOL_ENABLE_SYNC 1
#endif

/**
 * MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
 *   1 = maintain a bitmap to detect double-free errors.
 *   0 = skip bitmap; mempool_free trusts the caller (fastest path).
 * Default: 1
 */
#ifndef MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
#define MEMPOOL_ENABLE_DOUBLE_FREE_CHECK 1
#endif

/**
 * MEMPOOL_STATE_SIZE
 *   Size in bytes for the opaque state buffer provided by the caller.
 *   Must be >= sizeof(struct mempool). Increase if the static assert fires.
 * Default: 128 (sufficient for all features enabled on 32/64-bit targets)
 */
#ifndef MEMPOOL_STATE_SIZE
#define MEMPOOL_STATE_SIZE 128U
#endif

#endif /* MEMPOOL_CFG_H */
