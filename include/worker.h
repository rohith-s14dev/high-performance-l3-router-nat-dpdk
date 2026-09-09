#ifndef ROUTER_WORKER_H
#define ROUTER_WORKER_H

#include <stdint.h>
#include <rte_mbuf.h>

struct worker_stats {
    uint64_t rx;
    uint64_t parsed;
    uint64_t route_hits;
    uint64_t route_misses;
    uint64_t nat_packets;
    uint64_t drops;
    uint64_t tx;
};

int worker_launch(uint16_t port_id, uint16_t rx_queue, uint16_t tx_queue,
                  unsigned lcore_id);
void worker_print_stats(void);
void worker_stop(void);

#endif
