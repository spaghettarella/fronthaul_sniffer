#include "c_iq_analysis.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define IQ_ANALYSIS_MAX_STREAMS 16
#define IQ_ANALYSIS_SAMPLES_PER_SYMBOL 3276
#define IQ_ANALYSIS_PRBS 273
#define IQ_ANALYSIS_SAMPLES_PER_PRB 12

// Magnitudes span a wide dynamic range and the two paths sit at different
// absolute scales, so the histogram bins by log2 of magnitude. Percentiles
// read off it are then divided by the mean, which is what makes the two runs
// comparable without knowing either scale in advance.
#define IQ_HIST_BINS 200
#define IQ_HIST_LOG2_MIN (-60.0)
#define IQ_HIST_LOG2_STEP 0.5

// Symbols are only read once they can no longer be written to, so a row is
// never measured while it is still being filled in by later sections.
#define IQ_ANALYSIS_SETTLE_SYMBOLS 32

typedef struct {
    int in_use;
    uint16_t eaxc_id;
    uint8_t data_direction;

    int symbols_done;
    int has_last_symbol;
    int last_symbol;

    uint64_t sample_count;
    uint64_t zero_count;

    double sum_magnitude;
    double sum_magnitude_sq;
    float max_magnitude;
    float min_nonzero_magnitude;
    float min_nonzero_component;  // smallest non-zero |I| or |Q|: hints at the quantisation step

    uint64_t histogram[IQ_HIST_BINS];

    // Peak-to-mean within a single symbol. This is the direct measure of how
    // "uniform" a symbol looks: a flat symbol sits near 1, a spiky one high.
    double sum_symbol_crest;
    double max_symbol_crest;
    double min_symbol_crest;
    uint64_t symbol_crest_count;

    // Spread between per-PRB means inside one symbol. BFP carries a separate
    // exponent per PRB, so a mishandled exponent shows up here as steps from
    // one PRB to the next that the uncompressed path does not have.
    double sum_prb_spread;
    double max_prb_spread;
    uint64_t prb_spread_count;
} fh_iq_stream_stats_t;

struct fh_iq_analysis {
    int target_symbols;
    fh_iq_stream_stats_t streams[IQ_ANALYSIS_MAX_STREAMS];
};

fh_iq_analysis_t* fh_iq_analysis_create(int target_symbols_per_stream) {
    if (target_symbols_per_stream <= 0) {
        return NULL;
    }
    fh_iq_analysis_t* analysis = (fh_iq_analysis_t*)calloc(1, sizeof(*analysis));
    if (analysis == NULL) {
        return NULL;
    }
    analysis->target_symbols = target_symbols_per_stream;
    return analysis;
}

void fh_iq_analysis_destroy(fh_iq_analysis_t* analysis) {
    free(analysis);
}

static fh_iq_stream_stats_t* find_stream(fh_iq_analysis_t* analysis, uint16_t eaxc_id, uint8_t direction) {
    for (int i = 0; i < IQ_ANALYSIS_MAX_STREAMS; i++) {
        fh_iq_stream_stats_t* s = &analysis->streams[i];
        if (s->in_use && s->eaxc_id == eaxc_id && s->data_direction == direction) {
            return s;
        }
    }
    for (int i = 0; i < IQ_ANALYSIS_MAX_STREAMS; i++) {
        fh_iq_stream_stats_t* s = &analysis->streams[i];
        if (!s->in_use) {
            memset(s, 0, sizeof(*s));
            s->in_use = 1;
            s->eaxc_id = eaxc_id;
            s->data_direction = direction;
            s->min_nonzero_magnitude = INFINITY;
            s->min_nonzero_component = INFINITY;
            s->min_symbol_crest = INFINITY;
            return s;
        }
    }
    return NULL;
}

static void histogram_add(fh_iq_stream_stats_t* s, float magnitude) {
    const double l2 = log2((double)magnitude);
    int bin = (int)((l2 - IQ_HIST_LOG2_MIN) / IQ_HIST_LOG2_STEP);
    if (bin < 0) {
        bin = 0;
    }
    if (bin >= IQ_HIST_BINS) {
        bin = IQ_HIST_BINS - 1;
    }
    s->histogram[bin]++;
}

// Reads back the magnitude at the given percentile from the log2 histogram.
static double histogram_percentile(const fh_iq_stream_stats_t* s, uint64_t nonzero_total, double percentile) {
    if (nonzero_total == 0) {
        return 0.0;
    }
    const uint64_t target = (uint64_t)(percentile / 100.0 * (double)nonzero_total);
    uint64_t running = 0;
    for (int bin = 0; bin < IQ_HIST_BINS; bin++) {
        running += s->histogram[bin];
        if (running >= target) {
            const double l2 = IQ_HIST_LOG2_MIN + ((double)bin + 0.5) * IQ_HIST_LOG2_STEP;
            return pow(2.0, l2);
        }
    }
    return 0.0;
}

