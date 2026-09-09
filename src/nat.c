#include "nat.h"

#include <arpa/inet.h>
#include <rte_hash.h>
#include <rte_jhash.h>

struct nat_key {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t protocol;
};

struct nat_entry {
    struct nat_key key;
    uint32_t translated_ip;
    uint16_t translated_port;
};

static struct rte_hash *nat_hash;
static struct nat_entry *nat_entries;
static struct nat_config nat_cfg;
static struct nat_stats stats;

static uint32_t key_hash(const struct nat_key *key)
{
    return rte_jhash(key, sizeof(*key), 0);
}

int nat_init(int socket_id, const struct nat_config *config)
{
    struct rte_hash_parameters params = {
        .name = "router_nat_hash",
        .entries = NAT_MAX_ENTRIES,
        .key_len = sizeof(struct nat_key),
        .hash_func = rte_jhash,
        .hash_func_init_val = 0,
        .socket_id = socket_id
    };

    if (config == NULL)
        return -1;

    nat_cfg = *config;

    nat_hash = rte_hash_create(&params);
    if (nat_hash == NULL)
        return -1;

    nat_entries = rte_zmalloc_socket("router_nat_entries",
                                     sizeof(*nat_entries) * NAT_MAX_ENTRIES,
                                     RTE_CACHE_LINE_SIZE,
                                     socket_id);
    if (nat_entries == NULL) {
        rte_hash_free(nat_hash);
        nat_hash = NULL;
        return -1;
    }

    memset(&stats, 0, sizeof(stats));
    return 0;
}

static uint16_t allocate_port(void)
{
    static uint32_t next_port;

    uint32_t range;

    if (nat_cfg.public_port_max < nat_cfg.public_port_min)
        return 0;

    range = (uint32_t)nat_cfg.public_port_max -
            nat_cfg.public_port_min + 1;

    for (uint32_t i = 0; i < range; i++) {
        uint16_t candidate =
            (uint16_t)(nat_cfg.public_port_min +
                       ((next_port + i) % range));

        int used = 0;

        for (uint32_t j = 0; j < NAT_MAX_ENTRIES; j++) {
            if (nat_entries[j].translated_port == candidate) {
                used = 1;
                break;
            }
        }

        if (!used) {
            next_port = (candidate - nat_cfg.public_port_min + 1) % range;
            return candidate;
        }
    }

    return 0;
}

int nat_translate_outbound(struct packet_info *info)
{
    struct nat_key key;
    int32_t pos;
    struct nat_entry *entry;

    if (info == NULL || info->ip_version != 4)
        return -1;

    if (info->protocol != IPPROTO_TCP &&
        info->protocol != IPPROTO_UDP)
        return -1;

    memset(&key, 0, sizeof(key));
    key.src_ip = info->ip4->src_addr;
    key.dst_ip = info->ip4->dst_addr;
    key.src_port = rte_cpu_to_be_16(info->src_port);
    key.dst_port = rte_cpu_to_be_16(info->dst_port);
    key.protocol = info->protocol;

    pos = rte_hash_lookup(nat_hash, &key);

    if (pos >= 0) {
        entry = &nat_entries[pos];
        info->ip4->src_addr = entry->translated_ip;
        if (info->protocol == IPPROTO_TCP)
            info->tcp->src_port = rte_cpu_to_be_16(entry->translated_port);
        else
            info->udp->src_port = rte_cpu_to_be_16(entry->translated_port);

        stats.existing_flows++;
        stats.translations++;
        return 0;
    }

    uint16_t translated_port = allocate_port();
    if (translated_port == 0) {
        stats.drops++;
        return -1;
    }

    pos = rte_hash_add_key(nat_hash, &key);
    if (pos < 0) {
        stats.drops++;
        return -1;
    }

    entry = &nat_entries[pos];
    entry->key = key;
    entry->translated_ip = nat_cfg.public_ip;
    entry->translated_port = translated_port;

    info->ip4->src_addr = nat_cfg.public_ip;

    if (info->protocol == IPPROTO_TCP)
        info->tcp->src_port = rte_cpu_to_be_16(translated_port);
    else
        info->udp->src_port = rte_cpu_to_be_16(translated_port);

    stats.new_flows++;
    stats.translations++;
    return 0;
}

void nat_print_stats(void)
{
    printf("NAT translations : %lu\n", stats.translations);
    printf("New flows        : %lu\n", stats.new_flows);
    printf("Existing flows   : %lu\n", stats.existing_flows);
    printf("NAT misses       : %lu\n", stats.misses);
    printf("NAT drops        : %lu\n", stats.drops);
}

void nat_free(void)
{
    if (nat_entries != NULL)
        rte_free(nat_entries);

    if (nat_hash != NULL)
        rte_hash_free(nat_hash);

    nat_entries = NULL;
    nat_hash = NULL;
}
