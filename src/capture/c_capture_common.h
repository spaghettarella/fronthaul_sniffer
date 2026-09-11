#pragma once

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Read timeout every live capture backend polls with. It is deliberately short
// and deliberately independent of the user-facing stall timeout, so a capture
// loop always returns to its stop_flag check within this bound even while the
// stream is silent. Passing the user's "timeout disabled" value straight to the
// backend instead is what used to wedge shutdown: libpcap leaves a zero packet
// buffer timeout undefined (it blocks forever on Linux) and a zero SO_RCVTIMEO
// means "no timeout at all".
#define FH_CAPTURE_POLL_TIMEOUT_MS 100

// Capture backends return 0 when they stopped because the caller asked them to,
// -1 on error (message in errbuf), and this when they gave up because no packet
// arrived within the configured stall timeout.
#define FH_CAPTURE_RC_STREAM_STOPPED 1

// Stall detection runs on the monotonic clock: wall clock can step, and packet
// timestamps stop arriving in exactly the situation being measured.
static inline void fh_capture_monotonic_now(struct timespec* out) {
    if (out != NULL) {
        (void)clock_gettime(CLOCK_MONOTONIC, out);
    }
}

static inline long fh_capture_elapsed_ms(const struct timespec* since) {
    if (since == NULL) {
        return 0;
    }

    struct timespec now;
    fh_capture_monotonic_now(&now);

    const long seconds = (long)(now.tv_sec - since->tv_sec);
    const long nanoseconds = now.tv_nsec - since->tv_nsec;
    return (seconds * 1000L) + (nanoseconds / (1000L * 1000L));
}

#ifdef __cplusplus
}
#endif
