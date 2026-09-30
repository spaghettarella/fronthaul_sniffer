#pragma once

#include "../core/c_fronthaul_decoder.h"
#include "c_channel_filter.h"

#include <signal.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Highest replay speed the UI offers, in packets per second. Above what any
// part of the pipeline sustains today, so the top of the range simply means
// as fast as it will go; the schedule itself has no ceiling of its own.
#define FH_PCAP_MAX_SPEED_PPS 10000000

// Replay schedule: when the last packet was due. Owned by the caller when a
// replay spans several fh_pcap_stream_run calls, as looping does, so each pass
// picks up the timing where the last one left off. Zero-initialise it and
// leave the fields alone.
typedef struct fh_pcap_schedule {
    int started;
    double origin;    // monotonic time the schedule counts from
    double last_due;  // when the previous packet was due, relative to origin
} fh_pcap_schedule_t;

typedef struct fh_pcap_stream_options {
    const char* pcap_path;
    double demo_pcap_timeout; // in seconds
    // Optional replay speed in packets per second that another thread may
    // change while the replay runs; read with __atomic_load_n, so write it with
    // __atomic_store_n. Takes precedence over demo_pcap_timeout while it holds
    // a positive value. NULL keeps the fixed pacing above.
    const int* live_speed_pps;
    // Optional schedule carried across calls. Without one, every call starts
    // a fresh schedule, so a looped replay loses the time spent between
    // passes - at high speed, on a short file, that is a noticeable share.
    fh_pcap_schedule_t* schedule;
    // Optional eAxC / direction filter, applied before packets reach the
    // queue. Leave zeroed to replay the file unfiltered, which is what the
    // startup screen's background playback wants.
    fh_channel_filter_t filter;
    // Optional running selection on top of the filter; see
    // fh_channel_selection_pack. Read with __atomic_load_n, so write it with
    // __atomic_store_n. NULL, or a word of 0, decodes every channel.
    const uint64_t* live_selection;
} fh_pcap_stream_options_t;

typedef int (*fh_packet_callback_t)(const fh_packet_t* pkt, void* user_ctx);

int fh_pcap_stream_run(
    const fh_pcap_stream_options_t* options,
    fh_packet_callback_t on_packet,
    void* user_ctx,
    const volatile sig_atomic_t* stop_flag,
    char* errbuf,
    size_t errbuf_len);

#ifdef __cplusplus
}
#endif