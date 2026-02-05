# Networking Stack Guide

This document captures the networking work that landed in the v0.8 beta 1-3 milestones. It covers the new driver stack, socket interface, shell commands, and diagnostic tooling so you can exercise Ethernet and ICMP from a development build.

## Boot & Device Bring-Up

- The kernel now calls `net_init()` during boot to reset the core tables, start the raw socket subsystem, and announce readiness through the log buffer.
- `e1000_driver_init()` probes the PCI bus for any Intel 8254x controller, enables MMIO + bus mastering, and exposes the NIC as `net0` in the device manager.
- On a successful probe the driver registers a single logical interface (`eth0`) with the network core, wiring up transmit and poll callbacks.
- The shell banner still reports v0.8 b2, but the stack survives warm boots because descriptor rings and buffers are reinitialized on every bring-up.

## Packet Flow Architecture

1. Incoming frames reach the e1000 RX ring and are pushed into the network core through `net_receive_frame()`.
2. `ethernet_process_frame()` inspects the ethertype and dispatches to:
   - `arp_receive()` for address resolution traffic.
   - `ipv4_receive()` for layer 3 payloads.
3. IPv4 currently understands ICMP (type 1) and forwards packets to `icmp_receive()`.
4. Echo requests provoke an immediate reply; echo responses are recorded for later retrieval by the shell ping helper.
5. Outbound traffic follows the reverse path: ICMP → IPv4 (where ARP resolution is enforced) → Ethernet → e1000 transmit ring.

The ARP layer maintains an eight-entry cache per device. It opportunistically updates on any request or reply and falls back to broadcast resolution when a mapping is missing.

## Raw Socket Interface

- `net_open()` returns a lightweight handle that is permanently tied to the first registered interface.
- Each socket buffers up to eight frames of at most 1600 bytes. Overruns drop the oldest frame to keep the queue alive.
- `net_send()` pushes raw Ethernet payloads through the device transmit callback.
- `net_recv()` is non-blocking: it copies the next buffered frame or returns `0` if none are available. Oversized buffers are rejected so callers can retry with a larger destination.
- `net_close()` clears ownership and releases the queue.

Use this API for quick experiments in kernel modules without having to talk directly to the driver rings.

## Shell Tooling (`net` Command)

| Subcommand | Description |
|------------|-------------|
| `net help` | Prints the subcommand summary. |
| `net list` | Lists registered interfaces with their MAC addresses. |
| `net poll [n\|watch]` | Manually services RX/TX queues. `watch` loops (≤64 iterations) until no work remains. |
| `net arp <ipv4>` | Queries or triggers ARP resolution against the default interface. |
| `net ping <ipv4> [count]` | Sends ICMP echo requests, collecting min/avg/max round-trip statistics. |
| `net ip [ipv4]` | Shows or overrides the local IPv4 address used for outbound ARP and IP headers. |

### Example Session

1. `net list` — verify that `eth0` is present and note the MAC address logged during boot.
2. `net ip 10.0.2.15` — set a host address (required only if you want ARP replies to contain a non-zero sender IP).
3. `net arp 10.0.2.2` — warm the ARP cache for the virtual gateway; follow with `net poll watch` to process the reply.
4. `net ping 10.0.2.2 4` — send four echo requests; the shell reports packet loss plus RTT stats.

Because interrupts are not yet wired into the NIC, always run `net poll` (or use the `watch` loop) around traffic bursts to drain rings and service ARP/ICMP callbacks.

## Logging & Diagnostics

- The driver and protocol layers tag messages under the `net` module. Use `logs net` from the shell to dump `/System/Logs/net.log` after calling `klog_enable_proc_sink()` once per boot.
- `kdlg` continues to show the rolling klog buffer if you need a deeper dive into link state or transmit warnings (`tx ring full`, `frame rejected`, etc.).
- PCI probing also surfaces failures in `logs kernel` if the hardware is absent or lacks a usable BAR.

## Services & Background Work

- A stub network service (`netd`) now registers under `SYSTEM_SERVICE_NETD`. It currently yields in a loop, providing a hook for future socket routing or DHCP logic without blocking boot.
- IPC rights are pre-provisioned so future user-space clients can exchange messages with the daemon once functionality lands.

## Limitations & Follow-Up Ideas

- e1000 is the only supported NIC, and the stack assumes a single interface slot.
- IPv4 fragmentation, UDP/TCP, and checksum validation beyond headers are not implemented.
- ARP cache entries never expire; use `net arp` to refresh stale mappings manually.
- `net poll` is required after every burst because interrupts are masked. Hooking ISR 11 to wake the scheduler would eliminate the busy-looping.
- The socket layer exposes raw frames only. Higher-level protocols should build on top once packet parsing stabilizes.

Collectively, these pieces introduce a functioning Ethernet + ICMP pipeline suitable for integration tests and further protocol work in upcoming milestones.
