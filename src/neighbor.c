#include "neighbor.h"
#include "parser.h"

#include <string.h>
#include <arpa/inet.h>

#include <rte_arp.h>
#include <rte_byteorder.h>
#include <rte_ethdev.h>
#include <rte_ip6.h>

#define NEIGHBOR_MAX 256
#define MAX_NEIGHBOR_PORTS 2

#define ICMP6_NEIGHBOR_SOLICITATION 135
#define ICMP6_NEIGHBOR_ADVERTISEMENT 136
#define ICMP6_OPT_TARGET_LLADDR 2

struct neighbor_entry {
    int valid;
    uint16_t port_id;
    int is_ipv6;
    uint32_t ipv4;
    struct rte_ipv6_addr ipv6;
    struct rte_ether_addr mac;
};

struct local_port {
    int valid;
    struct rte_ether_addr mac;
    uint32_t ipv4;
    struct rte_ipv6_addr ipv6;
};

static struct neighbor_entry neighbors[NEIGHBOR_MAX];
static struct local_port local_ports[MAX_NEIGHBOR_PORTS];
static uint64_t arp_learned;
static uint64_t nd_learned;
static uint64_t arp_replies;
static uint64_t nd_replies;
static uint64_t resolve_hits;
static uint64_t resolve_misses;

static uint16_t checksum_fold(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xffffU) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t checksum_bytes(const uint8_t *data, size_t len)
{
    uint32_t sum = 0;

    while (len >= 2) {
        sum += ((uint32_t)data[0] << 8) | data[1];
        data += 2;
        len -= 2;
    }

    if (len)
        sum += (uint32_t)data[0] << 8;

    return checksum_fold(sum);
}

static uint16_t icmp6_checksum(const struct rte_ipv6_hdr *ip6,
                               const uint8_t *icmp, size_t icmp_len)
{
    uint32_t sum = 0;
    const uint8_t *src = ip6->src_addr.a;
    const uint8_t *dst = ip6->dst_addr.a;

    for (unsigned i = 0; i < 16; i += 2) {
        sum += ((uint32_t)src[i] << 8) | src[i + 1];
        sum += ((uint32_t)dst[i] << 8) | dst[i + 1];
    }

    sum += (uint32_t)((icmp_len >> 24) & 0xff) << 8;
    sum += (uint32_t)((icmp_len >> 16) & 0xff);
    sum += (uint32_t)((icmp_len >> 8) & 0xff) << 8;
    sum += (uint32_t)(icmp_len & 0xff);
    sum += IPPROTO_ICMPV6;

    while (icmp_len >= 2) {
        sum += ((uint32_t)icmp[0] << 8) | icmp[1];
        icmp += 2;
        icmp_len -= 2;
    }
    if (icmp_len)
        sum += (uint32_t)icmp[0] << 8;

    return checksum_fold(sum);
}

static struct neighbor_entry *find_neighbor(uint16_t port_id, int is_ipv6,
                                             uint32_t ipv4,
                                             const struct rte_ipv6_addr *ipv6)
{
    for (unsigned i = 0; i < NEIGHBOR_MAX; i++) {
        if (!neighbors[i].valid || neighbors[i].port_id != port_id ||
            neighbors[i].is_ipv6 != is_ipv6)
            continue;

        if (!is_ipv6 && neighbors[i].ipv4 == ipv4)
            return &neighbors[i];
        if (is_ipv6 && rte_ipv6_addr_eq(&neighbors[i].ipv6, ipv6))
            return &neighbors[i];
    }

    return NULL;
}

static int learn_neighbor(uint16_t port_id, int is_ipv6, uint32_t ipv4,
                          const struct rte_ipv6_addr *ipv6,
                          const struct rte_ether_addr *mac)
{
    struct neighbor_entry *entry = find_neighbor(port_id, is_ipv6, ipv4, ipv6);

    if (!entry) {
        for (unsigned i = 0; i < NEIGHBOR_MAX; i++) {
            if (!neighbors[i].valid) {
                entry = &neighbors[i];
                memset(entry, 0, sizeof(*entry));
                entry->valid = 1;
                entry->port_id = port_id;
                entry->is_ipv6 = is_ipv6;
                entry->ipv4 = ipv4;
                if (is_ipv6)
                    entry->ipv6 = *ipv6;
                break;
            }
        }
    }

    if (!entry)
        return -1;

    rte_ether_addr_copy(mac, &entry->mac);
    return 0;
}

static int ipv6_is_unspecified(const struct rte_ipv6_addr *addr)
{
    static const struct rte_ipv6_addr zero = { .a = { 0 } };
    return rte_ipv6_addr_eq(addr, &zero);
}

static int ipv6_is_multicast(const struct rte_ipv6_addr *addr)
{
    return addr->a[0] == 0xff;
}

static void set_all_nodes_multicast(struct rte_ipv6_addr *addr)
{
    static const struct rte_ipv6_addr all_nodes =
        RTE_IPV6(0xff02, 0, 0, 0, 0, 0, 0, 1);
    *addr = all_nodes;
}

