# Fronthaul Sniffer

Standalone C++ sniffer scaffold for O-RAN fronthaul traffic. The design keeps packet capture backends separate from packet processing, so Stage 1 (loopback simulation) can evolve into Stage 2/3 without changing processing logic.

## Current Status

- Stage 1 ready: streaming sniff on loopback via:
  - `pcap` backend (libpcap live capture)
  - `socket` backend (Linux `AF_PACKET` raw socket)
- Stage 2 planned: add DPDK capture backend that feeds the same queue/processor API.
- Stage 3 planned: switch interface from `lo` to mirrored RU/DU traffic interface without changing pipeline code.

## Build

Install dependencies on Debian/Ubuntu:

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config libpcap-dev
```

Build:

```bash
cmake -S . -B build
cmake --build build -j
```

## Run

Most capture modes need root or capabilities (`CAP_NET_RAW`, `CAP_NET_ADMIN`).

### 1) Stage 1, pcap streaming on loopback

```bash
sudo ./build/fronthaul_sniffer --mode pcap --iface lo --filter "ether"
```

### 2) Stage 1, raw socket streaming on loopback

```bash
sudo ./build/fronthaul_sniffer --mode socket --iface lo
```

Generate loopback test packets in another terminal:

```bash
ping 127.0.0.1
```

or

```bash
nc -l 127.0.0.1 9000
# in another terminal
printf 'test' | nc 127.0.0.1 9000
```

## CLI Options

```text
--mode <pcap|socket>
--iface <name>
--filter <bpf>
--snaplen <bytes>
--promisc <0|1>
--timeout-ms <ms>
--queue-packets <count>
--queue-bytes <bytes>
```

## Architecture

- `capture/*`: backend-specific packet ingestion
- `pipeline/packet_queue.hpp`: bounded queue with packet and byte limits
- `pipeline/packet_processor.*`: processing stage
- `main.cpp`: wiring and threading

Capture path:

```text
[pcap/socket backend] -> RawPacket -> bounded queue -> processor
```

This keeps your sniffer standalone and avoids any simulator source changes.

## Why a Buffer in C++?

C/C++ is fast enough for high packet rates, but buffering is still required because capture and processing rates are never perfectly matched.

Queue tuning rule of thumb:

- start with `--queue-packets 4096 --queue-bytes 33554432` (32 MiB)
- increase if you see drops under expected burst traffic
- if latency matters more than lossless behavior, keep queue small

The queue currently drops oldest packets when full to keep memory bounded.
