#pragma once

#include "../core/c_fronthaul_decoder.h"

#include <signal.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fh_capture_ocudu_phy_tap_options {
	const char* queue_name;
	int poll_sleep_ms;
} fh_capture_ocudu_phy_tap_options_t;

typedef int (*fh_packet_callback_t)(const fh_packet_t* pkt, void* user_ctx);

int fh_capture_ocudu_phy_tap_run(
	const fh_capture_ocudu_phy_tap_options_t* options,
	fh_packet_callback_t on_packet,
	void* user_ctx,
	const volatile sig_atomic_t* stop_flag,
	char* errbuf,
	size_t errbuf_len);

#ifdef __cplusplus
}
#endif
