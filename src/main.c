#include "common.h"
#include "port_init.h"
#include "routing.h"
#include "nat.h"
#include "worker.h"

#include <arpa/inet.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>

static struct rte_mempool *mbuf_pool;
static volatile sig_atomic_t force_quit;

static void signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM)
        force_quit = 1;
}

static int init_routes(void)
{
    if (routing_add_ipv4("10.0.0.0", 8, 0) < 0 ||
        routing_add_ipv4("192.168.1.0", 24, 0) < 0 ||
        routing_add_ipv4("0.0.0.0", 0, 1) < 0 ||
        routing_add_ipv6("2001:db8:1::", 64, 0) < 0 ||
        routing_add_ipv6("2001:db8:2::", 64, 1) < 0 ||
        routing_add_ipv6("::", 0, 1) < 0)
        return -1;

    return 0;
}

int main(int argc, char **argv)
{
    int ret;
    uint16_t nb_ports;

    ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Failed to initialize DPDK EAL\n");

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

    if (port_init(0, mbuf_pool) != 0)
        rte_exit(EXIT_FAILURE, "Cannot initialize port 0\n");

    if (routing_init(rte_socket_id()) != 0 ||
        init_routes() != 0)
        rte_exit(EXIT_FAILURE, "Cannot initialize routing\n");

    struct nat_config nat_cfg = {
        .public_ip = rte_cpu_to_be_32(RTE_IPV4(203, 0, 113, 10)),
        .public_port_min = 10000,
        .public_port_max = 60000
    };

    if (nat_init(rte_socket_id(), &nat_cfg) != 0)
        rte_exit(EXIT_FAILURE, "Cannot initialize NAT\n");

    printf("DPDK router Phase 5: multi-core packet processing\n");
    printf("Using burst processing and per-lcore statistics\n");

    unsigned workers = 0;
    unsigned lcore_id;

    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (worker_launch(0, 0, 0, lcore_id) == 0)
            workers++;
        if (workers >= 2)
            break;
    }

    if (workers == 0) {
        printf("No worker lcores available; run with at least -l 0-1.\n");
        worker_stop();
    } else {
        printf("Launched %u worker lcore(s)\n", workers);

        while (!force_quit)
            rte_pause();

        worker_stop();
        rte_eal_mp_wait_lcore();
    }

    worker_print_stats();

    nat_print_stats();
    rte_eth_dev_stop(0);
    rte_eth_dev_close(0);
    routing_free();
    nat_free();
    rte_eal_cleanup();

    return 0;
}
