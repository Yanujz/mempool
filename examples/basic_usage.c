#include <stdio.h>
#include <stdint.h>
#include "mempool.h"

#define BLOCK_SIZE  64U
#define NUM_BLOCKS  32U
#define POOL_ALIGN  8U

/*
 * MEMPOOL_POOL_BUFFER_SIZE accounts for guard canaries, bitmap, and tag
 * overhead so the buffer is always large enough regardless of build flags.
 */
static uint8_t state_buf[MEMPOOL_STATE_SIZE]
    __attribute__((aligned(POOL_ALIGN)));
static uint8_t pool_buf[MEMPOOL_POOL_BUFFER_SIZE(BLOCK_SIZE, NUM_BLOCKS, POOL_ALIGN)]
    __attribute__((aligned(POOL_ALIGN)));

int main(void)
{
    mempool_t *pool = NULL;
    mempool_error_t err;
    mempool_stats_t stats;
    void *block1 = NULL;
    void *block2 = NULL;

    if (mempool_state_size() > sizeof(state_buf)) {
        printf("State buffer too small for this build\n");
        return 1;
    }

    err = mempool_init(state_buf, sizeof(state_buf),
                       pool_buf, sizeof(pool_buf),
                       BLOCK_SIZE, POOL_ALIGN, &pool);
    if (err != MEMPOOL_OK) {
        printf("mempool_init failed: %s\n", mempool_strerror(err));
        return 1;
    }

    printf("Pool initialized successfully\n");

    (void)mempool_get_stats(pool, &stats);
    printf("Total blocks: %u, block size: %u\n",
           stats.total_blocks, stats.block_size);

    err = mempool_alloc(pool, &block1);
    if (err == MEMPOOL_OK) {
        printf("Allocated block1 at %p\n", block1);
    }

    err = mempool_alloc(pool, &block2);
    if (err == MEMPOOL_OK) {
        printf("Allocated block2 at %p\n", block2);
    }

    (void)mempool_get_stats(pool, &stats);
    printf("Blocks in use: %u/%u\n",
           stats.used_blocks, stats.total_blocks);

    (void)mempool_free(pool, block1);
    (void)mempool_free(pool, block2);

    printf("Blocks freed\n");
    return 0;
}
