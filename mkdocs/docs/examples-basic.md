# Examples – Basic Usage

This page shows a small, self-contained program that initializes a pool, allocates
a few blocks, and prints basic statistics.

```c
#include <stdio.h>
#include <stdint.h>
#include "mempool.h"

#define POOL_BYTES 4096U

static uint8_t state_buf[MEMPOOL_STATE_SIZE] __attribute__((aligned(8)));
static uint8_t pool_buf[POOL_BYTES]          __attribute__((aligned(8)));

int main(void)
{
    mempool_t *pool = NULL;

    if (mempool_state_size() > sizeof(state_buf)) {
        printf("State buffer too small (need %zu bytes)\n",
               mempool_state_size());
        return 1;
    }

    mempool_error_t err = mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf, sizeof(pool_buf),
        64U, 8U, &pool
    );
    if (err != MEMPOOL_OK) {
        printf("mempool_init failed: %s\n", mempool_strerror(err));
        return 1;
    }

    mempool_stats_t stats;
    (void)mempool_get_stats(pool, &stats);
    printf("Pool initialized: %u blocks of %u bytes\n",
           stats.total_blocks, stats.block_size);

    void *a = NULL;
    void *b = NULL;

    err = mempool_alloc(pool, &a);
    printf("alloc a -> %s (ptr=%p)\n",
           mempool_strerror(err), (void *)a);

    err = mempool_alloc(pool, &b);
    printf("alloc b -> %s (ptr=%p)\n",
           mempool_strerror(err), (void *)b);

    (void)mempool_get_stats(pool, &stats);
    printf("Used blocks: %u / %u (peak: %u)\n",
           stats.used_blocks, stats.total_blocks, stats.peak_usage);

    (void)mempool_free(pool, a);
    (void)mempool_free(pool, b);

    (void)mempool_get_stats(pool, &stats);
    printf("After free: used=%u free=%u\n",
           stats.used_blocks, stats.free_blocks);

    return 0;
}
```

Compile and run (for example):

```bash
gcc -std=c11 -Wall -Wextra -Iinclude     examples/basic_usage.c src/mempool.c     -o example_basic
./example_basic
```
