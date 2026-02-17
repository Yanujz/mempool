/**
 * @file mempool_cfg.h
 * @brief Compile-time configuration for the mempool library.
 *
 * Override any of these defaults by defining the macros before including
 * mempool.h, or via compiler flags (e.g. -DMEMPOOL_ENABLE_STATS=0).
 */

#ifndef MEMPOOL_CFG_H
#define MEMPOOL_CFG_H

/*
 * MEMPOOL_ENABLE_STATS
 *   1 = track alloc/free counts, peak usage, used/free block counts.
 *   0 = omit all statistics (saves RAM and cycles).
 */
#ifndef MEMPOOL_ENABLE_STATS
#define MEMPOOL_ENABLE_STATS 1
#endif

/*
 * MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
 *   1 = maintain a bitmap to detect double-free errors.
 *   0 = skip bitmap; mempool_free trusts the caller (fastest path).
 */
#ifndef MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
#define MEMPOOL_ENABLE_DOUBLE_FREE_CHECK 1
#endif

/*
 * MEMPOOL_ENABLE_STRERROR
 *   1 = compile mempool_strerror() with human-readable strings.
 *   0 = omit to save flash on constrained targets.
 */
#ifndef MEMPOOL_ENABLE_STRERROR
#define MEMPOOL_ENABLE_STRERROR 1
#endif

/*
 * MEMPOOL_LOCK / MEMPOOL_UNLOCK
 *   User-defined macros for critical-section protection.
 *   Define them to your platform primitives in this file or via compiler
 *   flags.  When left undefined, no synchronization is compiled in (zero
 *   overhead).
 *
 *   Example (FreeRTOS):
 *     #define MEMPOOL_LOCK()   taskENTER_CRITICAL()
 *     #define MEMPOOL_UNLOCK() taskEXIT_CRITICAL()
 *
 *   Example (bare-metal Cortex-M):
 *     #define MEMPOOL_LOCK()   __disable_irq()
 *     #define MEMPOOL_UNLOCK() __enable_irq()
 *
 *   Example (POSIX):
 *     extern pthread_mutex_t mempool_mtx;
 *     #define MEMPOOL_LOCK()   pthread_mutex_lock(&mempool_mtx)
 *     #define MEMPOOL_UNLOCK() pthread_mutex_unlock(&mempool_mtx)
 */
#ifndef MEMPOOL_LOCK
#define MEMPOOL_LOCK()   ((void)0)
#endif
#ifndef MEMPOOL_UNLOCK
#define MEMPOOL_UNLOCK() ((void)0)
#endif

/*
 * MEMPOOL_STATE_SIZE
 *   Size in bytes for the opaque state buffer provided by the caller.
 *   Must be >= sizeof(struct mempool). Increase if the static assert fires.
 */
#ifndef MEMPOOL_STATE_SIZE
#define MEMPOOL_STATE_SIZE 80U
#endif

#endif /* MEMPOOL_CFG_H */
