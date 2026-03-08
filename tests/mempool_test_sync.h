#ifndef MEMPOOL_TEST_SYNC_H
#define MEMPOOL_TEST_SYNC_H

#ifdef __cplusplus
extern "C" {
#endif

void mempool_test_lock(void);
void mempool_test_unlock(void);

/* Separate ISR-context lock used by the hardening test suite.
 * Maps to a different mutex than the task-level lock so that
 * mempool_drain_isr_queue() (which holds MEMPOOL_LOCK) can safely
 * acquire MEMPOOL_ISR_LOCK inside mp_flush_isr_queue() without deadlock. */
void mempool_test_isr_lock(void);
void mempool_test_isr_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* MEMPOOL_TEST_SYNC_H */
