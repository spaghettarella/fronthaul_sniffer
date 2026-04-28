#pragma once

#include "core/c_fronthaul_decoder.h"

#include <signal.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fh_packet_queue fh_packet_queue_t;

typedef struct fh_queue_stats {
    size_t dropped_packets;
    size_t dropped_bytes;
    size_t queued_packets;
    size_t queued_bytes;
} fh_queue_stats_t;

fh_packet_queue_t* fh_packet_queue_create(size_t max_packets, size_t max_bytes);
void fh_packet_queue_destroy(fh_packet_queue_t* q);

int fh_packet_queue_push_copy(fh_packet_queue_t* q, const fh_packet_t* pkt);
int fh_packet_queue_pop(fh_packet_queue_t* q, fh_packet_t* out, const volatile sig_atomic_t* stop_flag);

void fh_packet_release(fh_packet_t* pkt);
void fh_packet_queue_notify_all(fh_packet_queue_t* q);
fh_queue_stats_t fh_packet_queue_stats(fh_packet_queue_t* q);

#ifdef __cplusplus
}
#endif