static void ipv6_multicast_mac(const struct rte_ipv6_addr *addr,
                               struct rte_ether_addr *mac)
{
    mac->addr_bytes[0] = 0x33;
    mac->addr_bytes[1] = 0x33;
    mac->addr_bytes[2] = addr->a[12];
    mac->addr_bytes[3] = addr->a[13];
    mac->addr_bytes[4] = addr->a[14];
    mac->addr_bytes[5] = addr->a[15];
}

int neighbor_init(void)
{
    memset(neighbors, 0, sizeof(neighbors));
    memset(local_ports, 0, sizeof(local_ports));

    unsigned count = rte_eth_dev_count_avail();
    if (count > MAX_NEIGHBOR_PORTS)
        count = MAX_NEIGHBOR_PORTS;

    for (unsigned p = 0; p < count; p++) {
        struct local_port *lp = &local_ports[p];
        lp->valid = 1;
        rte_eth_macaddr_get((uint16_t)p, &lp->mac);

        if (p == 0) {
            lp->ipv4 = rte_cpu_to_be_32(RTE_IPV4(192, 168, 1, 1));
            lp->ipv6 = (struct rte_ipv6_addr)RTE_IPV6(0x2001, 0xdb8, 1, 0, 0, 0, 0, 1);
        } else {
            lp->ipv4 = rte_cpu_to_be_32(RTE_IPV4(10, 0, 0, 1));
            lp->ipv6 = (struct rte_ipv6_addr)RTE_IPV6(0x2001, 0xdb8, 2, 0, 0, 0, 0, 1);
        }
    }

    return count ? 0 : -1;
}

void neighbor_free(void)
{
    memset(neighbors, 0, sizeof(neighbors));
}

static int handle_arp(struct rte_mbuf *m, uint16_t port_id, uint16_t tx_queue)
{
    if (rte_pktmbuf_data_len(m) < sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr))
        return -1;
    if (port_id >= MAX_NEIGHBOR_PORTS || !local_ports[port_id].valid)
        return 0;

    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    struct rte_arp_hdr *arp = (struct rte_arp_hdr *)(eth + 1);

    if (rte_be_to_cpu_16(arp->arp_hardware) != RTE_ARP_HRD_ETHER ||
        rte_be_to_cpu_16(arp->arp_protocol) != RTE_ETHER_TYPE_IPV4 ||
        arp->arp_hlen != RTE_ETHER_ADDR_LEN || arp->arp_plen != 4)
        return -1;

    if (arp->arp_data.arp_sip != 0)
        if (learn_neighbor(port_id, 0, arp->arp_data.arp_sip, NULL,
                           &arp->arp_data.arp_sha) == 0)
            arp_learned++;

    if (arp->arp_opcode != rte_cpu_to_be_16(RTE_ARP_OP_REQUEST) ||
        arp->arp_data.arp_tip != local_ports[port_id].ipv4)
        return 1;

    struct rte_ether_addr requester = arp->arp_data.arp_sha;
    uint32_t requester_ip = arp->arp_data.arp_sip;

    rte_ether_addr_copy(&eth->s_addr, &eth->d_addr);
    rte_ether_addr_copy(&local_ports[port_id].mac, &eth->s_addr);

    arp->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REPLY);
    rte_ether_addr_copy(&arp->arp_data.arp_sha, &arp->arp_data.arp_tha);
    rte_ether_addr_copy(&local_ports[port_id].mac, &arp->arp_data.arp_sha);
    arp->arp_data.arp_tip = requester_ip;
    arp->arp_data.arp_sip = local_ports[port_id].ipv4;

    if (rte_eth_tx_burst(port_id, tx_queue, &m, 1) == 1) {
        arp_replies++;
        return 1;
    }

    rte_pktmbuf_free(m);
    return 1;
}

struct icmp6_nd_hdr {
    uint8_t type;
    uint8_t code;
    rte_be16_t checksum;
    rte_be32_t flags;
    struct rte_ipv6_addr target;
} __attribute__((packed));

struct icmp6_nd_opt_lladdr {
    uint8_t type;
    uint8_t length;
    struct rte_ether_addr mac;
} __attribute__((packed));

