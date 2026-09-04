#include "c_oran_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static int compute_symbol_number(uint64_t frame_counter, const fh_section_t* section, int scs_khz) {
    if (section == NULL || scs_khz <= 0) {
        return -1;
    }

    int slots_per_subframe = scs_khz / 15;
    if (slots_per_subframe <= 0) {
        slots_per_subframe = 1;
    }

    // The `frame_counter` tracks the number of 256-frame wraparounds observed.
    // The actual absolute frame number is therefore:
    //    full_frame = frame_counter * 256 + section->frame_id
    uint64_t full_frame = frame_counter * 256ULL + (uint8_t)section->frame_id;

    // Compute a symbol index from the absolute frame number, subframe, slot and symbol.
    // Keep the final result as int for compatibility with the rest of the code.
    uint64_t sym = full_frame * 10ULL * (uint64_t)slots_per_subframe * 14ULL
                 + (uint64_t)section->subframe_id * (uint64_t)slots_per_subframe * 14ULL
                 + (uint64_t)section->slot_id * 14ULL
                 + (uint64_t)section->start_symbol_id;

    if (sym > (uint64_t)INT_MAX) {
        // Avoid overflow when converting to int; saturate to INT_MAX.
        return INT_MAX;
    }
    return (int)sym;
}

static int clamp_symbol_number(int64_t value) {
    if (value > (int64_t)INT_MAX) {
        return INT_MAX;
    }
    if (value < (int64_t)INT_MIN) {
        return INT_MIN;
    }
    return (int)value;
}

static void clear_symbol_row(fh_oran_memory_t* mem, size_t idx) {
    if (mem == NULL || mem->symbols == NULL || idx >= mem->max_symbols || mem->symbols[idx] == NULL) {
        return;
    }

    memset(mem->symbols[idx], 0, 3276 * sizeof(fh_complex_t));
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
    mem->index_symbol_highest = 0;
    mem->max_symbols = max_symbols;
    mem->replay_symbol_offset = 0;
    mem->loop_first_symbol = INT_MIN;
    mem->loop_last_symbol = INT_MIN;
    mem->symbols = (fh_complex_t**)malloc(max_symbols * sizeof(fh_complex_t*));
    if (mem->symbols == NULL) {
        free(mem);
        return NULL;
    }

    mem->row_symbol_numbers = (int*)malloc(max_symbols * sizeof(int));
    if (mem->row_symbol_numbers == NULL) {
        free(mem->symbols);
        free(mem);
        return NULL;
    }
    for (size_t i = 0; i < max_symbols; ++i) {
        mem->row_symbol_numbers[i] = INT_MIN;
    }

    // Allocate each symbol's IQ data buffer (3276 complex values per symbol).
    for (size_t i = 0; i < max_symbols; i++) {
        mem->symbols[i] = (fh_complex_t*)calloc(3276, sizeof(fh_complex_t));
        if (mem->symbols[i] == NULL) {
            // Clean up already-allocated rows on failure.
            for (size_t j = 0; j < i; j++) {
                free(mem->symbols[j]);
            }
            free(mem->row_symbol_numbers);
            free(mem->symbols);
            free(mem);
            return NULL;
        }
    }

    // Initialize frame tracking for wraparound detection.
    mem->frame_counter = 0;
    mem->last_frame_id = 0;

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
    free(mem->row_symbol_numbers);
    mem->row_symbol_numbers = NULL;
    free(mem);
}