static void measure_symbol(fh_iq_stream_stats_t* s, const fh_complex_t* row) {
    double symbol_sum = 0.0;
    double symbol_peak = 0.0;
    uint64_t symbol_nonzero = 0;

    double prb_sum[IQ_ANALYSIS_PRBS];
    int prb_nonzero[IQ_ANALYSIS_PRBS];
    memset(prb_sum, 0, sizeof(prb_sum));
    memset(prb_nonzero, 0, sizeof(prb_nonzero));

    for (int i = 0; i < IQ_ANALYSIS_SAMPLES_PER_SYMBOL; i++) {
#ifdef __cplusplus
        const float i_val = row[i].real();
        const float q_val = row[i].imag();
#else
        const float i_val = crealf(row[i]);
        const float q_val = cimagf(row[i]);
#endif
        const float magnitude = sqrtf(i_val * i_val + q_val * q_val);

        s->sample_count++;
        if (magnitude <= 0.0f) {
            s->zero_count++;
            continue;
        }

        s->sum_magnitude += magnitude;
        s->sum_magnitude_sq += (double)magnitude * (double)magnitude;
        if (magnitude > s->max_magnitude) {
            s->max_magnitude = magnitude;
        }
        if (magnitude < s->min_nonzero_magnitude) {
            s->min_nonzero_magnitude = magnitude;
        }

        const float abs_i = fabsf(i_val);
        const float abs_q = fabsf(q_val);
        if (abs_i > 0.0f && abs_i < s->min_nonzero_component) {
            s->min_nonzero_component = abs_i;
        }
        if (abs_q > 0.0f && abs_q < s->min_nonzero_component) {
            s->min_nonzero_component = abs_q;
        }

        histogram_add(s, magnitude);

        symbol_sum += magnitude;
        symbol_nonzero++;
        if ((double)magnitude > symbol_peak) {
            symbol_peak = magnitude;
        }

        const int prb = i / IQ_ANALYSIS_SAMPLES_PER_PRB;
        prb_sum[prb] += magnitude;
        prb_nonzero[prb]++;
    }

    if (symbol_nonzero == 0 || symbol_sum <= 0.0) {
        return;  // an empty symbol says nothing about distribution shape
    }

    const double symbol_mean = symbol_sum / (double)symbol_nonzero;
    const double crest = symbol_peak / symbol_mean;
    s->sum_symbol_crest += crest;
    if (crest > s->max_symbol_crest) {
        s->max_symbol_crest = crest;
    }
    if (crest < s->min_symbol_crest) {
        s->min_symbol_crest = crest;
    }
    s->symbol_crest_count++;

    // Compare the loudest and quietest occupied PRB in this symbol. Only
    // count PRBs that are fully occupied, so a partly-scheduled edge PRB
    // doesn't masquerade as a scaling step.
    double prb_max = 0.0;
    double prb_min = INFINITY;
    int prb_used = 0;
    for (int prb = 0; prb < IQ_ANALYSIS_PRBS; prb++) {
        if (prb_nonzero[prb] < IQ_ANALYSIS_SAMPLES_PER_PRB) {
            continue;
        }
        const double prb_mean = prb_sum[prb] / (double)prb_nonzero[prb];
        if (prb_mean > prb_max) {
            prb_max = prb_mean;
        }
        if (prb_mean < prb_min) {
            prb_min = prb_mean;
        }
        prb_used++;
    }
    if (prb_used >= 2 && prb_min > 0.0) {
        const double spread = prb_max / prb_min;
        s->sum_prb_spread += spread;
        if (spread > s->max_prb_spread) {
            s->max_prb_spread = spread;
        }
        s->prb_spread_count++;
    }
}

