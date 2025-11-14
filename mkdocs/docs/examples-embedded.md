# Examples – Embedded / Packet Buffers

A common pattern in embedded systems is to use a fixed pool of packet buffers
or message objects that are recycled.

The following example shows a simple packet structure allocated from a pool.

```c
#include <stdio.h>
#include <stdint.h>
#include "mempool.h"

#define PACKET_SIZE 256U
#define MAX_PACKETS 16U

typedef struct {
    uint8_t  data[PACKET_SIZE];
    uint32_t length;
    uint32_t timestamp;
} packet_t;

static uint8_t state_buf[MEMPOOL_STATE_SIZE] __attribute__((aligned(8)));
static uint8_t pool_buf[PACKET_SIZE * MAX_PACKETS] __attribute__((aligned(8)));

int main(void)
{
    mempool_t *pool = NULL;

    if (mempool_state_size() > sizeof(state_buf)) {
        /* configuration error */
        return 1;
    }

    mempool_error_t err = mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf, sizeof(pool_buf),
        sizeof(packet_t), 8U,
        &pool
    );
    if (err != MEMPOOL_OK) {
        printf("mempool_init failed: %s\n", mempool_strerror(err));
        return 1;
    }

    packet_t *tx = NULL;
    packet_t *rx = NULL;

    (void)mempool_alloc(pool, (void **)&tx);
    (void)mempool_alloc(pool, (void **)&rx);

    if ((tx != NULL) && (rx != NULL)) {
        tx->length    = 128U;
        tx->timestamp = 1000U;

        rx->length    = 64U;
        rx->timestamp = 1001U;

        printf("TX length=%u RX length=%u\n",
               (unsigned)tx->length, (unsigned)rx->length);
    }

    (void)mempool_free(pool, tx);
    (void)mempool_free(pool, rx);

    return 0;
}
```

On a real embedded target, you would typically:

- place `state_buf` and `pool_buf` in specific memory sections,
- possibly align `pool_buf` for DMA,
- and integrate with your RTOS or scheduler.
