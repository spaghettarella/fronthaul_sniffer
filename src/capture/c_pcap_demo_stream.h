#pragma once

#include "../core/c_fronthaul_decoder.h"
#include "c_channel_filter.h"

#include <signal.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fh_pcap_stream_options {
    const char* pcap_path;
    double demo_pcap_timeout; // in seconds
    // Optional eAxC / direction filter, applied before packets reach the
    // queue. Leave zeroed to replay the file unfiltered, which is what the
    // startup screen's background playback wants.
    fh_channel_filter_t filter;
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