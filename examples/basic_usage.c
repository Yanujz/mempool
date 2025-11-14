#include <stdio.h>
#include <stdint.h>

#include "mempool.h"

static void example_basic(void)
{
    printf("\n=== Basic Usage Example ===\n");

    static uint8_t buffer[4096] __attribute__((aligned(8)));
    mempool_t pool;
    mempool_error_t err;
    void *block1;
    void *block2;
    mempool_stats_t stats;

    err = mempool_init(&pool, buffer, sizeof(buffer), 64U, 8U);
    if (err != MEMPOOL_OK) {
        printf("Failed to initialize pool: %s\n", mempool_strerror(err));
        return;
    }

    printf("Pool initialized successfully\n");
    (void)mempool_get_stats(&pool, &stats);
    printf("Total blocks: %u\n", stats.total_blocks);
    printf("Block size: %u bytes\n", stats.block_size);

    err = mempool_alloc(&pool, &block1);
    if (err == MEMPOOL_OK) {
        printf("Allocated block1 at %p\n", block1);
    }

    err = mempool_alloc(&pool, &block2);
    if (err == MEMPOOL_OK) {
        printf("Allocated block2 at %p\n", block2);
    }

    (void)mempool_get_stats(&pool, &stats);
    printf("Blocks in use: %u/%u\n", stats.used_blocks, stats.total_blocks);

    (void)mempool_free(&pool, block1);
    (void)mempool_free(&pool, block2);

    printf("Blocks freed\n");
}

int main(void)
{
    example_basic();
    return 0;
}
