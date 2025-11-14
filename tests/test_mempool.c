/*
 * Minimal C test harness.
 *
 * For thorough testing (including multithreaded scenarios) use the
 * GoogleTest-based suite in tests/gtest_mempool.cpp.
 */

#include <stdio.h>
#include <stdint.h>

#include "mempool.h"

#define TEST_BUFFER_SIZE 4096U

static int run_basic_tests(void)
{
    uint8_t buffer[TEST_BUFFER_SIZE] __attribute__((aligned(8)));
    mempool_t pool;
    mempool_error_t err;
    void *block1 = NULL;
    void *block2 = NULL;
    mempool_stats_t stats;

    printf("Running basic C tests...\n");

    err = mempool_init(&pool, buffer, TEST_BUFFER_SIZE, 64U, 8U);
    if (err != MEMPOOL_OK) {
        printf("mempool_init failed: %s\n", mempool_strerror(err));
        return 1;
    }

    err = mempool_alloc(&pool, &block1);
    if (err != MEMPOOL_OK) {
        printf("mempool_alloc block1 failed: %s\n", mempool_strerror(err));
        return 1;
    }

    err = mempool_alloc(&pool, &block2);
    if (err != MEMPOOL_OK) {
        printf("mempool_alloc block2 failed: %s\n", mempool_strerror(err));
        return 1;
    }

    (void)mempool_get_stats(&pool, &stats);
    printf("Used blocks after 2 allocs: %u\n", stats.used_blocks);

    (void)mempool_free(&pool, block1);
    (void)mempool_free(&pool, block2);

    (void)mempool_get_stats(&pool, &stats);
    printf("Used blocks after frees: %u\n", stats.used_blocks);

    /* Double-free should now be detected. */
    err = mempool_free(&pool, block1);
    if (err != MEMPOOL_ERR_DOUBLE_FREE) {
        printf("Expected double-free error, got: %s\n", mempool_strerror(err));
        return 1;
    }

    printf("Basic C tests passed.\n");
    return 0;
}

int main(void)
{
    int rc = run_basic_tests();
    return rc;
}
