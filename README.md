# High-Performance L3 Router + NAT Engine using DPDK

A learning and portfolio project for building a high-performance user-space L3 router using DPDK.

## Phase 1 — DPDK Foundation
This phase establishes the DPDK application foundation:
- Initialize DPDK EAL.
- Discover available Ethernet ports.
- Create an mbuf memory pool.
- Configure RX/TX queues.
- Receive packets in bursts and transmit them back.
- Maintain basic RX/TX/drop statistics.
- Handle clean shutdown on SIGINT/SIGTERM.

Later phases will add packet parsing, IPv4/IPv6 routing, NAT, multi-core processing, and testing.

## Build
Requires a Linux system with DPDK development packages.

```bash
make
```

## Run
Example:

```bash
sudo ./dpdk_router -l 0-1 -n 4
```

> Hardware/NIC binding and hugepage configuration depend on the test environment and DPDK version.
