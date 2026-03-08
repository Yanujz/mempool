/**
 * @file mempool_cfg.h
 * @brief Compile-time configuration for the mempool library.
 *
 * Override any of these defaults by defining the macros before including
 * mempool.h, or via compiler flags (e.g. -DMEMPOOL_ENABLE_STATS=0).
 */

#ifndef MEMPOOL_CFG_H
#define MEMPOOL_CFG_H

/* -------------------------------------------------------------------------
 * Core features
 * ---------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------------
 * Guard canaries — buffer-overrun detection
 * ---------------------------------------------------------------------- */

/*
 * MEMPOOL_ENABLE_GUARD
 *   1 = write a 32-bit canary word immediately after each block's user area;
 *       validate it on mempool_free().  Detects writes past the end of a
 *       block (buffer overruns into adjacent blocks).  Adds sizeof(uint32_t)
 *       bytes of overhead to the per-block stride, reducing the number of
 *       blocks that fit in a given pool buffer.
 *   0 = no guard overhead (default).
 */
#ifndef MEMPOOL_ENABLE_GUARD
#define MEMPOOL_ENABLE_GUARD 0
#endif

/*
 * MEMPOOL_CANARY_VALUE
 *   The 32-bit pattern written as the post-canary.  Choose a value unlikely
 *   to appear in user data.
 */
#ifndef MEMPOOL_CANARY_VALUE
#define MEMPOOL_CANARY_VALUE 0xDEADBEEFUL
#endif

/* -------------------------------------------------------------------------
 * Poison fill — use-after-free / uninitialised-read detection
 * ---------------------------------------------------------------------- */

/*
 * MEMPOOL_ENABLE_POISON
 *   1 = fill the user area of each block with a known byte pattern on
 *       allocation (ALLOC byte) and on free (FREE byte).  Helps surface
 *       use-after-free and reads of uninitialised memory during development.
 *       Adds a memset() call to every alloc and free.
 *   0 = no fill (default).
 */
#ifndef MEMPOOL_ENABLE_POISON
#define MEMPOOL_ENABLE_POISON 0
#endif

/* Byte written over the user area immediately before handing a block to the
 * caller (signals "freshly allocated, not yet written"). */
#ifndef MEMPOOL_ALLOC_POISON_BYTE
#define MEMPOOL_ALLOC_POISON_BYTE ((uint8_t)0xCDU)
#endif

/* Byte written over the user area when a block is returned (signals
 * "freed — do not read"). */
#ifndef MEMPOOL_FREE_POISON_BYTE
#define MEMPOOL_FREE_POISON_BYTE ((uint8_t)0xDDU)
#endif

/* -------------------------------------------------------------------------
 * Out-of-memory hook
 * ---------------------------------------------------------------------- */

/*
 * MEMPOOL_ENABLE_OOM_HOOK
 *   1 = compile support for a user-registered out-of-memory callback invoked
 *       by mempool_alloc() when the pool is exhausted.  Use
 *       mempool_set_oom_hook() to register the handler.  Adds two pointers
 *       (hook + user_data) to the pool state.
 *   0 = omit hook (default off to keep state minimal).
 */
#ifndef MEMPOOL_ENABLE_OOM_HOOK
#define MEMPOOL_ENABLE_OOM_HOOK 0
#endif

/* -------------------------------------------------------------------------
 * ISR-safe deferred free
 * ---------------------------------------------------------------------- */

/*
 * MEMPOOL_ENABLE_ISR_FREE
 *   1 = compile a fixed-capacity ring buffer so that interrupt handlers can
 *       call mempool_free_from_isr() without taking the task-level lock.
 *       Pending frees are drained by mempool_drain_isr_queue() (or lazily
 *       inside mempool_alloc()).  Requires MEMPOOL_ISR_LOCK / ISR_UNLOCK.
 *   0 = omit ISR queue (default).
 */
#ifndef MEMPOOL_ENABLE_ISR_FREE
#define MEMPOOL_ENABLE_ISR_FREE 0
#endif

/*
 * MEMPOOL_ISR_QUEUE_CAPACITY
 *   Maximum blocks that can be queued via mempool_free_from_isr() before
 *   mempool_drain_isr_queue() must process them.  Keep small on constrained
 *   targets (each slot is one pointer).
 */
#ifndef MEMPOOL_ISR_QUEUE_CAPACITY
#define MEMPOOL_ISR_QUEUE_CAPACITY 8U
#endif

/*
 * MEMPOOL_ISR_LOCK / MEMPOOL_ISR_UNLOCK
 *   Protect the ISR queue against concurrent writes from multiple ISRs
 *   (or against preemption on single-core MCUs).  On bare-metal Cortex-M:
 *     #define MEMPOOL_ISR_LOCK()   __disable_irq()
 *     #define MEMPOOL_ISR_UNLOCK() __enable_irq()
 *   On an RTOS where the task lock already disables interrupts, these can
 *   be identical to MEMPOOL_LOCK / MEMPOOL_UNLOCK.
 */
#ifndef MEMPOOL_ISR_LOCK
#define MEMPOOL_ISR_LOCK()   ((void)0)
#endif
#ifndef MEMPOOL_ISR_UNLOCK
#define MEMPOOL_ISR_UNLOCK() ((void)0)
#endif

/* -------------------------------------------------------------------------
 * Per-block caller tags
 * ---------------------------------------------------------------------- */

/*
 * MEMPOOL_ENABLE_TAGS
 *   1 = reserve a 32-bit tag word per block in the pool buffer for caller
 *       annotation (e.g. task ID, source-line hash, sequence counter).
 *       Use mempool_set_block_tag() / mempool_get_block_tag() or the
 *       MEMPOOL_ALLOC_TAGGED() macro.  Adds 4 * total_blocks bytes to the
 *       pool buffer overhead, reducing the number of blocks that fit.
 *   0 = omit tags (default).
 */
#ifndef MEMPOOL_ENABLE_TAGS
#define MEMPOOL_ENABLE_TAGS 0
#endif

/* -------------------------------------------------------------------------
 * Task-level lock (critical-section protection)
 * ---------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------------
 * State buffer sizing
 * ---------------------------------------------------------------------- */

/*
 * MEMPOOL_STATE_SIZE
 *   Size in bytes for the opaque state buffer provided by the caller.
 *   Must be >= sizeof(struct mempool).  The required size depends on which
 *   optional features are enabled:
 *
 *     Features disabled (core only):          ~  48 bytes (32-bit)
 *     All default features ON:                ~  96 bytes (64-bit)
 *     All features ON, ISR_QUEUE_CAPACITY=8:  ~ 192 bytes (64-bit)
 *
 *   256 bytes is safe for all supported feature combinations on both
 *   32-bit and 64-bit targets.  The static assert in mempool.c fires if
 *   MEMPOOL_STATE_SIZE is insufficient for the active feature set.
 */
#ifndef MEMPOOL_STATE_SIZE
#define MEMPOOL_STATE_SIZE 256U
#endif

#endif /* MEMPOOL_CFG_H */