void fh_iq_analysis_observe(fh_iq_analysis_t* analysis,
                            uint16_t eaxc_id,
                            uint8_t data_direction,
                            const fh_oran_memory_t* memory) {
    if (analysis == NULL || memory == NULL || memory->symbols == NULL) {
        return;
    }
    if (memory->symbol_highest < memory->symbol_lowest) {
        return;
    }

    fh_iq_stream_stats_t* s = find_stream(analysis, eaxc_id, data_direction);
    if (s == NULL || s->symbols_done >= analysis->target_symbols) {
        return;
    }

    // Stay clear of the newest symbols, which may still be gathering sections.
    const int newest_settled = memory->symbol_highest - IQ_ANALYSIS_SETTLE_SYMBOLS;
    if (newest_settled < memory->symbol_lowest) {
        return;
    }

    int first = s->has_last_symbol ? (s->last_symbol + 1) : newest_settled;
    if (first < memory->symbol_lowest) {
        first = memory->symbol_lowest;
    }

    for (int symbol_num = first; symbol_num <= newest_settled; symbol_num++) {
        if (s->symbols_done >= analysis->target_symbols) {
            break;
        }

        const int age = memory->symbol_highest - symbol_num;
        const size_t idx =
            (memory->index_symbol_highest + memory->max_symbols - (size_t)age) % memory->max_symbols;
        if (memory->symbols[idx] == NULL) {
            continue;
        }
        if (memory->row_symbol_numbers != NULL && memory->row_symbol_numbers[idx] != symbol_num) {
            continue;  // evicted or never written
        }

        measure_symbol(s, memory->symbols[idx]);
        s->symbols_done++;
    }

    s->last_symbol = newest_settled;
    s->has_last_symbol = 1;
}

int fh_iq_analysis_is_complete(const fh_iq_analysis_t* analysis) {
    if (analysis == NULL) {
        return 0;
    }
    int any = 0;
    for (int i = 0; i < IQ_ANALYSIS_MAX_STREAMS; i++) {
        const fh_iq_stream_stats_t* s = &analysis->streams[i];
        if (!s->in_use) {
            continue;
        }
        any = 1;
        if (s->symbols_done < analysis->target_symbols) {
            return 0;
        }
    }
    return any;
}

static const char* direction_name(uint8_t direction) {
    return (direction == 0) ? "UL" : ((direction == 1) ? "DL" : "??");
}

void fh_iq_analysis_report(const fh_iq_analysis_t* analysis, FILE* out, const char* source_label) {
    if (analysis == NULL || out == NULL) {
        return;
    }

    fprintf(out, "\n========== IQ analysis (source: %s) ==========\n",
            source_label ? source_label : "unknown");
    fprintf(out, "Absolute scale is expected to differ between capture paths.\n");
    fprintf(out, "Compare the normalised rows: they should match if only scale differs.\n");

    for (int i = 0; i < IQ_ANALYSIS_MAX_STREAMS; i++) {
        const fh_iq_stream_stats_t* s = &analysis->streams[i];
        if (!s->in_use) {
            continue;
        }

        const uint64_t nonzero = s->sample_count - s->zero_count;
        fprintf(out, "\n-- eAxC 0x%04X %s --\n", s->eaxc_id, direction_name(s->data_direction));
        fprintf(out, "  symbols measured    : %d (target %d)\n", s->symbols_done, analysis->target_symbols);
        fprintf(out, "  samples             : %llu\n", (unsigned long long)s->sample_count);
        if (s->sample_count == 0 || nonzero == 0) {
            fprintf(out, "  (no non-zero samples collected)\n");
            continue;
        }

        const double zero_pct = 100.0 * (double)s->zero_count / (double)s->sample_count;
        const double mean = s->sum_magnitude / (double)nonzero;
        const double rms = sqrt(s->sum_magnitude_sq / (double)nonzero);

        fprintf(out, "  zero samples        : %.2f%%  (unallocated REs / TDD gaps)\n", zero_pct);
        fprintf(out, "  magnitude min       : %.6g\n", (double)s->min_nonzero_magnitude);
        fprintf(out, "  magnitude max       : %.6g\n", (double)s->max_magnitude);
        fprintf(out, "  magnitude mean      : %.6g\n", mean);
        fprintf(out, "  magnitude rms       : %.6g\n", rms);
        fprintf(out, "  smallest |I| or |Q| : %.6g  (quantisation step hint)\n",
                (double)s->min_nonzero_component);
        if (s->min_nonzero_component > 0.0f && isfinite(s->min_nonzero_component)) {
            fprintf(out, "  max / smallest step : %.6g  (~2^%.1f, i.e. effective bits)\n",
                    (double)s->max_magnitude / (double)s->min_nonzero_component,
                    log2((double)s->max_magnitude / (double)s->min_nonzero_component));
        }

        // Everything below is scale-free, so the two capture paths should
        // agree here even though their absolute numbers do not.
        fprintf(out, "  --- normalised (divided by mean) ---\n");
        fprintf(out, "  rms / mean          : %.4f\n", rms / mean);
        fprintf(out, "  max / mean (crest)  : %.4f\n", (double)s->max_magnitude / mean);

        static const double percentiles[] = { 1.0, 10.0, 25.0, 50.0, 75.0, 90.0, 99.0, 99.9 };
        fprintf(out, "  percentiles / mean  :");
        for (size_t p = 0; p < sizeof(percentiles) / sizeof(percentiles[0]); p++) {
            const double value = histogram_percentile(s, nonzero, percentiles[p]);
            fprintf(out, "  p%.4g=%.4f", percentiles[p], value / mean);
        }
        fprintf(out, "\n");

        if (s->symbol_crest_count > 0) {
            fprintf(out, "  per-symbol crest    : mean %.4f  min %.4f  max %.4f  (over %llu symbols)\n",
                    s->sum_symbol_crest / (double)s->symbol_crest_count,
                    s->min_symbol_crest,
                    s->max_symbol_crest,
                    (unsigned long long)s->symbol_crest_count);
            fprintf(out, "    lower and steadier = more uniform-looking symbols\n");
        }

        if (s->prb_spread_count > 0) {
            fprintf(out, "  per-PRB spread      : mean %.4f  max %.4f  (over %llu symbols)\n",
                    s->sum_prb_spread / (double)s->prb_spread_count,
                    s->max_prb_spread,
                    (unsigned long long)s->prb_spread_count);
            fprintf(out, "    loudest/quietest fully-occupied PRB within a symbol;\n");
            fprintf(out, "    a large gap versus the other path points at per-PRB scaling (BFP exponent)\n");
        }
    }

    fprintf(out, "\n========== end IQ analysis ==========\n\n");
    fflush(out);
}

