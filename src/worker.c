#include "worker.h"
#include "parser.h"
#include "routing.h"
#include "nat.h"
#include "neighbor.h"
#include "forward.h"

#include <signal.h>
#include <netinet/in.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_cycles.h>

#define WORKER_BURST 32
#define NAT_MAINTENANCE_INTERVAL_SEC 1

struct worker_context {
    uint16_t port_id;
    uint16_t rx_queue;
    uint16_t tx_queue;
};

static volatile sig_atomic_t worker_quit;
static struct worker_stats stats[RTE_MAX_LCORE];
static struct worker_context contexts[RTE_MAX_LCORE];

static int worker_loop(void *arg)
{
    struct worker_context *ctx = arg;
    unsigned lcore_id = rte_lcore_id();
    struct worker_stats *s = &stats[lcore_id];
    struct rte_mbuf *bufs[WORKER_BURST];
    uint64_t last_nat_maintenance = 0;
    uint64_t maintenance_interval =
        (uint64_t)NAT_MAINTENANCE_INTERVAL_SEC * rte_get_timer_hz();

    while (!worker_quit) {
        /* One worker owns periodic NAT table maintenance. The mapping used
         * by main.c always assigns port 0 / queue 0 to exactly one worker. */
        if (ctx->port_id == 0 && ctx->rx_queue == 0) {
            uint64_t now = rte_get_timer_cycles();

            if (last_nat_maintenance == 0 ||
                now - last_nat_maintenance >= maintenance_interval) {
                nat_maintenance();
                last_nat_maintenance = now;
            }
        }

        uint16_t n = rte_eth_rx_burst(ctx->port_id, ctx->rx_queue,
                                      bufs, WORKER_BURST);
        if (n == 0)
            continue;

        s->rx += n;

        for (uint16_t i = 0; i < n; i++) {
            struct packet_info info;
            struct rte_ether_addr dst_mac;
            uint16_t egress_port;
            int control;

            control = neighbor_handle_control(bufs[i], ctx->port_id,
                                              ctx->tx_queue);
            if (control < 0) {
                s->drops++;
                rte_pktmbuf_free(bufs[i]);
                continue;
            }
            if (control > 0)
                continue;

            if (parse_packet(bufs[i], &info) < 0) {
                s->drops++;
                rte_pktmbuf_free(bufs[i]);
                continue;
            }

            s->parsed++;

            if (info.ip_version == 4 &&
                (info.protocol == IPPROTO_TCP ||
                 info.protocol == IPPROTO_UDP)) {

                int nat_result;

                if (nat_is_public_destination(&info))
                    nat_result = nat_translate_inbound(&info);
                else
                    nat_result = nat_translate_outbound(&info);

                if (nat_result != 0) {
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

            if (egress_port >= rte_eth_dev_count_avail()) {
                s->drops++;
                rte_pktmbuf_free(bufs[i]);
                continue;
            }

            if (forward_prepare(bufs[i], &info) != 0) {
                /* TTL/Hop-Limit expiry or malformed L3 packet. */
                s->drops++;
                rte_pktmbuf_free(bufs[i]);
                continue;
            }

            if (neighbor_resolve(&info, egress_port, &dst_mac) != 0) {
                /* ARP/ND must populate the neighbor cache before forwarding. */
                s->drops++;
                rte_pktmbuf_free(bufs[i]);
                continue;
            }

            if (neighbor_rewrite_l2(bufs[i], egress_port, &dst_mac) != 0) {
                s->drops++;
                rte_pktmbuf_free(bufs[i]);
                continue;
            }

            if (rte_eth_tx_burst(egress_port, ctx->tx_queue, &bufs[i], 1) == 1)
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
    if (!rte_lcore_is_enabled(lcore_id) || lcore_id >= RTE_MAX_LCORE)
        return -1;

    contexts[lcore_id].port_id = port_id;
    contexts[lcore_id].rx_queue = rx_queue;
    contexts[lcore_id].tx_queue = tx_queue;

    return rte_eal_remote_launch(worker_loop, &contexts[lcore_id], lcore_id);
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
