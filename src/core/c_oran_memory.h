#pragma once

#include "core/c_fronthaul_decoder.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fh_oran_memory {
    fh_decoded_packet_t* packets;
    size_t packet_count;
    size_t max_packets;
} fh_oran_memory_t;

typedef struct fh_oran_memory_stats {
    size_t stored_packets;
    size_t max_packets;
} fh_oran_memory_stats_t;

fh_oran_memory_t* fh_oran_memory_create(size_t max_packets);
void fh_oran_memory_destroy(fh_oran_memory_t* mem);

int fh_oran_memory_save(fh_oran_memory_t* mem, const fh_decoded_packet_t* pkt);
fh_oran_memory_stats_t fh_oran_memory_stats(const fh_oran_memory_t* mem);

#ifdef __cplusplus
}
#endif
