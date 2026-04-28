#include "c_oran_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

fh_oran_memory_t* fh_oran_memory_create(size_t max_packets) {
    if (max_packets == 0) {
        return NULL;
    }

    fh_oran_memory_t* mem = (fh_oran_memory_t*)malloc(sizeof(*mem));
    if (mem == NULL) {
        return NULL;
    }

    memset(mem, 0, sizeof(*mem));
    mem->max_packets = max_packets;
    mem->packets = (fh_decoded_packet_t*)calloc(max_packets, sizeof(fh_decoded_packet_t));
    if (mem->packets == NULL) {
        free(mem);
        return NULL;
    }

    return mem;
}

void fh_oran_memory_destroy(fh_oran_memory_t* mem) {
    if (mem == NULL) {
        return;
    }

    if (mem->packets != NULL) {
        free(mem->packets);
        mem->packets = NULL;
    }
    free(mem);
}

int fh_oran_memory_save(fh_oran_memory_t* mem, const fh_decoded_packet_t* pkt) {
    if (mem == NULL || pkt == NULL) {
        return -1;
    }

    if (mem->packet_count >= mem->max_packets) {
        return 1;
    }

    size_t idx = mem->packet_count;
    memcpy(&mem->packets[idx], pkt, sizeof(*pkt));
    mem->packet_count += 1;

    return 0;
}

fh_oran_memory_stats_t fh_oran_memory_stats(const fh_oran_memory_t* mem) {
    fh_oran_memory_stats_t stats;
    memset(&stats, 0, sizeof(stats));

    if (mem != NULL) {
        stats.stored_packets = mem->packet_count;
        stats.max_packets = mem->max_packets;
    }

    return stats;
}
