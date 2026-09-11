#include "common.h"
#include "port_init.h"

int port_init(uint16_t port, uint16_t nb_queues, struct rte_mempool *mbuf_pool)
{
    struct rte_eth_conf port_conf = {0};
    struct rte_eth_dev_info dev_info;
    int ret;

    if (!rte_eth_dev_is_valid_port(port) || nb_queues == 0)
        return -1;

    ret = rte_eth_dev_info_get(port, &dev_info);
    if (ret != 0)
        return ret;

    if (nb_queues > dev_info.max_rx_queues || nb_queues > dev_info.max_tx_queues)
        return -1;

    if (nb_queues > 1) {
        port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
        port_conf.rx_adv_conf.rss_conf.rss_hf =
            RTE_ETH_RSS_IPV4 | RTE_ETH_RSS_IPV6 |
            RTE_ETH_RSS_NONFRAG_IPV4_TCP | RTE_ETH_RSS_NONFRAG_IPV6_TCP |
            RTE_ETH_RSS_NONFRAG_IPV4_UDP | RTE_ETH_RSS_NONFRAG_IPV6_UDP;
        port_conf.rx_adv_conf.rss_conf.rss_hf &= dev_info.flow_type_rss_offloads;
    }

    ret = rte_eth_dev_configure(port, nb_queues, nb_queues, &port_conf);
    if (ret < 0)
        return ret;

    for (uint16_t q = 0; q < nb_queues; q++) {
        ret = rte_eth_rx_queue_setup(port, q, RX_RING_SIZE,
                                     rte_eth_dev_socket_id(port), NULL,
                                     mbuf_pool);
        if (ret < 0)
            return ret;

        ret = rte_eth_tx_queue_setup(port, q, TX_RING_SIZE,
                                     rte_eth_dev_socket_id(port), NULL);
        if (ret < 0)
            return ret;
    }

    ret = rte_eth_dev_start(port);
    if (ret < 0)
        return ret;

    rte_eth_promiscuous_enable(port);
    return 0;
}

void port_stop(uint16_t port)
{
    if (!rte_eth_dev_is_valid_port(port))
        return;

    rte_eth_dev_stop(port);
    rte_eth_dev_close(port);
}
