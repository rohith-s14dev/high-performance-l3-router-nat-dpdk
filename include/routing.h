#ifndef ROUTER_ROUTING_H
#define ROUTER_ROUTING_H

#include "parser.h"

int routing_init(int socket_id);
int routing_add_ipv4(const char *prefix, uint8_t depth, uint16_t port);
int routing_add_ipv6(const char *prefix, uint8_t depth, uint16_t port);
int routing_lookup(const struct packet_info *info, uint16_t *port);
void routing_print_summary(void);
void routing_free(void);

#endif
