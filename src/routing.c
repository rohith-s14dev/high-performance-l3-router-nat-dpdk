#include "routing.h"

#include <arpa/inet.h>
#include <rte_lpm.h>
#include <rte_lpm6.h>

#define MAX_IPV4_RULES 4096
#define MAX_IPV6_RULES 4096
#define MAX_TBL8S 256

static struct rte_lpm *lpm4;
static struct rte_lpm6 *lpm6;

int routing_init(int socket_id)
{
    struct rte_lpm_config cfg4 = {
        .max_rules = MAX_IPV4_RULES,
        .number_tbl8s = MAX_TBL8S,
        .flags = 0
    };

    struct rte_lpm6_config cfg6 = {
        .max_rules = MAX_IPV6_RULES,
        .number_tbl8s = MAX_TBL8S,
        .flags = 0
    };

    lpm4 = rte_lpm_create("router_lpm4", socket_id, &cfg4);
    if (lpm4 == NULL)
        return -1;

    lpm6 = rte_lpm6_create("router_lpm6", socket_id, &cfg6);
    if (lpm6 == NULL) {
        rte_lpm_free(lpm4);
        lpm4 = NULL;
        return -1;
    }

    return 0;
}

int routing_add_ipv4(const char *prefix, uint8_t depth, uint16_t port)
{
    struct in_addr addr;

    if (lpm4 == NULL || prefix == NULL || depth > 32 ||
        port >= RTE_MAX_ETHPORTS)
        return -1;

    if (inet_pton(AF_INET, prefix, &addr) != 1)
        return -1;

    return rte_lpm_add(lpm4, rte_be_to_cpu_32(addr.s_addr),
                       depth, port);
}

int routing_add_ipv6(const char *prefix, uint8_t depth, uint16_t port)
{
    struct in6_addr addr6;
    struct rte_ipv6_addr rte_addr6;

    if (lpm6 == NULL || prefix == NULL || depth > 128 ||
        port >= RTE_MAX_ETHPORTS)
        return -1;

    if (inet_pton(AF_INET6, prefix, &addr6) != 1)
        return -1;

    memcpy(&rte_addr6, &addr6, sizeof(rte_addr6));

    return rte_lpm6_add(lpm6, &rte_addr6, depth, port);
}

int routing_lookup(const struct packet_info *info, uint16_t *port)
{
    uint32_t next_hop;

    if (info == NULL || port == NULL)
        return -1;

    if (info->ip_version == 4) {
        if (lpm4 == NULL)
            return -1;

        if (rte_lpm_lookup(lpm4,
                           rte_be_to_cpu_32(info->ip4->dst_addr),
                           &next_hop) != 0)
            return -1;
    } else if (info->ip_version == 6) {
        if (lpm6 == NULL)
            return -1;

        if (rte_lpm6_lookup(lpm6, &info->ip6->dst_addr,
                            &next_hop) != 0)
            return -1;
    } else {
        return -1;
    }

    if (next_hop >= RTE_MAX_ETHPORTS)
        return -1;

    *port = (uint16_t)next_hop;
    return 0;
}

void routing_print_summary(void)
{
    printf("Routing: IPv4/IPv6 Longest Prefix Match enabled\n");
}

void routing_free(void)
{
    if (lpm4 != NULL)
        rte_lpm_free(lpm4);

    if (lpm6 != NULL)
        rte_lpm6_free(lpm6);

    lpm4 = NULL;
    lpm6 = NULL;
}
