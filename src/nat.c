#include "nat.h"

#include <string.h>
#include <netinet/in.h>

#include <rte_byteorder.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_spinlock.h>
#include <rte_cycles.h>
#include <rte_malloc.h>

struct nat_key {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t protocol;
};

struct nat_reverse_key {
    uint32_t public_ip;
    uint32_t remote_ip;
    uint16_t public_port;
    uint16_t remote_port;
    uint8_t protocol;
};

struct nat_entry {
    int valid;
    struct nat_key key;
    struct nat_reverse_key reverse_key;
    uint32_t translated_ip;
    uint16_t translated_port;
    uint64_t last_seen_cycles;
};

static struct rte_hash *nat_hash;
static struct rte_hash *nat_reverse_hash;
static struct nat_entry *nat_entries;
static struct nat_config nat_cfg;
static struct nat_stats stats;
static rte_spinlock_t nat_lock;

static uint16_t checksum_fold(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xffffU) + (sum >> 16);

    return (uint16_t)~sum;
}

static void checksum_add_bytes(uint32_t *sum, const uint8_t *data, uint32_t len)
{
    while (len >= 2) {
        *sum += ((uint32_t)data[0] << 8) | data[1];
        data += 2;
        len -= 2;
    }

    if (len)
        *sum += (uint32_t)data[0] << 8;
}

static uint16_t l4_checksum_ipv4(struct packet_info *info)
{
    struct rte_ipv4_hdr *ip4 = info->ip4;
    uint8_t *l4;
    uint16_t total_len;
    uint8_t ihl;
    uint16_t l4_len;
    uint16_t *checksum_field;
    uint32_t sum = 0;

    ihl = (uint8_t)((ip4->version_ihl & 0x0fU) * 4U);
    total_len = rte_be_to_cpu_16(ip4->total_length);

    if (total_len < ihl)
        return 0;

    l4_len = (uint16_t)(total_len - ihl);
    l4 = (uint8_t *)ip4 + ihl;

    sum += (uint32_t)(ip4->src_addr >> 16);
    sum += (uint32_t)(ip4->src_addr & 0xffffU);
    sum += (uint32_t)(ip4->dst_addr >> 16);
    sum += (uint32_t)(ip4->dst_addr & 0xffffU);
    sum += (uint32_t)ip4->next_proto_id;
    sum += l4_len;

    checksum_field = NULL;
    if (info->protocol == IPPROTO_TCP) {
        if (l4_len < sizeof(struct rte_tcp_hdr))
            return 0;
        checksum_field = &info->tcp->cksum;
    } else if (info->protocol == IPPROTO_UDP) {
        if (l4_len < sizeof(struct rte_udp_hdr))
            return 0;
        checksum_field = &info->udp->dgram_cksum;
    } else {
        return 0;
    }

    *checksum_field = 0;
    checksum_add_bytes(&sum, l4, l4_len);

    uint16_t checksum = checksum_fold(sum);

    /* UDP checksum 0 means "no checksum"; after NAT we deliberately
     * generate a valid checksum. RFC 768 reserves zero as the disabled
     * value, so encode a computed zero as 0xffff. */
    if (info->protocol == IPPROTO_UDP && checksum == 0)
        checksum = 0xffff;

    return checksum;
}

static int refresh_l4_checksum(struct packet_info *info)
{
    uint16_t checksum;

    checksum = l4_checksum_ipv4(info);
    if (checksum == 0)
        return -1;

    if (info->protocol == IPPROTO_TCP)
        info->tcp->cksum = rte_cpu_to_be_16(checksum);
    else
        info->udp->dgram_cksum = rte_cpu_to_be_16(checksum);

    stats.checksum_updates++;
    return 0;
}

static void build_reverse_key(const struct nat_entry *entry,
                              struct nat_reverse_key *key)
{
    *key = entry->reverse_key;
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
            if (nat_entries[j].valid &&
                nat_entries[j].translated_port == candidate) {
                used = 1;
                break;
            }
        }

        if (!used) {
            next_port =
                (candidate - nat_cfg.public_port_min + 1) % range;
            return candidate;
        }
    }

    return 0;
}