int fh_oran_memory_save(fh_oran_memory_t* mem, const fh_decoded_packet_t* pkt, int scs_khz) {
    // Validate pointers from caller.
    if (mem == NULL || pkt == NULL || pkt->sections == NULL || pkt->section_count <= 0) {
        return -1;
    }

    if (scs_khz > 0) {
        mem->scs_khz = scs_khz;
    }

    // Iterate through each section in the packet and place its IQ data at the correct PRB offset.
    for (int i = 0; i < pkt->section_count; i++) {
        const fh_section_t* section = &pkt->sections[i];

        // Detect frame_id wraparound (8-bit counter resets to 0 after 255)
        if (section->frame_id < mem->last_frame_id) {
            mem->frame_counter++;
        }
        mem->last_frame_id = section->frame_id;

        int raw_symbol_number = compute_symbol_number(mem->frame_counter, section, scs_khz);
        if (raw_symbol_number < 0) {
            continue;
        }

        if (mem->loop_first_symbol == INT_MIN || raw_symbol_number < mem->loop_first_symbol) {
            mem->loop_first_symbol = raw_symbol_number;
        }
        if (mem->loop_last_symbol == INT_MIN || raw_symbol_number > mem->loop_last_symbol) {
            mem->loop_last_symbol = raw_symbol_number;
        }

        int symbol_number = clamp_symbol_number(mem->replay_symbol_offset + (int64_t)raw_symbol_number);

        if (mem->symbol_highest == INT_MIN) {
            // First symbol defines the initial ring position.
            mem->index_symbol_highest = 0;
            mem->symbol_highest = symbol_number;
            mem->symbol_lowest = symbol_number - (int)(mem->max_symbols - 1);
        } else if (symbol_number < mem->symbol_lowest) {
            printf("Warning: symbol number %d is older than the current memory base and will be skipped\n", symbol_number);
            continue;
        }

        if (symbol_number > mem->symbol_highest) {
            size_t delta = (size_t)(symbol_number - mem->symbol_highest);
            size_t old_highest_idx = mem->index_symbol_highest;

            mem->index_symbol_highest = (old_highest_idx + delta) % mem->max_symbols;
            mem->symbol_highest = symbol_number;
            mem->symbol_lowest = symbol_number - (int)(mem->max_symbols - 1);
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

        size_t idx = (mem->index_symbol_highest + mem->max_symbols - (size_t)(mem->symbol_highest - symbol_number)) % mem->max_symbols;

        // Row reuse is common as the ring advances. Clear only when a row starts
        // representing a different absolute symbol to avoid eager gap clearing.
        if (mem->row_symbol_numbers != NULL && mem->row_symbol_numbers[idx] != symbol_number) {
            clear_symbol_row(mem, idx);
            mem->row_symbol_numbers[idx] = symbol_number;
        }

        // Copy only the requested PRB range into the correct location in the symbol buffer.
        memcpy(mem->symbols[idx] + start_iq, section->iq_data, iq_count * sizeof(fh_complex_t));
    }

    return 0;
}

int fh_oran_memory_wipe(fh_oran_memory_t* mem) {
    // Validate pointer from caller.
    if (mem == NULL || mem->symbols == NULL) {
        return -1;
    }

    // Zero out all symbol buffers and reset counters.
    for (size_t i = 0; i < mem->max_symbols; i++) {
        if (mem->symbols[i] != NULL) {
            memset(mem->symbols[i], 0, 3276 * sizeof(fh_complex_t));
        }
        if (mem->row_symbol_numbers != NULL) {
            mem->row_symbol_numbers[i] = INT_MIN;
        }
    }
    mem->symbol_lowest = INT_MAX;
    mem->symbol_highest = INT_MIN;
    mem->replay_symbol_offset = 0;
    mem->loop_first_symbol = INT_MIN;
    mem->loop_last_symbol = INT_MIN;
    mem->frame_counter = 0;
    mem->last_frame_id = 0;

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
        stats.max_symbols = mem->max_symbols;
    }

    return stats;
}

void fh_oran_memory_advance_loop(fh_oran_memory_t* mem) {
    if (mem == NULL) {
        return;
    }

    if (mem->loop_first_symbol != INT_MIN && mem->loop_last_symbol != INT_MIN) {
        int64_t loop_span = (int64_t)mem->loop_last_symbol - (int64_t)mem->loop_first_symbol + 1;
        if (loop_span < 1) {
            loop_span = 1;
        }
        mem->replay_symbol_offset += loop_span;
    }

    mem->loop_first_symbol = INT_MIN;
    mem->loop_last_symbol = INT_MIN;
    mem->frame_counter = 0;
    mem->last_frame_id = 0;
}
