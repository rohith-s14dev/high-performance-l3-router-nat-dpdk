#ifndef ROUTER_PORT_INIT_H
#define ROUTER_PORT_INIT_H

#include "common.h"

int port_init(uint16_t port, uint16_t nb_queues,
              struct rte_mempool *mbuf_pool);
void port_stop(uint16_t port);

#endif