int nat_init(int socket_id, const struct nat_config *config)
{
    struct rte_hash_parameters params = {
        .entries = NAT_MAX_ENTRIES,
        .key_len = sizeof(struct nat_key),
        .hash_func = rte_jhash,
        .hash_func_init_val = 0,
        .socket_id = socket_id,
        .extra_flag = RTE_HASH_EXTRA_FLAGS_MULTI_WRITER_ADD
    };
    struct rte_hash_parameters reverse_params = {
        .entries = NAT_MAX_ENTRIES,
        .key_len = sizeof(struct nat_reverse_key),
        .hash_func = rte_jhash,
        .hash_func_init_val = 0,
        .socket_id = socket_id,
        .extra_flag = RTE_HASH_EXTRA_FLAGS_MULTI_WRITER_ADD
    };

    if (config == NULL)
        return -1;

    nat_cfg = *config;
    if (nat_cfg.flow_timeout_sec == 0)
        nat_cfg.flow_timeout_sec = NAT_DEFAULT_TIMEOUT_SEC;

    params.name = "router_nat_hash";
    reverse_params.name = "router_nat_reverse_hash";

    nat_hash = rte_hash_create(&params);
    if (nat_hash == NULL)
        return -1;

    nat_reverse_hash = rte_hash_create(&reverse_params);
    if (nat_reverse_hash == NULL) {
        rte_hash_free(nat_hash);
        nat_hash = NULL;
        return -1;
    }

    nat_entries = rte_zmalloc_socket("router_nat_entries",
                                     sizeof(*nat_entries) * NAT_MAX_ENTRIES,
                                     RTE_CACHE_LINE_SIZE,
                                     socket_id);
    if (nat_entries == NULL) {
        rte_hash_free(nat_reverse_hash);
        rte_hash_free(nat_hash);
        nat_reverse_hash = NULL;
        nat_hash = NULL;
        return -1;
    }

    memset(&stats, 0, sizeof(stats));
    rte_spinlock_init(&nat_lock);
    return 0;
}

int nat_is_public_destination(const struct packet_info *info)
{
    if (info == NULL || info->ip_version != 4 || info->ip4 == NULL)
        return 0;

    return info->ip4->dst_addr == nat_cfg.public_ip;
}

int nat_translate_outbound(struct packet_info *info)
{
    struct nat_key key;
    int32_t pos;
    struct nat_entry *entry;
    uint16_t translated_port;

    if (info == NULL || info->ip_version != 4 ||
        (info->protocol != IPPROTO_TCP && info->protocol != IPPROTO_UDP))
        return -1;

    memset(&key, 0, sizeof(key));
    key.src_ip = info->ip4->src_addr;
    key.dst_ip = info->ip4->dst_addr;
    key.src_port = rte_cpu_to_be_16(info->src_port);
    key.dst_port = rte_cpu_to_be_16(info->dst_port);
    key.protocol = info->protocol;

    rte_spinlock_lock(&nat_lock);

    pos = rte_hash_lookup(nat_hash, &key);
    if (pos >= 0) {
        entry = &nat_entries[pos];
        if (!entry->valid) {
            rte_spinlock_unlock(&nat_lock);
            return -1;
        }

        entry->last_seen_cycles = rte_get_timer_cycles();
        info->ip4->src_addr = entry->translated_ip;

        if (info->protocol == IPPROTO_TCP)
            info->tcp->src_port = rte_cpu_to_be_16(entry->translated_port);
        else
            info->udp->src_port = rte_cpu_to_be_16(entry->translated_port);

        if (refresh_l4_checksum(info) != 0) {
            stats.drops++;
            rte_spinlock_unlock(&nat_lock);
            return -1;
        }

        stats.existing_flows++;
        stats.translations++;
        rte_spinlock_unlock(&nat_lock);
        return 0;
    }

    translated_port = allocate_port();
    if (translated_port == 0) {
        stats.drops++;
        rte_spinlock_unlock(&nat_lock);
        return -1;
    }

    entry = NULL;
    pos = rte_hash_add_key(nat_hash, &key);
    if (pos < 0) {
        stats.drops++;
        rte_spinlock_unlock(&nat_lock);
        return -1;
    }

    entry = &nat_entries[pos];
    memset(entry, 0, sizeof(*entry));
    entry->valid = 1;
    entry->key = key;
    entry->translated_ip = nat_cfg.public_ip;
    entry->translated_port = translated_port;

    entry->reverse_key.public_ip = nat_cfg.public_ip;
    entry->reverse_key.remote_ip = key.dst_ip;
    entry->reverse_key.public_port =
        rte_cpu_to_be_16(translated_port);
    entry->reverse_key.remote_port = key.dst_port;
    entry->reverse_key.protocol = key.protocol;
    entry->last_seen_cycles = rte_get_timer_cycles();

    if (rte_hash_add_key_data(nat_reverse_hash, &entry->reverse_key,
                              entry) < 0) {
        rte_hash_del_key(nat_hash, &key);
        memset(entry, 0, sizeof(*entry));
        stats.drops++;
        rte_spinlock_unlock(&nat_lock);
        return -1;
    }

    info->ip4->src_addr = entry->translated_ip;

    if (info->protocol == IPPROTO_TCP)
        info->tcp->src_port = rte_cpu_to_be_16(translated_port);
    else
        info->udp->src_port = rte_cpu_to_be_16(translated_port);

    if (refresh_l4_checksum(info) != 0) {
        rte_hash_del_key(nat_reverse_hash, &entry->reverse_key);
        rte_hash_del_key(nat_hash, &key);
        memset(entry, 0, sizeof(*entry));
        stats.drops++;
        rte_spinlock_unlock(&nat_lock);
        return -1;
    }

    stats.new_flows++;
    stats.translations++;
    rte_spinlock_unlock(&nat_lock);
    return 0;
}

