#include "forward.h"

#include <rte_byteorder.h>
#include <rte_ip.h>
#include <rte_ip6.h>

static uint16_t checksum_fold(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xffffU) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t ipv4_checksum(const struct rte_ipv4_hdr *ip4)
{
    uint32_t sum = 0;
    const uint8_t *p = (const uint8_t *)ip4;
    unsigned len = (ip4->version_ihl & 0x0fU) * 4U;

    for (unsigned i = 0; i < len; i += 2)
        sum += ((uint32_t)p[i] << 8) | p[i + 1];

    return checksum_fold(sum);
}

static int ipv4_is_multicast(uint32_t addr)
{
    uint32_t host = rte_be_to_cpu_32(addr);
    return host >= 0xe0000000U && host <= 0xefffffffU;
}

static int prepare_ipv4(struct rte_mbuf *m, struct packet_info *info)
{
    struct rte_ipv4_hdr *ip4 = info->ip4;
    uint16_t total_len;
    uint8_t ihl;

    if (rte_pktmbuf_data_len(m) < info->l3_offset + sizeof(*ip4))
        return -1;

    ihl = (uint8_t)((ip4->version_ihl & 0x0fU) * 4U);
    if ((ip4->version_ihl >> 4) != 4 || ihl < sizeof(*ip4) ||
        rte_pktmbuf_data_len(m) < info->l3_offset + ihl)
        return -1;

    total_len = rte_be_to_cpu_16(ip4->total_length);
    if (total_len < ihl ||
        total_len > rte_pktmbuf_data_len(m) - info->l3_offset)
        return -1;

    if (!ipv4_is_multicast(ip4->dst_addr)) {
        if (ip4->time_to_live <= 1)
            return -1;
        ip4->time_to_live--;
    }

    ip4->hdr_checksum = 0;
    ip4->hdr_checksum = rte_cpu_to_be_16(ipv4_checksum(ip4));
    return 0;
}

static int prepare_ipv6(struct rte_mbuf *m, struct packet_info *info)
{
    struct rte_ipv6_hdr *ip6 = info->ip6;
    uint16_t payload_len;

    if (rte_pktmbuf_data_len(m) < info->l3_offset + sizeof(*ip6))
        return -1;

    if ((rte_be_to_cpu_32(ip6->vtc_flow) >> 28) != 6)
        return -1;

    payload_len = rte_be_to_cpu_16(ip6->payload_len);
    if (payload_len > rte_pktmbuf_data_len(m) -
                       info->l3_offset - sizeof(*ip6))
        return -1;

    if (ip6->hop_limits <= 1)
        return -1;

    ip6->hop_limits--;
    return 0;
}

int forward_prepare(struct rte_mbuf *m, struct packet_info *info)
{
    if (m == NULL || info == NULL)
        return -1;

    if (info->ip_version == 4)
        return prepare_ipv4(m, info);

    if (info->ip_version == 6)
        return prepare_ipv6(m, info);

    return -1;
}
