#pragma once

#include "../core/c_fronthaul_decoder.h"

#include <signal.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fh_capture_loopback_options {
    const char* interface_name;  // e.g., "lo"
    int snaplen;
    int timeout_ms;
    // Optional filter: if filter_has_eaxc is non-zero, only packets matching
    // filter_eaxc (16-bit) are considered. If filter_has_direction is non-zero,
    // only packets matching filter_direction (0=UL,1=DL) are considered.
    int filter_has_eaxc;
    uint16_t filter_eaxc;
    int filter_has_direction;
    int filter_direction;
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
