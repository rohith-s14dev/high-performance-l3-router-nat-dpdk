#ifndef ROUTER_NAT_H
#define ROUTER_NAT_H

#include "parser.h"

#define NAT_TABLE_SIZE 65536
#define NAT_MAX_ENTRIES 65536

struct nat_config {
    uint32_t public_ip;
    uint16_t public_port_min;
    uint16_t public_port_max;
};

struct nat_stats {
    uint64_t translations;
    uint64_t existing_flows;
    uint64_t new_flows;
    uint64_t misses;
    uint64_t drops;
};

int nat_init(int socket_id, const struct nat_config *config);
int nat_translate_outbound(struct packet_info *info);
void nat_print_stats(void);
void nat_free(void);

#endif
