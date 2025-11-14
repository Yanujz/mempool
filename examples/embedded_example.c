#include <stdio.h>
// #include <stdint.h>
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
    mempool_error_t err;
    packet_t *tx_packet = NULL;
    packet_t *rx_packet = NULL;
    mempool_stats_t stats;

    printf("=== Embedded Example ===\n");

    if (mempool_state_size() > sizeof(state_buf)) {
        printf("State buffer too small for this build\n");
        return 1;
    }

    err = mempool_init(state_buf, sizeof(state_buf),
                       pool_buf, sizeof(pool_buf),
                       sizeof(packet_t), 8U,
                       &pool);
    if (err != MEMPOOL_OK) {
        printf("mempool_init failed: %s\n", mempool_strerror(err));
        return 1;
    }

    err = mempool_alloc(pool, (void **)&tx_packet);
    if ((err == MEMPOOL_OK) && (tx_packet != NULL)) {
        tx_packet->length    = 128U;
        tx_packet->timestamp = 1000U;
        printf("TX packet prepared\n");
    }

    err = mempool_alloc(pool, (void **)&rx_packet);
    if ((err == MEMPOOL_OK) && (rx_packet != NULL)) {
        rx_packet->length    = 64U;
        rx_packet->timestamp = 1001U;
        printf("RX packet received\n");
    }

    (void)mempool_get_stats(pool, &stats);
    printf("Packets in use: %u/%u (peak: %u)\n",
           stats.used_blocks, stats.total_blocks, stats.peak_usage);

    (void)mempool_free(pool, tx_packet);
    (void)mempool_free(pool, rx_packet);

    printf("Packets processed and freed\n");
    return 0;
}
