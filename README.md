# High-Performance L3 Router + NAT Engine using DPDK

A learning and portfolio project for building a high-performance user-space L3 router using DPDK.

## Project Phases

### Phase 1 — DPDK Foundation
- Initialize DPDK EAL.
- Discover Ethernet ports.
- Create an mbuf memory pool.
- Configure RX/TX queues.
- Receive and transmit packets in bursts.
- Maintain RX/TX/drop statistics.
- Handle clean shutdown.

### Phase 2 — Packet Parsing
- Ethernet and 802.1Q VLAN parsing.
- IPv4 and IPv6 parsing.
- TCP/UDP header parsing.
- Malformed-packet validation.

### Phase 3 — LPM Routing
- IPv4 `rte_lpm` lookup.
- IPv6 `rte_lpm6` lookup.
- Longest-prefix route selection.
- Configurable output-port decisions.

### Phase 4 — IPv4 NAT
- TCP/UDP source NAT.
- Flow tracking with DPDK hash tables.
- Dynamic translated-port allocation.
- NAT statistics.

### Phase 5 — Multi-Core Processing
- Burst-based worker processing.
- Per-lcore statistics.
- Clean worker shutdown.

### Phase 6 — RSS + Multi-Queue
- Multiple RX/TX queues.
- RSS for IPv4/IPv6 TCP/UDP traffic when supported by the NIC.
- One RX/TX queue pair per worker lcore on each assigned port.
- Per-lcore packet statistics.

### Phase 7 — ARP + IPv6 Neighbor Discovery
- Per-port neighbor cache.
- Learn IPv4 neighbors from ARP traffic.
- Respond to ARP requests for configured router IPv4 addresses.
- Learn IPv6 neighbors from Neighbor Solicitation/Advertisement traffic.
- Respond to IPv6 Neighbor Solicitations with Neighbor Advertisements.
- Resolve the next-hop L2 address before forwarding.
- Rewrite Ethernet source/destination MAC addresses on egress.
- Support a two-port lab topology when two DPDK ports are available, while retaining a one-port lab mode.
- Distribute worker lcores across both ports and their RX/TX queues.

### Phase 8 — L3 Forwarding Semantics
- Decrement IPv4 TTL on routed packets.
- Decrement IPv6 Hop Limit on routed packets.
- Drop packets whose TTL/Hop Limit has expired.
- Validate IPv4 header length and total packet length before forwarding.
- Recompute the IPv4 header checksum after L3 modifications.
- Preserve the IPv6 header without an IPv6 header checksum, as required by the IPv6 design.
- Keep the forwarding order explicit: NAT → route lookup → L3 forwarding update → neighbor resolution → L2 rewrite → TX.

The Phase 8 forwarding module is intentionally separated from packet parsing and neighbor handling so the dataplane stages are easier to test and extend.

### Phase 9 — Bidirectional NAT + Flow Aging
- Reverse NAT for inbound IPv4 TCP/UDP traffic.
- Separate forward and reverse DPDK hash indexes.
- Preserve the original inside source IP/port for reverse translation.
- Recompute TCP/UDP checksums after source or destination NAT changes.
- Refresh a flow's last-seen timestamp on both directions.
- Expire idle NAT flows using a configurable timeout.
- Protect the shared NAT tables with a DPDK spinlock for the multi-core lab dataplane.
- Periodically run NAT table maintenance from one worker.
- Report reverse translations, checksum updates, and expired flows.

The Phase 9 NAT implementation is still intentionally a learning/portfolio implementation. It does not yet implement TCP connection-state tracking, FIN/RST-aware teardown, protocol-specific UDP/TCP timeouts, ICMP NAT, hairpin NAT, or a high-performance lock-free flow allocator.


## Build
Requires a Linux system with DPDK development packages.

```bash
make clean
make
```

## Run
Example with four worker lcores:

```bash
sudo ./dpdk_router -l 0-4 -n 4
```

With two ports and four worker lcores, startup is similar to:

```text
DPDK router Phase 9: bidirectional NAT + flow aging
Active ports: 2, queues per port: 2
NAT public IP: 203.0.113.10, flow timeout: 60 seconds
lcore 1 -> port 0 RX queue 0 -> TX queue 0
lcore 2 -> port 1 RX queue 0 -> TX queue 0
lcore 3 -> port 0 RX queue 1 -> TX queue 1
lcore 4 -> port 1 RX queue 1 -> TX queue 1
```

> Hardware/NIC binding, hugepages, queue limits, and RSS capabilities depend on the test environment and DPDK version. The implementation targets modern DPDK APIs. Available port IDs can be non-contiguous; this lab topology still assumes the first two active ports are IDs 0 and 1.

## Phase 7/8 Lab Addresses

When two ports are available, the demo assigns these logical router addresses:

| Port | IPv4 | IPv6 |
|---|---|---|
| 0 | `192.168.1.1` | `2001:db8:1::1/64` |
| 1 | `10.0.0.1` | `2001:db8:2::1/64` |

Routes use port 0 for `192.168.1.0/24` and `2001:db8:1::/64`, and port 1 for `10.0.0.0/8` and `2001:db8:2::/64`. The default route also uses port 1 in the two-port lab topology.

Neighbor entries are learned dynamically from ARP/ND packets. A packet is forwarded only after its destination neighbor has been learned.
