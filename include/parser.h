#ifndef ROUTER_PARSER_H
#define ROUTER_PARSER_H

#include "common.h"
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>

struct packet_info {
    struct rte_ether_hdr *eth;
    struct rte_vlan_hdr *vlan;
    struct rte_ipv4_hdr *ip4;
    struct rte_ipv6_hdr *ip6;
    struct rte_tcp_hdr *tcp;
    struct rte_udp_hdr *udp;

    uint16_t ether_type;
    uint8_t ip_version;
    uint8_t protocol;
    uint16_t l3_offset;
    uint16_t l4_offset;
    uint16_t src_port;
    uint16_t dst_port;
};

int parse_packet(struct rte_mbuf *m, struct packet_info *info);

#endif
