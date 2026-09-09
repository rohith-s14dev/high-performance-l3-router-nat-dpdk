#include "common.h"
#include "parser.h"
#include "port_init.h"

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
    uint16_t port;
    uint16_t nb_ports;
    uint64_t rx_packets = 0;
    uint64_t parsed_packets = 0;
    uint64_t parse_errors = 0;
    uint64_t tx_packets = 0;
    uint64_t dropped_packets = 0;

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

    port = 0;
    if (port_init(port, mbuf_pool) != 0)
        rte_exit(EXIT_FAILURE, "Cannot initialize port %u\n", port);

    printf("DPDK router Phase 2 started on port %u\n", port);
    printf("Parser: Ethernet/VLAN/IPv4/IPv6/TCP/UDP\n");

    struct rte_mbuf *bufs[BURST_SIZE];

    while (!force_quit) {
        uint16_t nb_rx = rte_eth_rx_burst(port, 0, bufs, BURST_SIZE);
        if (nb_rx == 0)
            continue;

        rx_packets += nb_rx;

        for (uint16_t i = 0; i < nb_rx; i++) {
            struct packet_info info;

            if (parse_packet(bufs[i], &info) < 0) {
                parse_errors++;
                dropped_packets++;
                rte_pktmbuf_free(bufs[i]);
                continue;
            }

            parsed_packets++;
            print_packet_info(&info);

            /*
             * Phase 2 only parses and inspects packets.
             * Phase 3 will use the parsed destination address
             * for IPv4/IPv6 LPM routing decisions.
             */
            uint16_t nb_tx = rte_eth_tx_burst(port, 0, &bufs[i], 1);

            if (nb_tx == 1)
                tx_packets++;
            else {
                dropped_packets++;
                rte_pktmbuf_free(bufs[i]);
            }
        }
    }

    printf("\n=== Phase 2 Statistics ===\n");
    printf("RX packets      : %lu\n", rx_packets);
    printf("Parsed packets  : %lu\n", parsed_packets);
    printf("Parse errors    : %lu\n", parse_errors);
    printf("TX packets      : %lu\n", tx_packets);
    printf("Dropped packets : %lu\n", dropped_packets);

    rte_eth_dev_stop(port);
    rte_eth_dev_close(port);
    rte_eal_cleanup();

    return 0;
}
