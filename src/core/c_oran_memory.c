#include "c_oran_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static int compute_symbol_number(const fh_section_t* section, int scs_khz) {
    if (section == NULL || scs_khz <= 0) {
        return -1;
    }

    int slots_per_subframe = scs_khz / 15;
    if (slots_per_subframe <= 0) {
        slots_per_subframe = 1;
    }

    return section->frame_id * 10 * slots_per_subframe * 14 +
           section->subframe_id * slots_per_subframe * 14 +
           section->slot_id * 14 +
           section->start_symbol_id;
}

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

    // Zero-init then allocate array of symbol row pointers.
    memset(mem, 0, sizeof(*mem));
    mem->symbol_lowest = INT_MAX;
    mem->symbol_highest = INT_MIN;
    mem->symbol_count = 0;
    mem->max_symbols = max_symbols;
    mem->symbols = (fh_complex_t**)malloc(max_symbols * sizeof(fh_complex_t*));
    if (mem->symbols == NULL) {
        free(mem);
        return NULL;
    }

    // Allocate each symbol's IQ data buffer (3276 complex values per symbol).
    for (size_t i = 0; i < max_symbols; i++) {
        mem->symbols[i] = (fh_complex_t*)calloc(3276, sizeof(fh_complex_t));
        if (mem->symbols[i] == NULL) {
            // Clean up already-allocated rows on failure.
            for (size_t j = 0; j < i; j++) {
                free(mem->symbols[j]);
            }
            free(mem->symbols);
            free(mem);
            return NULL;
        }
    }

    return mem;
}

void fh_oran_memory_destroy(fh_oran_memory_t* mem) {
    // Destroy is idempotent for NULL inputs.
    if (mem == NULL) {
        return;
    }

    // Free each symbol's IQ buffer, then the row pointer array, then the container.
    if (mem->symbols != NULL) {
        for (size_t i = 0; i < mem->max_symbols; i++) {
            if (mem->symbols[i] != NULL) {
                free(mem->symbols[i]);
                mem->symbols[i] = NULL;
            }
        }
        free(mem->symbols);
        mem->symbols = NULL;
    }
    free(mem);
}

int fh_oran_memory_save(fh_oran_memory_t* mem, const fh_decoded_packet_t* pkt, int scs_khz) {
    // Validate pointers from caller.
    if (mem == NULL || pkt == NULL || pkt->sections == NULL || pkt->section_count <= 0) {
        return -1;
    }

    // Iterate through each section in the packet and place its IQ data at the correct PRB offset.
    for (int i = 0; i < pkt->section_count; i++) {
        const fh_section_t* section = &pkt->sections[i];

        int symbol_number = compute_symbol_number(section, scs_khz);
        if (symbol_number < 0) {
            continue;
        }

        if (mem->symbol_count == 0) {
            mem->symbol_lowest = symbol_number;
            mem->symbol_highest = symbol_number;
        } else if (symbol_number < mem->symbol_lowest) {
            printf("Warning: symbol number %d is older than the current memory base and will be skipped\n", symbol_number);
            continue;
        } else if (symbol_number > mem->symbol_highest) {
            mem->symbol_highest = symbol_number;
        }

        int idx = symbol_number - mem->symbol_lowest;
        if (idx < 0 || (size_t)idx >= mem->max_symbols) {
            printf("Warning: symbol number %d is out of memory bounds and will be skipped\n", symbol_number);
            continue;
        }

        // Calculate the destination offset from the PRB start inside the symbol row.
        size_t start_iq = (size_t)section->start_prbu * 12;
        size_t prb_count = section->num_prbu;
        if (prb_count == 0) { // Already checked in decoder but for sanity checked again here.
            prb_count = 273;
        }
        size_t iq_count = prb_count * 12;

        // Keep the copy inside the fixed 3276-sample symbol row.
        if (start_iq >= 3276) {
            printf("Warning: start_prbu %u is out of symbol bounds and will be skipped\n", section->start_prbu);
            continue;
        }

        if (section->iq_data == NULL || iq_count == 0) {
            continue;
        }

        // Copy only the requested PRB range into the correct location in the symbol buffer.
        memcpy(mem->symbols[idx] + start_iq, section->iq_data, iq_count * sizeof(fh_complex_t));
        if ((size_t)(idx + 1) > mem->symbol_count) { // TODO: idx and symbol count do not neccesarily start at 0.
            mem->symbol_count = (size_t)(idx + 1);
        }
    }

    return 0;
}

fh_oran_memory_stats_t fh_oran_memory_stats(const fh_oran_memory_t* mem) {
    // Return zeroed stats for NULL input.
    fh_oran_memory_stats_t stats;
    memset(&stats, 0, sizeof(stats));

    // Snapshot counters for reporting.
    if (mem != NULL) {
        stats.symbol_lowest = mem->symbol_lowest;
        stats.symbol_highest = mem->symbol_highest;
        stats.stored_symbols = mem->symbol_count;
        stats.max_symbols = mem->max_symbols;
    }

    return stats;
}
