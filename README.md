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

`--timeout-ms <ms>` is the stream-stop timeout, equivalent to "Exit when stream
stops" on the startup screen: once the capture has been silent for that long the
run ends by itself. It is armed only after the first packet arrives, so starting
the sniffer before the gNB/RU does not cut the run short, and omitting it keeps
the capture alive until the window is closed. It is not a read timeout — every
live backend polls on a fixed `FH_CAPTURE_POLL_TIMEOUT_MS` interval so it always
returns to its stop-flag check, silent stream or not. Run `--help` for the
authoritative option list.

## Zoom

The plot can be zoomed independently on both axes to inspect part of a signal
closely. It is a pure display transform: nothing it does reaches the capture,
decode, memory ring, rolling-mean accumulators or the trigger, so Memory size,
averaging period and capture depth all keep their configured values and
measurements are unaffected.

- **Scroll wheel** over the plot zooms both axes about the cursor, keeping the
  data under the pointer fixed. **Shift+wheel** zooms horizontally only,
  **Ctrl+wheel** vertically only.
- **Left-drag** on the plot moves the window, carrying the data under the
  cursor along with it. Only the position where the drag *started* has to be on
  the plot, so a gesture that runs out over a panel or off the edge keeps
  working.
- **Arrow keys** move the window by 10% of a screenful per press.
- The **Zoom** window shows the window position and size in real units
  (resource elements horizontally; symbol rows or, in Curve view, percent of
  full scale vertically), lets either be typed in directly, and has a
  **Reset to full view** button. An overview graphic above those fields shows
  the window as a box inside the full extent, so it is always clear which part
  of the memory is on screen.

Axis ticks re-derive their density from the zoomed window, so zooming in
promotes finer divisions into view - individual resource elements appear within
a PRB, and individual symbols within a slot - and zooming out thins them again.
Once the heatmap's cells are magnified past a couple of pixels the texture
filter switches from linear to nearest, so cells read as the discrete samples
they are rather than as invented gradients.

The vertical window is kept as a fraction rather than an absolute row range,
which is what lets it survive the row count changing underneath it when Memory
size, the averaging period or a trigger capture takes over the view. Waterfall
and Curve view each keep their own vertical window, since symbol rows and
magnitude are not interchangeable; the horizontal window is shared.

## Value at cursor

The **Markers** window has a **Value at cursor** tick box that reads out the
cell under the pointer without placing a marker - useful for sweeping across a
region rather than measuring one fixed point. It reports the absolute IQ
magnitude and the percentage of the on-screen baseline, the same two numbers a
marker gives, resolved through the same lookup so the two can never disagree
about what a cell holds. The position is inverted through the current zoom
window, so it reads the right cell at any zoom.

Two independent destinations, either or both:

- **Show next to cursor** draws a crosshair and a value box pinned to the
  pointer.
- **Show here** puts the reading in the Markers window, which is easier to read
  while watching a live stream.

Like markers, this describes the waterfall grid only. In Curve view the vertical
axis is magnitude rather than time, so there is no cell under the cursor to
read and the readout says so. Hovering a panel reports nothing rather than the
cell hidden behind it.

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
