#ifndef ROUTER_FORWARD_H
#define ROUTER_FORWARD_H

#include <stdint.h>
#include <rte_mbuf.h>

#include "parser.h"

/* Apply L3 forwarding semantics before L2 rewrite/TX.
 * Returns 0 when the packet may be forwarded, -1 when it must be dropped. */
int forward_prepare(struct rte_mbuf *m, struct packet_info *info);

#endif
