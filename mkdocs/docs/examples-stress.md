# Examples – Stress & Testing

The library ships with internal stress tests as part of the test harness, but
you can also create a very simple stress program for your own environment.

```c
#include <stdio.h>
#include <stdint.h>
#include "mempool.h"

#define POOL_BYTES 8192U
#define BLOCK_SIZE 64U

static uint8_t state_buf[MEMPOOL_STATE_SIZE] __attribute__((aligned(8)));
static uint8_t pool_buf[POOL_BYTES]          __attribute__((aligned(8)));

int main(void)
{
    mempool_t *pool = NULL;

    if (mempool_state_size() > sizeof(state_buf)) {
        printf("State buffer too small.\n");
        return 1;
    }

    if (mempool_init(state_buf, sizeof(state_buf),
                     pool_buf, sizeof(pool_buf),
                     BLOCK_SIZE, 8U,
                     &pool) != MEMPOOL_OK) {
        printf("Failed to init pool.\n");
        return 1;
    }

    const uint32_t cycles = 1000U;
    const uint32_t max_blocks = 128U;

    void *blocks[128];
    for (uint32_t i = 0U; i < max_blocks; i++) {
        blocks[i] = NULL;
    }

    for (uint32_t c = 0U; c < cycles; c++) {
        /* allocate as many as we can */
        uint32_t allocated = 0U;
        for (uint32_t i = 0U; i < max_blocks; i++) {
            if (mempool_alloc(pool, &blocks[i]) == MEMPOOL_OK) {
                allocated++;
            } else {
                break;
            }
        }

        /* free them again */
        for (uint32_t i = 0U; i < max_blocks; i++) {
            if (blocks[i] != NULL) {
                (void)mempool_free(pool, blocks[i]);
                blocks[i] = NULL;
            }
        }
    }

    mempool_stats_t stats;
    (void)mempool_get_stats(pool, &stats);

    printf("Stress done. total=%u used=%u free=%u peak=%u alloc_count=%u free_count=%u\n",
           stats.total_blocks,
           stats.used_blocks,
           stats.free_blocks,
           stats.peak_usage,
           stats.alloc_count,
           stats.free_count);

    return 0;
}
```

This example is intentionally simple. For deeper stress testing, consider:

- mixing random allocation and free patterns,
- running under sanitizers (ASan, UBSan, TSAN),
- integrating with your system's typical data-flow paths.