int nat_translate_inbound(struct packet_info *info)
{
    struct nat_reverse_key key;
    struct nat_entry *entry = NULL;
    void *data = NULL;

    if (info == NULL || info->ip_version != 4 ||
        (info->protocol != IPPROTO_TCP && info->protocol != IPPROTO_UDP))
        return -1;

    memset(&key, 0, sizeof(key));
    key.public_ip = info->ip4->dst_addr;
    key.remote_ip = info->ip4->src_addr;
    key.public_port = rte_cpu_to_be_16(info->dst_port);
    key.remote_port = rte_cpu_to_be_16(info->src_port);
    key.protocol = info->protocol;

    rte_spinlock_lock(&nat_lock);

    if (rte_hash_lookup_data(nat_reverse_hash, &key, &data) < 0 ||
        data == NULL) {
        stats.misses++;
        rte_spinlock_unlock(&nat_lock);
        return -1;
    }

    entry = data;
    if (!entry->valid) {
        stats.misses++;
        rte_spinlock_unlock(&nat_lock);
        return -1;
    }

    entry->last_seen_cycles = rte_get_timer_cycles();
    info->ip4->dst_addr = entry->key.src_ip;

    if (info->protocol == IPPROTO_TCP)
        info->tcp->dst_port = entry->key.src_port;
    else
        info->udp->dst_port = entry->key.src_port;

    if (refresh_l4_checksum(info) != 0) {
        stats.drops++;
        rte_spinlock_unlock(&nat_lock);
        return -1;
    }

    stats.reverse_translations++;
    stats.translations++;
    rte_spinlock_unlock(&nat_lock);
    return 0;
}

void nat_maintenance(void)
{
    uint64_t now;
    uint64_t timeout_cycles;

    rte_spinlock_lock(&nat_lock);

    now = rte_get_timer_cycles();
    timeout_cycles =
        (uint64_t)nat_cfg.flow_timeout_sec * rte_get_timer_hz();

    for (uint32_t i = 0; i < NAT_MAX_ENTRIES; i++) {
        struct nat_entry *entry = &nat_entries[i];

        if (!entry->valid)
            continue;

        if (now - entry->last_seen_cycles < timeout_cycles)
            continue;

        rte_hash_del_key(nat_reverse_hash, &entry->reverse_key);
        rte_hash_del_key(nat_hash, &entry->key);
        memset(entry, 0, sizeof(*entry));
        stats.expired++;
    }

    rte_spinlock_unlock(&nat_lock);
}

void nat_print_stats(void)
{
    printf("NAT translations : %lu\n", stats.translations);
    printf("New flows        : %lu\n", stats.new_flows);
    printf("Existing flows   : %lu\n", stats.existing_flows);
    printf("Reverse NAT      : %lu\n", stats.reverse_translations);
    printf("NAT misses       : %lu\n", stats.misses);
    printf("Expired flows    : %lu\n", stats.expired);
    printf("L4 checksum fix  : %lu\n", stats.checksum_updates);
    printf("NAT drops        : %lu\n", stats.drops);
}

void nat_free(void)
{
    if (nat_entries != NULL)
        rte_free(nat_entries);

    if (nat_reverse_hash != NULL)
        rte_hash_free(nat_reverse_hash);

    if (nat_hash != NULL)
        rte_hash_free(nat_hash);

    nat_entries = NULL;
    nat_reverse_hash = NULL;
    nat_hash = NULL;
}