static int handle_nd(struct rte_mbuf *m, uint16_t port_id, uint16_t tx_queue)
{
    if (port_id >= MAX_NEIGHBOR_PORTS || !local_ports[port_id].valid)
        return 0;

    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    size_t l3_offset = sizeof(struct rte_ether_hdr);
    if (rte_pktmbuf_data_len(m) < l3_offset + sizeof(struct rte_ipv6_hdr) + sizeof(struct icmp6_nd_hdr))
        return -1;

    struct rte_ipv6_hdr *ip6 = (struct rte_ipv6_hdr *)((char *)eth + l3_offset);
    if (ip6->proto != IPPROTO_ICMPV6 || ip6->hop_limits != 255)
        return 0;

    size_t icmp_len = rte_be_to_cpu_16(ip6->payload_len);
    if (icmp_len < sizeof(struct icmp6_nd_hdr) ||
        rte_pktmbuf_data_len(m) < l3_offset + sizeof(*ip6) + icmp_len)
        return -1;

    struct icmp6_nd_hdr *nd = (struct icmp6_nd_hdr *)((char *)ip6 + sizeof(*ip6));
    struct rte_ether_addr src_mac = eth->s_addr;

    if (!ipv6_is_unspecified(&ip6->src_addr)) {
        if (learn_neighbor(port_id, 1, 0, &ip6->src_addr, &src_mac) == 0)
            nd_learned++;
    }

    if (nd->type == ICMP6_NEIGHBOR_ADVERTISEMENT) {
        if (icmp_len >= sizeof(*nd) + sizeof(struct icmp6_nd_opt_lladdr)) {
            struct icmp6_nd_opt_lladdr *opt =
                (struct icmp6_nd_opt_lladdr *)((char *)nd + sizeof(*nd));
            if (opt->type == ICMP6_OPT_TARGET_LLADDR && opt->length == 1)
                if (learn_neighbor(port_id, 1, 0, &nd->target, &opt->mac) == 0)
                    nd_learned++;
        }
        return 1;
    }

    if (nd->type != ICMP6_NEIGHBOR_SOLICITATION ||
        !rte_ipv6_addr_eq(&nd->target, &local_ports[port_id].ipv6))
        return 1;

    struct rte_ipv6_addr original_src = ip6->src_addr;
    int source_unspecified = ipv6_is_unspecified(&original_src);

    if (source_unspecified) {
        set_all_nodes_multicast(&ip6->dst_addr);
        ipv6_multicast_mac(&ip6->dst_addr, &eth->d_addr);
    } else {
        ip6->dst_addr = original_src;
        eth->d_addr = src_mac;
    }
    ip6->src_addr = local_ports[port_id].ipv6;
    ip6->hop_limits = 255;

    nd->type = ICMP6_NEIGHBOR_ADVERTISEMENT;
    nd->code = 0;
    nd->flags = rte_cpu_to_be_32(source_unspecified ? 0x20000000U : 0x60000000U);
    nd->target = local_ports[port_id].ipv6;

    struct icmp6_nd_opt_lladdr *opt =
        (struct icmp6_nd_opt_lladdr *)((char *)nd + sizeof(*nd));
    opt->type = ICMP6_OPT_TARGET_LLADDR;
    opt->length = 1;
    opt->mac = local_ports[port_id].mac;

    size_t new_icmp_len = sizeof(*nd) + sizeof(*opt);
    ip6->payload_len = rte_cpu_to_be_16((uint16_t)new_icmp_len);
    nd->checksum = 0;
    nd->checksum = rte_cpu_to_be_16(
        icmp6_checksum(ip6, (const uint8_t *)nd, new_icmp_len));

    if (rte_eth_tx_burst(port_id, tx_queue, &m, 1) == 1) {
        nd_replies++;
        return 1;
    }

    rte_pktmbuf_free(m);
    return 1;
}

int neighbor_handle_control(struct rte_mbuf *m, uint16_t ingress_port,
                            uint16_t tx_queue)
{
    if (rte_pktmbuf_data_len(m) < sizeof(struct rte_ether_hdr))
        return -1;

    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    uint16_t ether_type = rte_be_to_cpu_16(eth->ether_type);

    if (ether_type == RTE_ETHER_TYPE_ARP)
        return handle_arp(m, ingress_port, tx_queue);

    if (ether_type == RTE_ETHER_TYPE_IPV6)
        return handle_nd(m, ingress_port, tx_queue);

    return 0;
}

int neighbor_resolve(const struct packet_info *info, uint16_t egress_port,
                     struct rte_ether_addr *dst_mac)
{
    struct neighbor_entry *entry = NULL;

    if (info->ip_version == 4)
        entry = find_neighbor(egress_port, 0, info->ip4->dst_addr, NULL);
    else if (info->ip_version == 6)
        entry = find_neighbor(egress_port, 1, 0, &info->ip6->dst_addr);
    else
        return -1;

    if (!entry) {
        resolve_misses++;
        return -1;
    }

    rte_ether_addr_copy(&entry->mac, dst_mac);
    resolve_hits++;
    return 0;
}

int neighbor_rewrite_l2(struct rte_mbuf *m, uint16_t egress_port,
                        const struct rte_ether_addr *dst_mac)
{
    if (egress_port >= MAX_NEIGHBOR_PORTS || !local_ports[egress_port].valid)
        return -1;

    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    rte_ether_addr_copy(dst_mac, &eth->d_addr);
    rte_ether_addr_copy(&local_ports[egress_port].mac, &eth->s_addr);
    return 0;
}

void neighbor_print_stats(void)
{
    printf("Neighbor: ARP learned=%lu replies=%lu, ND learned=%lu replies=%lu, "
           "resolve_hit=%lu resolve_miss=%lu\n",
           arp_learned, arp_replies, nd_learned, nd_replies,
           resolve_hits, resolve_misses);
}
