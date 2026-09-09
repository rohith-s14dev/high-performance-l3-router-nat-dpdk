#include "parser.h"

static int parse_l4(struct rte_mbuf *m, struct packet_info *p,
                    uint16_t l4_offset, uint16_t l4_length)
{
    uint8_t *data = rte_pktmbuf_mtod(m, uint8_t *);

    if (p->protocol == IPPROTO_TCP) {
        if (l4_length < sizeof(struct rte_tcp_hdr))
            return -1;

        p->tcp = (struct rte_tcp_hdr *)(data + l4_offset);
        p->src_port = rte_be_to_cpu_16(p->tcp->src_port);
        p->dst_port = rte_be_to_cpu_16(p->tcp->dst_port);
    } else if (p->protocol == IPPROTO_UDP) {
        if (l4_length < sizeof(struct rte_udp_hdr))
            return -1;

        p->udp = (struct rte_udp_hdr *)(data + l4_offset);
        p->src_port = rte_be_to_cpu_16(p->udp->src_port);
        p->dst_port = rte_be_to_cpu_16(p->udp->dst_port);
    }

    return 0;
}

int parse_packet(struct rte_mbuf *m, struct packet_info *p)
{
    uint8_t *data;
    uint16_t offset = 0;

    if (m == NULL || p == NULL)
        return -1;

    memset(p, 0, sizeof(*p));

    if (rte_pktmbuf_pkt_len(m) < sizeof(struct rte_ether_hdr))
        return -1;

    data = rte_pktmbuf_mtod(m, uint8_t *);
    p->eth = (struct rte_ether_hdr *)data;

    p->ether_type = rte_be_to_cpu_16(p->eth->ether_type);
    offset = sizeof(struct rte_ether_hdr);

    /* Handle one 802.1Q VLAN header. */
    if (p->ether_type == RTE_ETHER_TYPE_VLAN) {
        if (rte_pktmbuf_pkt_len(m) < offset + sizeof(struct rte_vlan_hdr))
            return -1;

        p->vlan = (struct rte_vlan_hdr *)(data + offset);
        p->ether_type = rte_be_to_cpu_16(p->vlan->eth_proto);
        offset += sizeof(struct rte_vlan_hdr);
    }

    p->l3_offset = offset;

    if (p->ether_type == RTE_ETHER_TYPE_IPV4) {
        struct rte_ipv4_hdr *ip;
        uint16_t total_length;
        uint16_t header_length;
        uint32_t packet_end;

        if (rte_pktmbuf_pkt_len(m) < offset + sizeof(struct rte_ipv4_hdr))
            return -1;

        ip = (struct rte_ipv4_hdr *)(data + offset);

        if ((ip->version_ihl >> 4) != 4)
            return -1;

        header_length = (uint16_t)((ip->version_ihl & 0x0f) * 4);
        total_length = rte_be_to_cpu_16(ip->total_length);

        if (header_length < sizeof(struct rte_ipv4_hdr) ||
            total_length < header_length)
            return -1;

        packet_end = offset + total_length;

        if (packet_end > rte_pktmbuf_pkt_len(m))
            return -1;

        p->ip4 = ip;
        p->ip_version = 4;
        p->protocol = ip->next_proto_id;
        p->l4_offset = offset + header_length;

        return parse_l4(m, p, p->l4_offset,
                        total_length - header_length);
    }

    if (p->ether_type == RTE_ETHER_TYPE_IPV6) {
        struct rte_ipv6_hdr *ip6;
        uint16_t payload_length;
        uint32_t packet_end;

        if (rte_pktmbuf_pkt_len(m) < offset + sizeof(struct rte_ipv6_hdr))
            return -1;

        ip6 = (struct rte_ipv6_hdr *)(data + offset);

        if ((ip6->vtc_flow >> 28) != 6)
            return -1;

        payload_length = rte_be_to_cpu_16(ip6->payload_len);
        packet_end = offset + sizeof(struct rte_ipv6_hdr) + payload_length;

        if (packet_end > rte_pktmbuf_pkt_len(m))
            return -1;

        p->ip6 = ip6;
        p->ip_version = 6;
        p->protocol = ip6->proto;
        p->l4_offset = offset + sizeof(struct rte_ipv6_hdr);

        /*
         * Phase 2 handles the common case where the IPv6 Next Header
         * directly identifies TCP/UDP. Extension-header traversal will
         * be added as a later hardening enhancement.
         */
        return parse_l4(m, p, p->l4_offset, payload_length);
    }

    return 0;
}
