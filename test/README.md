# Phase 1 Testing

Phase 1 is a basic DPDK RX/TX forwarding skeleton.

## Functional validation

1. Build the application with `make`.
2. Run it in a Linux environment with DPDK installed and an available DPDK-compatible port.
3. Generate controlled traffic toward the configured port.
4. Confirm packets are received and transmitted by observing NIC statistics and packet captures where supported.
5. Use GDB for debugging and `tcpdump`/Wireshark on a suitable interface or traffic endpoint.

The exact NIC binding and EAL parameters depend on the host and DPDK version, so they are intentionally not hard-coded into the project yet.