// ---------------------------------------------------------------------------
// BFP exponent probe
// ---------------------------------------------------------------------------

#define IQ_PROBE_MAX_OFFSET 8

typedef struct {
    int enabled;
    uint64_t sections;
    uint64_t prbs;

    // Exponent nibble read at the offset the decoder uses today, and at one
    // byte later. Whichever is the real exponent field should cluster; the
    // other is mantissa data and should look flat.
    uint64_t exponent_current[16];
    uint64_t exponent_shifted[16];

    // Full first byte of the PRB area. If a udCompHdr is really sitting
    // there it is a fixed value - high nibble iqWidth, low nibble compMeth -
    // so one value should dominate completely.
    uint64_t first_byte[256];

    // Same nibbles again but only for the first PRB of each section, which is
    // the one the udCompHdr would displace.
    uint64_t first_prb_exponent_current[16];
    uint64_t first_prb_exponent_shifted[16];

    // Sections claiming more PRBs than the packet actually carries. Those
    // trailing PRBs decode from whatever follows the data, so they look like
    // noise no matter how well the header is parsed.
    uint64_t sections_truncated;
    uint64_t prbs_claimed;
    uint64_t prbs_missing;

    // Exponent per PRB position. If only some positions decode badly, their
    // pattern says why: trouble bunched at the end of a section points at the
    // stride or length, trouble spread evenly points at the data itself.
    uint64_t exponent_by_prb[IQ_ANALYSIS_PRBS][16];

    // Spread of exponents inside one section. A steady signal should span
    // only a value or two across the band; spanning the whole range means
    // neighbouring PRBs are being scaled by wildly different powers of two.
    uint64_t exponent_range_hist[17];

    // Same range measure split by stream, to show whether trouble belongs to
    // particular eAxC/direction combinations rather than the capture at large.
    struct {
        int in_use;
        unsigned short eaxc_id;
        unsigned char data_direction;
        uint64_t sections;
        uint64_t exponent_range_hist[17];
        uint64_t exponent[16];
        // Exponent nibbles read at each candidate distance from the section
        // header. Whichever distance makes them cluster is where this
        // stream's per-PRB exponents really sit.
        uint64_t exponent_by_offset[IQ_PROBE_MAX_OFFSET][16];
        int applied_offset;
        // The two bytes straight after the section header, per stream. If a
        // stream carries a udCompHdr these are fixed; what they contain says
        // what width and method that stream is really using.
        uint64_t header_byte0[256];
        uint64_t header_byte1[256];
        // A verbatim sample of one section's opening bytes, so the layout can
        // be read directly instead of inferred from statistics.
        unsigned char sample[64];
        int sample_len;
        // A real udCompParam byte has a zero reserved (high) nibble. Sweeping
        // stride and offset for the combination where that holds locates the
        // per-PRB exponents without assuming the layout.
        uint64_t stride_zero_hi[40][4];
        uint64_t stride_total[40][4];
    } per_stream[IQ_ANALYSIS_MAX_STREAMS];
} fh_bfp_probe_t;

static fh_bfp_probe_t g_bfp_probe;

void fh_iq_bfp_probe_enable(int enabled) {
    memset(&g_bfp_probe, 0, sizeof(g_bfp_probe));
    g_bfp_probe.enabled = enabled ? 1 : 0;
}

