#include "common.h"
#include "port_init.h"

int port_init(uint16_t port, struct rte_mempool *mbuf_pool)
{
    struct rte_eth_conf port_conf = {0};
    struct rte_eth_dev_info dev_info;
    int ret;

    if (!rte_eth_dev_is_valid_port(port))
        return -1;

    ret = rte_eth_dev_info_get(port, &dev_info);
    if (ret != 0)
        return ret;

    ret = rte_eth_dev_configure(port, 1, 1, &port_conf);
    if (ret < 0)
        return ret;

    ret = rte_eth_rx_queue_setup(port, 0, RX_RING_SIZE,
                                 rte_eth_dev_socket_id(port), NULL,
                                 mbuf_pool);
    if (ret < 0)
        return ret;

    ret = rte_eth_tx_queue_setup(port, 0, TX_RING_SIZE,
                                 rte_eth_dev_socket_id(port), NULL);
    if (ret < 0)
        return ret;

    ret = rte_eth_dev_start(port);
    if (ret < 0)
        return ret;

    rte_eth_promiscuous_enable(port);
    return 0;
}
