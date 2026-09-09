#include "common.h"
#include "port_init.h"

static struct rte_mempool *mbuf_pool;

int main(int argc, char **argv)
{
    uint16_t port;
    uint16_t nb_ports;

    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Failed to initialize DPDK EAL\n");

    argc -= ret;
    argv += ret;
    (void)argc;
    (void)argv;

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

    printf("DPDK router Phase 1 started on port %u\n", port);

    struct rte_mbuf *bufs[BURST_SIZE];

    while (1) {
        uint16_t nb_rx = rte_eth_rx_burst(port, 0, bufs, BURST_SIZE);
        if (nb_rx == 0)
            continue;

        uint16_t nb_tx = rte_eth_tx_burst(port, 0, bufs, nb_rx);

        for (uint16_t i = nb_tx; i < nb_rx; i++)
            rte_pktmbuf_free(bufs[i]);
    }

    return 0;
}
