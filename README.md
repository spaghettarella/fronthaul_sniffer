# Fronthaul Sniffer

Standalone mixed C/C++ sniffer scaffold for O-RAN fronthaul traffic. Capture and queue pipeline are implemented in C for a low-overhead fast path, while higher-level processing remains in C++.

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

- `capture/c_capture.*` (C): backend-specific packet ingestion (`pcap`, `socket`)
- `pipeline/c_packet_queue.*` (C): bounded queue with packet and byte limits
- `pipeline/packet_processor.*` (C++): higher-level processing stage
- `main.cpp` (C++): wiring, threading, and orchestration

Capture path:

```text
[C capture backend] -> C packet queue -> C++ processor
```

This keeps your sniffer standalone and avoids any simulator source changes.

## Why a Buffer in C++?

C/C++ is fast enough for high packet rates, but buffering is still required because capture and processing rates are never perfectly matched.

Queue tuning rule of thumb:

- start with `--queue-packets 4096 --queue-bytes 33554432` (32 MiB)
- increase if you see drops under expected burst traffic
- if latency matters more than lossless behavior, keep queue small

The queue currently drops oldest packets when full to keep memory bounded.

## Committing Changes

GitHub rejects any single file over 100 MB, and it does so at push time — by
which point the file is already inside a commit, and only rewriting history
gets it back out. Packet captures are the usual culprit: a few seconds of
100 MHz fronthaul traffic runs to hundreds of megabytes.

Two guards are in place:

- `.gitignore` skips `*.pcap` and `*.pcapng`, so recordings are never picked
  up by `git add .`. The small captures already tracked under `src/capture/`
  are unaffected — ignore rules do not apply to files git already follows.
- `.git/hooks/pre-commit` refuses any staged file over 100 MB and names it.

The hook lives in `.git/`, which is not versioned, so it protects only the
clone it was created in. Reinstall it after cloning elsewhere.

Normal workflow:

```bash
git status                  # check nothing unexpected is about to be added
git add .
git commit -m "message"
git push
```

To commit a capture deliberately, override the ignore rule — but keep it
under 100 MB:

```bash
git add -f src/capture/small_capture.pcap
```

If a large file does slip into a commit that has not been pushed yet, drop it
from the commit rather than rewriting history. `--cached` removes it from git
only, leaving the file on disk:

```bash
git rm --cached path/to/large.pcap
git commit --amend --no-edit
git push
```

Once such a commit has been pushed, or if the file is several commits back,
this no longer works and the repository needs `git filter-repo` (or BFG)
followed by a force push — which rewrites history for everyone. Avoiding that
is the reason the hook exists.
