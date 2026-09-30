#pragma once

#include "../core/c_fronthaul_decoder.h"
#include "c_capture_common.h"
#include "c_channel_filter.h"

#include <signal.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fh_capture_loopback_options {
    const char* interface_name;  // e.g., "lo"
    int snaplen;
    // Give up and return FH_CAPTURE_RC_STREAM_STOPPED once this many
    // milliseconds pass with no matching packet. 0 keeps capturing forever.
    // Only armed after the first packet, so starting the sniffer before the
    // traffic source does not end the run immediately.
    int stall_timeout_ms;
    // Optional eAxC / direction filter, applied before packets reach the queue.
    fh_channel_filter_t filter;
    // Optional running selection on top of the filter; see
    // fh_channel_selection_pack. Read with __atomic_load_n, so write it with
    // __atomic_store_n. NULL, or a word of 0, decodes every channel.
    const uint64_t* live_selection;
} fh_capture_loopback_options_t;

typedef int (*fh_packet_callback_t)(const fh_packet_t* pkt, void* user_ctx);

int fh_capture_loopback_run(
    const fh_capture_loopback_options_t* options,
    fh_packet_callback_t on_packet,
    void* user_ctx,
    const volatile sig_atomic_t* stop_flag,
    char* errbuf,
    size_t errbuf_len);

#ifdef __cplusplus
}
#endif
