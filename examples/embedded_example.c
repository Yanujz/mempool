#include <stdio.h>
#include <stdint.h>

#include "mempool.h"

#define PACKET_SIZE 256U
#define MAX_PACKETS 16U

typedef struct {
    uint8_t data[PACKET_SIZE];
    uint32_t length;
    uint32_t timestamp;
} packet_t;

static void example_embedded(void)
{
    printf("\n=== Embedded System Example ===\n");
    printf("Simulating packet buffer management\n");

    static uint8_t packet_buffer[PACKET_SIZE * MAX_PACKETS]
        __attribute__((aligned(8)));
    mempool_t packet_pool;
    packet_t *tx_packet = NULL;
    packet_t *rx_packet = NULL;
    mempool_stats_t stats;

    (void)mempool_init(&packet_pool, packet_buffer, sizeof(packet_buffer),
                 sizeof(packet_t), 8U);

    (void)mempool_alloc(&packet_pool, (void **)&tx_packet);
    if (tx_packet != NULL) {
        tx_packet->length = 128U;
        tx_packet->timestamp = 1000U;
        printf("TX packet prepared\n");
    }

    (void)mempool_alloc(&packet_pool, (void **)&rx_packet);
    if (rx_packet != NULL) {
        rx_packet->length = 64U;
        rx_packet->timestamp = 1001U;
        printf("RX packet received\n");
    }

    (void)mempool_get_stats(&packet_pool, &stats);
    printf("Packets in use: %u/%u (peak: %u)\n",
           stats.used_blocks, stats.total_blocks, stats.peak_usage);

    (void)mempool_free(&packet_pool, tx_packet);
    (void)mempool_free(&packet_pool, rx_packet);
    printf("Packets processed and freed\n");
}

int main(void)
{
    example_embedded();
    return 0;
}