void fh_iq_bfp_probe_note_section(unsigned short eaxc_id,
                                  unsigned char data_direction,
                                  const unsigned char* section_data,
                                  int applied_offset,
                                  int num_prbu,
                                  int prb_stride,
                                  int available_bytes) {
    if (!g_bfp_probe.enabled || section_data == NULL || num_prbu <= 0 || prb_stride <= 0) {
        return;
    }
    const unsigned char* prb_area = section_data + applied_offset;
    available_bytes -= applied_offset;
    if (available_bytes <= 0) {
        return;
    }

    int stream_slot = -1;
    for (int i = 0; i < IQ_ANALYSIS_MAX_STREAMS; i++) {
        if (g_bfp_probe.per_stream[i].in_use &&
            g_bfp_probe.per_stream[i].eaxc_id == eaxc_id &&
            g_bfp_probe.per_stream[i].data_direction == data_direction) {
            stream_slot = i;
            break;
        }
        if (!g_bfp_probe.per_stream[i].in_use) {
            g_bfp_probe.per_stream[i].in_use = 1;
            g_bfp_probe.per_stream[i].eaxc_id = eaxc_id;
            g_bfp_probe.per_stream[i].data_direction = data_direction;
            stream_slot = i;
            break;
        }
    }

    g_bfp_probe.sections++;
    g_bfp_probe.first_byte[prb_area[0]]++;
    g_bfp_probe.prbs_claimed += (uint64_t)num_prbu;

    if (stream_slot >= 0) {
        g_bfp_probe.per_stream[stream_slot].applied_offset = applied_offset;
        g_bfp_probe.per_stream[stream_slot].header_byte0[section_data[0]]++;
        g_bfp_probe.per_stream[stream_slot].header_byte1[section_data[1]]++;
        if (g_bfp_probe.per_stream[stream_slot].sample_len == 0) {
            int n = available_bytes + applied_offset;
            if (n > 64) {
                n = 64;
            }
            for (int b = 0; b < n; b++) {
                g_bfp_probe.per_stream[stream_slot].sample[b] = section_data[b];
            }
            g_bfp_probe.per_stream[stream_slot].sample_len = n;
        }
        // Sweep the candidate distances on a sample of PRBs; the winner is
        // read off the concentration in the report.
        for (int off = 0; off < IQ_PROBE_MAX_OFFSET; off++) {
            for (int prb = 0; prb < num_prbu; prb += 4) {
                const int at = off + prb * prb_stride;
                if (at >= available_bytes + applied_offset) {
                    break;
                }
                g_bfp_probe.per_stream[stream_slot].exponent_by_offset[off][section_data[at] & 0x0Fu]++;
            }
        }
    }

    if (stream_slot >= 0) {
        const int total_len = available_bytes + applied_offset;
        for (int si = 0; si < 40; si++) {
            const int cand_stride = 20 + si;  // 20..59 covers 8..16-bit, with and without an exponent byte
            for (int off = 0; off < 4; off++) {
                for (int prb = 0; prb < 60; prb++) {
                    const int at = off + prb * cand_stride;
                    if (at >= total_len) {
                        break;
                    }
                    g_bfp_probe.per_stream[stream_slot].stride_total[si][off]++;
                    if ((section_data[at] & 0xF0u) == 0) {
                        g_bfp_probe.per_stream[stream_slot].stride_zero_hi[si][off]++;
                    }
                }
            }
        }
    }

    int prbs_measured = 0;
    unsigned int section_min_exp = 15;
    unsigned int section_max_exp = 0;
    for (int prb = 0; prb < num_prbu; prb++) {
        const int offset = prb * prb_stride;
        if (offset + prb_stride > available_bytes) {
            break;  // the rest of this section was never captured
        }
        prbs_measured++;

        const unsigned int current = prb_area[offset] & 0x0Fu;
        const unsigned int shifted = prb_area[offset + 1] & 0x0Fu;

        g_bfp_probe.exponent_current[current]++;
        g_bfp_probe.exponent_shifted[shifted]++;
        if (prb < IQ_ANALYSIS_PRBS) {
            g_bfp_probe.exponent_by_prb[prb][current]++;
        }
        if (stream_slot >= 0) {
            g_bfp_probe.per_stream[stream_slot].exponent[current]++;
        }
        if (current < section_min_exp) {
            section_min_exp = current;
        }
        if (current > section_max_exp) {
            section_max_exp = current;
        }
        if (prb == 0) {
            g_bfp_probe.first_prb_exponent_current[current]++;
            g_bfp_probe.first_prb_exponent_shifted[shifted]++;
        }
        g_bfp_probe.prbs++;
    }

    if (prbs_measured > 0 && section_max_exp >= section_min_exp) {
        const unsigned int range = section_max_exp - section_min_exp;
        g_bfp_probe.exponent_range_hist[range]++;
        if (stream_slot >= 0) {
            g_bfp_probe.per_stream[stream_slot].sections++;
            g_bfp_probe.per_stream[stream_slot].exponent_range_hist[range]++;
        }
    }

    if (prbs_measured < num_prbu) {
        g_bfp_probe.sections_truncated++;
        g_bfp_probe.prbs_missing += (uint64_t)(num_prbu - prbs_measured);
    }
}

