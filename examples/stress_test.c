#include <stdio.h>
#include <stdint.h>

#include "mempool.h"

#define TEST_ALIGNMENT 8U

int main(void)
{
    uint8_t buffer[8192] __attribute__((aligned(TEST_ALIGNMENT)));
    mempool_t pool;
    void *blocks[100];
    uint32_t i;
    uint32_t cycle;

    printf("=== Stress Test Example ===\n");

    if (mempool_init(&pool, buffer, sizeof(buffer), 64U, TEST_ALIGNMENT) != MEMPOOL_OK) {
        printf("Failed to initialize pool\n");
        return 1;
    }

    for (i = 0U; i < 100U; i++) {
        blocks[i] = NULL;
    }

    for (cycle = 0U; cycle < 10U; cycle++) {
        for (i = 0U; i < 100U; i++) {
            if (mempool_alloc(&pool, &blocks[i]) != MEMPOOL_OK) {
                blocks[i] = NULL;
            }
        }

        for (i = 0U; i < 100U; i++) {
            if (blocks[i] != NULL) {
                (void)mempool_free(&pool, blocks[i]);
                blocks[i] = NULL;
            }
        }
    }

    {
        mempool_stats_t stats;
        (void)mempool_get_stats(&pool, &stats);
        printf("Stress test complete. Used blocks: %u, alloc_count: %u, free_count: %u\n",
               stats.used_blocks, stats.alloc_count, stats.free_count);
    }

    return 0;
}
