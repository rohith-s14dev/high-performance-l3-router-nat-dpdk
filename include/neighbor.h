#ifndef ROUTER_NEIGHBOR_H
#define ROUTER_NEIGHBOR_H

#include <stdint.h>
#include <rte_ether.h>
#include <rte_mbuf.h>

struct packet_info;

/* Initialize per-port local addresses and the neighbor cache. */
int neighbor_init(void);
void neighbor_free(void);

/* Learn neighbors / answer ARP and IPv6 Neighbor Solicitation messages.
 * Returns 1 when the packet was consumed (including a generated reply),
 * 0 when normal L3 processing should continue, and -1 on malformed input.
 */
int neighbor_handle_control(struct rte_mbuf *m, uint16_t ingress_port,
                            uint16_t tx_queue);

/* Resolve the next-hop L2 address for an IPv4/IPv6 packet. */
int neighbor_resolve(const struct packet_info *info, uint16_t egress_port,
                     struct rte_ether_addr *dst_mac);

/* Rewrite Ethernet source/destination MAC addresses for an outgoing packet. */
int neighbor_rewrite_l2(struct rte_mbuf *m, uint16_t egress_port,
                        const struct rte_ether_addr *dst_mac);

void neighbor_print_stats(void);

#endif
