# queue_drain

Standalone consumer for the PHY tap shared-memory queue.

It opens the same POSIX shared-memory object that the tap producer creates via `tap_ul_queue_name`, reads records in order, prints each symbol's metadata, and advances the shared `read_sequence`. If `--max-records` is omitted, it keeps running continuously.

## Build

```bash
cmake -S src/capture/ocudu_phy_tap_queue_drain -B build/ocudu_phy_tap_queue_drain
cmake --build build/ocudu_phy_tap_queue_drain -j2
```

## Run

```bash
./build/ocudu_phy_tap_queue_drain/queue_drain --queue-name=/ocudu_phy_tap_queue --max-records=1000
```

Optional flags:

- `--log-file=/tmp/queue_drain.log` writes the output to a file.
- `--poll-sleep-ms=1` sets the wait time when the queue is empty.
- `--queue-name` must match the `tap_ul_queue_name` passed to the gNB PHY tap.
- Omit `--max-records` to keep draining forever.

## Connection

- The gNB producer writes records into `/dev/shm/<queue-name>` using the shared-memory ring buffer.
- The consumer maps the same object, reads the record at `read_sequence % capacity`, prints it, and increments `read_sequence`.
- If the producer runs faster than the consumer, the producer overwrites old records and updates the shared dropped-count counter.