// Fraction of samples landing in the most populated bin. Near 1 means the
// values cluster on one exponent; near 1/16 means they are spread evenly,
// which is what mantissa bits read as an exponent look like.
static double concentration(const uint64_t bins[16], uint64_t total) {
    if (total == 0) {
        return 0.0;
    }
    uint64_t best = 0;
    for (int i = 0; i < 16; i++) {
        if (bins[i] > best) {
            best = bins[i];
        }
    }
    return (double)best / (double)total;
}

static void print_exponent_row(FILE* out, const char* label, const uint64_t bins[16], uint64_t total) {
    fprintf(out, "  %-18s:", label);
    for (int i = 0; i < 16; i++) {
        const double pct = (total > 0) ? (100.0 * (double)bins[i] / (double)total) : 0.0;
        fprintf(out, " %5.1f", pct);
    }
    fprintf(out, "   concentration %.3f\n", concentration(bins, total));
}

void fh_iq_bfp_probe_report(FILE* out) {
    if (out == NULL || !g_bfp_probe.enabled) {
        return;
    }

    fprintf(out, "\n========== BFP exponent probe ==========\n");
    fprintf(out, "sections seen: %llu   PRBs seen: %llu\n",
            (unsigned long long)g_bfp_probe.sections,
            (unsigned long long)g_bfp_probe.prbs);

    if (g_bfp_probe.prbs == 0) {
        fprintf(out, "(no BFP sections were decoded - is the capture using BFP?)\n");
        fprintf(out, "========== end BFP exponent probe ==========\n\n");
        fflush(out);
        return;
    }

    fprintf(out, "\nExponent distribution, %% per value 0..15:\n");
    fprintf(out, "  %-18s: %s\n", "value", "    0     1     2     3     4     5     6     7"
                                            "     8     9    10    11    12    13    14    15");
    print_exponent_row(out, "as decoded now", g_bfp_probe.exponent_current, g_bfp_probe.prbs);
    print_exponent_row(out, "shifted +1 byte", g_bfp_probe.exponent_shifted, g_bfp_probe.prbs);

    fprintf(out, "\nFirst PRB of each section only:\n");
    print_exponent_row(out, "as decoded now", g_bfp_probe.first_prb_exponent_current, g_bfp_probe.sections);
    print_exponent_row(out, "shifted +1 byte", g_bfp_probe.first_prb_exponent_shifted, g_bfp_probe.sections);

    fprintf(out, "\nSection fit against what the packet actually carries:\n");
    fprintf(out, "  sections short of their own numPrbu : %llu of %llu (%.2f%%)\n",
            (unsigned long long)g_bfp_probe.sections_truncated,
            (unsigned long long)g_bfp_probe.sections,
            (g_bfp_probe.sections > 0)
                ? 100.0 * (double)g_bfp_probe.sections_truncated / (double)g_bfp_probe.sections
                : 0.0);
    fprintf(out, "  PRBs claimed but not captured        : %llu of %llu (%.2f%%)\n",
            (unsigned long long)g_bfp_probe.prbs_missing,
            (unsigned long long)g_bfp_probe.prbs_claimed,
            (g_bfp_probe.prbs_claimed > 0)
                ? 100.0 * (double)g_bfp_probe.prbs_missing / (double)g_bfp_probe.prbs_claimed
                : 0.0);
    fprintf(out, "  (these decode from whatever follows the data, i.e. noise)\n");

    fprintf(out, "\nFirst byte of the PRB area (where a udCompHdr would sit):\n");
    for (int value = 0; value < 256; value++) {
        if (g_bfp_probe.first_byte[value] == 0) {
            continue;
        }
        const double pct = 100.0 * (double)g_bfp_probe.first_byte[value] / (double)g_bfp_probe.sections;
        if (pct < 1.0) {
            continue;  // keep the list readable
        }
        fprintf(out, "  0x%02X : %5.1f%%   (as udCompHdr: iqWidth=%d, compMeth=%d)\n",
                value, pct, (value >> 4) & 0x0F, value & 0x0F);
    }

    fprintf(out, "\nExponent range within a single section (max - min across its PRBs):\n");
    {
        uint64_t total = 0;
        for (int r = 0; r <= 16; r++) {
            total += g_bfp_probe.exponent_range_hist[r];
        }
        for (int r = 0; r <= 15; r++) {
            if (g_bfp_probe.exponent_range_hist[r] == 0) {
                continue;
            }
            fprintf(out, "  range %2d : %5.1f%%\n", r,
                    (total > 0) ? 100.0 * (double)g_bfp_probe.exponent_range_hist[r] / (double)total : 0.0);
        }
        fprintf(out, "  (a steady signal should sit at 1-3; 12+ means neighbouring PRBs\n");
        fprintf(out, "   differ by four orders of magnitude, which no real spectrum does)\n");
    }

    fprintf(out, "\nExponent range per stream (%% of that stream's sections):\n");
    for (int i = 0; i < IQ_ANALYSIS_MAX_STREAMS; i++) {
        if (!g_bfp_probe.per_stream[i].in_use || g_bfp_probe.per_stream[i].sections == 0) {
            continue;
        }
        const uint64_t total = g_bfp_probe.per_stream[i].sections;
        double wide = 0.0;
        for (int r = 4; r <= 16; r++) {
            wide += (double)g_bfp_probe.per_stream[i].exponent_range_hist[r];
        }
        uint64_t exp_total = 0;
        for (int e = 0; e < 16; e++) {
            exp_total += g_bfp_probe.per_stream[i].exponent[e];
        }
        fprintf(out, "  eAxC 0x%04X %s: %8llu sections   range<=1: %5.1f%%   range>=4: %5.1f%%   exp concentration %.3f\n",
                g_bfp_probe.per_stream[i].eaxc_id,
                direction_name(g_bfp_probe.per_stream[i].data_direction),
                (unsigned long long)total,
                100.0 * (double)(g_bfp_probe.per_stream[i].exponent_range_hist[0] +
                                 g_bfp_probe.per_stream[i].exponent_range_hist[1]) / (double)total,
                100.0 * wide / (double)total,
                concentration(g_bfp_probe.per_stream[i].exponent, exp_total));
    }

    fprintf(out, "\nWhere each stream's exponents actually sit (concentration by distance\n");
    fprintf(out, "from the section header; the highest value is the real offset):\n");
    fprintf(out, "  %-18s  %s\n", "stream", " off0  off1  off2  off3  off4  off5  off6  off7   using");
    for (int i = 0; i < IQ_ANALYSIS_MAX_STREAMS; i++) {
        if (!g_bfp_probe.per_stream[i].in_use || g_bfp_probe.per_stream[i].sections == 0) {
            continue;
        }
        char name[32];
        snprintf(name, sizeof(name), "eAxC 0x%04X %s",
                 g_bfp_probe.per_stream[i].eaxc_id,
                 direction_name(g_bfp_probe.per_stream[i].data_direction));
        fprintf(out, "  %-18s:", name);
        int best_off = 0;
        double best_conc = -1.0;
        for (int off = 0; off < IQ_PROBE_MAX_OFFSET; off++) {
            uint64_t total = 0;
            for (int e = 0; e < 16; e++) {
                total += g_bfp_probe.per_stream[i].exponent_by_offset[off][e];
            }
            const double conc = concentration(g_bfp_probe.per_stream[i].exponent_by_offset[off], total);
            if (conc > best_conc) {
                best_conc = conc;
                best_off = off;
            }
            fprintf(out, " %5.3f", conc);
        }
        fprintf(out, "   %d  -> best %d\n", g_bfp_probe.per_stream[i].applied_offset, best_off);
    }

    fprintf(out, "\nThe two bytes right after each section header, per stream:\n");
    for (int i = 0; i < IQ_ANALYSIS_MAX_STREAMS; i++) {
        if (!g_bfp_probe.per_stream[i].in_use || g_bfp_probe.per_stream[i].sections == 0) {
            continue;
        }
        int top0 = 0, top1 = 0;
        uint64_t best0 = 0, best1 = 0, tot = 0;
        for (int v = 0; v < 256; v++) {
            if (g_bfp_probe.per_stream[i].header_byte0[v] > best0) {
                best0 = g_bfp_probe.per_stream[i].header_byte0[v];
                top0 = v;
            }
            if (g_bfp_probe.per_stream[i].header_byte1[v] > best1) {
                best1 = g_bfp_probe.per_stream[i].header_byte1[v];
                top1 = v;
            }
            tot += g_bfp_probe.per_stream[i].header_byte0[v];
        }
        char name[32];
        snprintf(name, sizeof(name), "eAxC 0x%04X %s",
                 g_bfp_probe.per_stream[i].eaxc_id,
                 direction_name(g_bfp_probe.per_stream[i].data_direction));
        fprintf(out, "  %-18s: byte0 0x%02X (%.1f%%)  byte1 0x%02X (%.1f%%)   "
                     "byte0 as udCompHdr: iqWidth=%d compMeth=%d\n",
                name, top0, (tot > 0) ? 100.0 * (double)best0 / (double)tot : 0.0,
                top1, (tot > 0) ? 100.0 * (double)best1 / (double)tot : 0.0,
                (top0 >> 4) & 0x0F, top0 & 0x0F);
    }

    fprintf(out, "\nFirst bytes of one section per stream (after the 4-byte section header).\n");
    fprintf(out, "Expected for BFP9: 91 00 then, every 28 bytes, an exponent byte:\n");
    for (int i = 0; i < IQ_ANALYSIS_MAX_STREAMS; i++) {
        if (!g_bfp_probe.per_stream[i].in_use || g_bfp_probe.per_stream[i].sample_len == 0) {
            continue;
        }
        char name[32];
        snprintf(name, sizeof(name), "eAxC 0x%04X %s",
                 g_bfp_probe.per_stream[i].eaxc_id,
                 direction_name(g_bfp_probe.per_stream[i].data_direction));
        fprintf(out, "  %-18s:", name);
        for (int b = 0; b < g_bfp_probe.per_stream[i].sample_len; b++) {
            if (b == 2 || b == 30 || b == 58) {
                fprintf(out, " |");  // where exponent bytes should fall
            }
            fprintf(out, " %02X", g_bfp_probe.per_stream[i].sample[b]);
        }
        fprintf(out, "\n");
    }

    fprintf(out, "\nLocating the per-PRB exponent byte without assuming a layout.\n");
    fprintf(out, "A udCompParam has a zero reserved nibble, so the stride/offset where that\n");
    fprintf(out, "holds for every PRB is the real one (chance alone gives about 6%%):\n");
    for (int i = 0; i < IQ_ANALYSIS_MAX_STREAMS; i++) {
        if (!g_bfp_probe.per_stream[i].in_use || g_bfp_probe.per_stream[i].sections == 0) {
            continue;
        }
        int best_stride = 0, best_off = 0;
        double best_frac = -1.0;
        for (int si = 0; si < 40; si++) {
            for (int off = 0; off < 4; off++) {
                const uint64_t tot = g_bfp_probe.per_stream[i].stride_total[si][off];
                if (tot < 100) {
                    continue;
                }
                const double frac = (double)g_bfp_probe.per_stream[i].stride_zero_hi[si][off] / (double)tot;
                if (frac > best_frac) {
                    best_frac = frac;
                    best_stride = 20 + si;
                    best_off = off;
                }
            }
        }
        char name[32];
        snprintf(name, sizeof(name), "eAxC 0x%04X %s",
                 g_bfp_probe.per_stream[i].eaxc_id,
                 direction_name(g_bfp_probe.per_stream[i].data_direction));
        fprintf(out, "  %-18s: best stride %2d offset %d -> %.1f%% zero reserved nibble\n",
                name, best_stride, best_off, 100.0 * best_frac);
    }

    fprintf(out, "\nPer-PRB-position concentration (share landing on that position's most\n");
    fprintf(out, "common exponent; low means that position decodes as noise):\n");
    {
        int worst_listed = 0;
        double sum_conc = 0.0;
        int counted = 0;
        for (int prb = 0; prb < IQ_ANALYSIS_PRBS; prb++) {
            uint64_t total = 0;
            for (int e = 0; e < 16; e++) {
                total += g_bfp_probe.exponent_by_prb[prb][e];
            }
            if (total == 0) {
                continue;
            }
            const double conc = concentration(g_bfp_probe.exponent_by_prb[prb], total);
            sum_conc += conc;
            counted++;
            if (conc < 0.5 && worst_listed < 24) {
                if (worst_listed == 0) {
                    fprintf(out, "  positions below 0.50:");
                }
                fprintf(out, " %d(%.2f)", prb, conc);
                worst_listed++;
            }
        }
        if (worst_listed > 0) {
            fprintf(out, "%s\n", (worst_listed >= 24) ? " ..." : "");
        } else {
            fprintf(out, "  none below 0.50\n");
        }
        if (counted > 0) {
            fprintf(out, "  mean concentration across positions: %.3f\n", sum_conc / (double)counted);
        }
    }

    fprintf(out, "\nReading: a real exponent field tracks the signal and clusters, so the\n");
    fprintf(out, "reading with the higher concentration is the one landing on the exponent.\n");
    fprintf(out, "A flat ~6.3%% across all sixteen values is mantissa data, not an exponent.\n");
    fprintf(out, "========== end BFP exponent probe ==========\n\n");
    fflush(out);
}
