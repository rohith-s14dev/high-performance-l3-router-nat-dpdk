#ifndef ROUTER_PORT_INIT_H
#define ROUTER_PORT_INIT_H

#include <rte_mempool.h>
#include <stdint.h>

int port_init(uint16_t port, struct rte_mempool *mbuf_pool);

#endif
