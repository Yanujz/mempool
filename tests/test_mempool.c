#include <stdio.h>
#include <stdint.h>
#include "mempool.h"

#define TEST_POOL_BYTES 4096U

static int run_basic_tests(void)
{
    uint8_t state_buf[MEMPOOL_STATE_SIZE] __attribute__((aligned(8)));
    uint8_t pool_buf[TEST_POOL_BYTES]     __attribute__((aligned(8)));

    mempool_t *pool = NULL;
    mempool_error_t err;
    mempool_stats_t stats;
    void *b1 = NULL;
    void *b2 = NULL;

    printf("Running basic C tests...\n");

    if (mempool_state_size() > sizeof(state_buf)) {
        printf("State buffer too small for this build\n");
        return 1;
    }

    err = mempool_init(state_buf, sizeof(state_buf),
                       pool_buf, sizeof(pool_buf),
                       64U, 8U, &pool);
    if (err != MEMPOOL_OK) {
        printf("mempool_init failed: %s\n", mempool_strerror(err));
        return 1;
    }

    err = mempool_get_stats(pool, &stats);
    if (err != MEMPOOL_OK) {
        printf("mempool_get_stats failed\n");
        return 1;
    }
    printf("Total blocks: %u, free: %u\n",
           stats.total_blocks, stats.free_blocks);

    err = mempool_alloc(pool, &b1);
    if (err != MEMPOOL_OK) {
        printf("alloc b1 failed: %s\n", mempool_strerror(err));
        return 1;
    }
    err = mempool_alloc(pool, &b2);
    if (err != MEMPOOL_OK) {
        printf("alloc b2 failed: %s\n", mempool_strerror(err));
        return 1;
    }

    (void)mempool_get_stats(pool, &stats);
    printf("Used after 2 alloc: %u\n", stats.used_blocks);

    (void)mempool_free(pool, b1);
    (void)mempool_free(pool, b2);

    (void)mempool_get_stats(pool, &stats);
    printf("Used after frees: %u\n", stats.used_blocks);

    err = mempool_free(pool, b1);
    if (err != MEMPOOL_ERR_DOUBLE_FREE) {
        printf("Expected double-free error, got: %s\n", mempool_strerror(err));
        return 1;
    }

    printf("Basic C tests passed.\n");
    return 0;
}

int main(void)
{
    return run_basic_tests();
}
