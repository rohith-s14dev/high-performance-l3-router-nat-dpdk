#include "worker.h"
#include "parser.h"
#include "routing.h"
#include "nat.h"

#include <signal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>

#define WORKER_BURST 32

static volatile sig_atomic_t worker_quit;
static struct worker_stats stats[RTE_MAX_LCORE];

static int worker_loop(void *arg)
{
    uint16_t port_id = (uint16_t)(uintptr_t)arg;
    unsigned lcore_id = rte_lcore_id();
    struct worker_stats *s = &stats[lcore_id];
    struct rte_mbuf *bufs[WORKER_BURST];

    while (!worker_quit) {
        uint16_t n = rte_eth_rx_burst(port_id, 0, bufs, WORKER_BURST);
        if (n == 0)
            continue;

        s->rx += n;

        for (uint16_t i = 0; i < n; i++) {
            struct packet_info info;
            uint16_t egress_port;

            if (parse_packet(bufs[i], &info) < 0) {
                s->drops++;
                rte_pktmbuf_free(bufs[i]);
                continue;
            }

            s->parsed++;

            if (info.ip_version == 4 &&
                (info.protocol == IPPROTO_TCP ||
                 info.protocol == IPPROTO_UDP)) {
                if (nat_translate_outbound(&info) != 0) {
                    s->drops++;
                    rte_pktmbuf_free(bufs[i]);
                    continue;
                }
                s->nat_packets++;
            }

            if (routing_lookup(&info, &egress_port) != 0) {
                s->route_misses++;
                s->drops++;
                rte_pktmbuf_free(bufs[i]);
                continue;
            }

            s->route_hits++;

            if (rte_eth_tx_burst(egress_port, 0, &bufs[i], 1) == 1)
                s->tx++;
            else {
                s->drops++;
                rte_pktmbuf_free(bufs[i]);
            }
        }
    }

    return 0;
}

int worker_launch(uint16_t port_id, uint16_t rx_queue, uint16_t tx_queue,
                  unsigned lcore_id)
{
    (void)rx_queue;
    (void)tx_queue;

    if (!rte_lcore_is_enabled(lcore_id))
        return -1;

    return rte_eal_remote_launch(worker_loop, (void *)(uintptr_t)port_id,
                                 lcore_id);
}

void worker_print_stats(void)
{
    for (unsigned i = 0; i < RTE_MAX_LCORE; i++) {
        const struct worker_stats *s = &stats[i];

        if (s->rx == 0 && s->tx == 0 && s->drops == 0)
            continue;

        printf("lcore %u: RX=%lu parsed=%lu route_hit=%lu "
               "route_miss=%lu NAT=%lu TX=%lu drops=%lu\n",
               i, s->rx, s->parsed, s->route_hits, s->route_misses,
               s->nat_packets, s->tx, s->drops);
    }
}

void worker_stop(void)
{
    worker_quit = 1;
}
