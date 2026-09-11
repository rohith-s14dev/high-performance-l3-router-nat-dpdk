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

The current Phase 7 implementation is intentionally a learning/portfolio implementation. It does not yet include full neighbor-cache aging, active ARP/ND solicitation/retransmission timers, queued packets awaiting resolution, or complete routing next-hop configuration.

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
DPDK router Phase 7: RSS + multi-queue + ARP/IPv6 ND
Active ports: 2, queues per port: 2
Neighbor cache learns from ARP/ND and performs L2 rewrite.
lcore 1 -> port 0 RX queue 0 -> TX queue 0
lcore 2 -> port 1 RX queue 0 -> TX queue 0
lcore 3 -> port 0 RX queue 1 -> TX queue 1
lcore 4 -> port 1 RX queue 1 -> TX queue 1
```

> Hardware/NIC binding, hugepages, queue limits, and RSS capabilities depend on the test environment and DPDK version. The implementation targets modern DPDK APIs; DPDK 24.11 documentation was used as the API reference. DPDK available-port IDs can be non-contiguous, so production code should use `RTE_ETH_FOREACH_DEV` when generalized beyond this two-port lab topology.

## Phase 7 Lab Addresses

When two ports are available, the demo assigns these logical router addresses:

| Port | IPv4 | IPv6 |
|---|---|---|
| 0 | `192.168.1.1` | `2001:db8:1::1/64` |
| 1 | `10.0.0.1` | `2001:db8:2::1/64` |

Routes use port 0 for `192.168.1.0/24` and `2001:db8:1::/64`, and port 1 for `10.0.0.0/8` and `2001:db8:2::/64`. The default route also uses port 1 in the two-port lab topology.

Neighbor entries are learned dynamically from ARP/ND packets. A packet is forwarded only after its destination neighbor has been learned.
