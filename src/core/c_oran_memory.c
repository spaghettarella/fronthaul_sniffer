#include "c_oran_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

fh_oran_memory_t* fh_oran_memory_create(size_t max_symbols) {
    // Reject nonsensical capacity requests early.
    if (max_symbols == 0) {
        return NULL;
    } else if (max_symbols > 14*16*10*256) {
        max_symbols = 14*16*10*256; // Max possible symbols in 256 Frames of 10 subframes of 16 slots of 14 symbols
    }

    // Allocate the container metadata first.
    fh_oran_memory_t* mem = (fh_oran_memory_t*)malloc(sizeof(*mem));
    if (mem == NULL) {
        return NULL;
    }

    // Zero-init then allocate fixed-capacity storage for decoded packets to write into.
    memset(mem, 0, sizeof(*mem));
    mem->max_symbols = max_symbols;
    mem->symbols = (fh_decoded_packet_t*)calloc(max_symbols, 273*12*sizeof(fh_complex_t)); // Allocate max possible symbols with IQ data buffer for 273 PRBs * 12 samples each
    if (mem->symbols == NULL) {
        free(mem);
        return NULL;
    }

    return mem;
}

void fh_oran_memory_destroy(fh_oran_memory_t* mem) {
    // Destroy is idempotent for NULL inputs.
    if (mem == NULL) {
        return;
    }

    // Free symbol array first, then the container itself.
    if (mem->symbols != NULL) {
        free(mem->symbols);
        mem->symbols = NULL;
    }
    free(mem);
}

int fh_oran_memory_save(fh_oran_memory_t* mem, const fh_decoded_packet_t* pkt) {
    // Validate pointers from caller.
    if (mem == NULL || pkt == NULL) {
        return -1;
    }

    // Signal "full" without overwriting existing entries.
    if (mem->symbol_count >= mem->max_symbols) {
        return 1;
    }

    // Append at tail and advance count.
    // Note: this is a shallow struct copy; pointer fields are copied as-is.
    size_t idx = mem->symbol_count;
    memcpy(&mem->symbols[idx], pkt, sizeof(*pkt));
    mem->symbol_count += 1;

    return 0;
}

fh_oran_memory_stats_t fh_oran_memory_stats(const fh_oran_memory_t* mem) {
    // Return zeroed stats for NULL input.
    fh_oran_memory_stats_t stats;
    memset(&stats, 0, sizeof(stats));

    // Snapshot counters for reporting.
    if (mem != NULL) {
        stats.stored_symbols = mem->symbol_count;
        stats.max_symbols = mem->max_symbols;
    }

    return stats;
}
