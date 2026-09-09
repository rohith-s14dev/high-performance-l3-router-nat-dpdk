#include "common.h"
#include "parser.h"
#include "port_init.h"
#include "routing.h"
#include "nat.h"

#include <arpa/inet.h>

static struct rte_mempool *mbuf_pool;
static volatile sig_atomic_t force_quit;

static void signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM)
        force_quit = 1;
}

static void print_packet_info(const struct packet_info *p)
{
    if (p->ip_version == 4) {
        char src[INET_ADDRSTRLEN];
        char dst[INET_ADDRSTRLEN];

        inet_ntop(AF_INET, &p->ip4->src_addr, src, sizeof(src));
        inet_ntop(AF_INET, &p->ip4->dst_addr, dst, sizeof(dst));
        printf("IPv4 %s -> %s proto=%u", src, dst, p->protocol);
    } else if (p->ip_version == 6) {
        char src[INET6_ADDRSTRLEN];
        char dst[INET6_ADDRSTRLEN];

        inet_ntop(AF_INET6, &p->ip6->src_addr, src, sizeof(src));
        inet_ntop(AF_INET6, &p->ip6->dst_addr, dst, sizeof(dst));
        printf("IPv6 %s -> %s proto=%u", src, dst, p->protocol);
    } else {
        printf("Non-IP EtherType=0x%04x", p->ether_type);
        return;
    }

    if (p->protocol == IPPROTO_TCP || p->protocol == IPPROTO_UDP)
        printf(" ports=%u -> %u", p->src_port, p->dst_port);

    printf("\n");
}

int main(int argc, char **argv)
{
    uint16_t port = 0;
    uint16_t nb_ports;
    uint64_t rx_packets = 0, parsed_packets = 0;
    uint64_t route_hits = 0, route_misses = 0;
    uint64_t nat_hits = 0, nat_misses = 0;
    uint64_t tx_packets = 0, dropped_packets = 0;

    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Failed to initialize DPDK EAL\n");

    argc -= ret;
    argv += ret;
    (void)argc;
    (void)argv;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    nb_ports = rte_eth_dev_count_avail();
    if (nb_ports < 1)
        rte_exit(EXIT_FAILURE, "No available Ethernet ports\n");

    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS,
                                        MBUF_CACHE_SIZE, 0,
                                        RTE_MBUF_DEFAULT_BUF_SIZE,
                                        rte_socket_id());
    if (mbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

    if (port_init(port, mbuf_pool) != 0)
        rte_exit(EXIT_FAILURE, "Cannot initialize port %u\n", port);

    if (routing_init(rte_socket_id()) != 0)
        rte_exit(EXIT_FAILURE, "Cannot initialize routing tables\n");

    routing_add_ipv4("10.0.0.0", 8, 0);
    routing_add_ipv4("192.168.1.0", 24, 0);
    routing_add_ipv4("0.0.0.0", 0, 1);
    routing_add_ipv6("2001:db8:1::", 64, 0);
    routing_add_ipv6("2001:db8:2::", 64, 1);
    routing_add_ipv6("::", 0, 1);

    /*
     * Phase 4 NAT configuration.
     * 203.0.113.10 is documentation-only TEST-NET-3 space.
     */
    struct nat_config nat_cfg = {
        .public_ip = rte_cpu_to_be_32(
            RTE_IPV4(203, 0, 113, 10)),
        .public_port_min = 10000,
        .public_port_max = 60000
    };

    if (nat_init(rte_socket_id(), &nat_cfg) != 0)
        rte_exit(EXIT_FAILURE, "Cannot initialize NAT table\n");

    printf("DPDK router Phase 4 started on port %u\n", port);
    printf("NAT: IPv4 TCP/UDP source translation + flow tracking\n");

    struct rte_mbuf *bufs[BURST_SIZE];

    while (!force_quit) {
        uint16_t nb_rx = rte_eth_rx_burst(port, 0, bufs, BURST_SIZE);
        if (nb_rx == 0)
            continue;

        rx_packets += nb_rx;

        for (uint16_t i = 0; i < nb_rx; i++) {
            struct packet_info info;
            uint16_t egress_port;

            if (parse_packet(bufs[i], &info) < 0) {
                dropped_packets++;
                rte_pktmbuf_free(bufs[i]);
                continue;
            }

            parsed_packets++;

            /*
             * Phase 4 applies source NAT to IPv4 TCP/UDP traffic.
             * IPv6 forwarding remains unchanged.
             */
            if (info.ip_version == 4 &&
                (info.protocol == IPPROTO_TCP ||
                 info.protocol == IPPROTO_UDP)) {
                if (nat_translate_outbound(&info) != 0) {
                    nat_misses++;
                    dropped_packets++;
                    rte_pktmbuf_free(bufs[i]);
                    continue;
                }
                nat_hits++;
            }

            if (routing_lookup(&info, &egress_port) != 0) {
                route_misses++;
                dropped_packets++;
                rte_pktmbuf_free(bufs[i]);
                continue;
            }

            route_hits++;
            print_packet_info(&info);

            /*
             * Checksum correction is intentionally deferred to the
             * checksum/forwarding hardening phase. Do not use this
             * lab as a production NAT implementation yet.
             */
            uint16_t nb_tx = rte_eth_tx_burst(egress_port, 0,
                                               &bufs[i], 1);

            if (nb_tx == 1)
                tx_packets++;
            else {
                dropped_packets++;
                rte_pktmbuf_free(bufs[i]);
            }
        }
    }

    printf("\n=== Phase 4 Statistics ===\n");
    printf("RX packets      : %lu\n", rx_packets);
    printf("Parsed packets  : %lu\n", parsed_packets);
    printf("Route hits      : %lu\n", route_hits);
    printf("Route misses    : %lu\n", route_misses);
    printf("NAT packets     : %lu\n", nat_hits);
    printf("NAT misses      : %lu\n", nat_misses);
    printf("TX packets      : %lu\n", tx_packets);
    printf("Dropped packets : %lu\n", dropped_packets);

    nat_print_stats();

    rte_eth_dev_stop(port);
    rte_eth_dev_close(port);
    routing_free();
    nat_free();
    rte_eal_cleanup();

    return 0;
}
