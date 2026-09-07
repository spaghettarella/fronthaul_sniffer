#pragma once

// Collects distribution statistics on decoded IQ so two capture paths can be
// compared directly. The phy tap and the network/loopback path reach the
// visualiser through different decode chains - the loopback one goes through
// BFP decompression, the phy tap does not - and the results look different on
// screen. Absolute scale is expected to differ; what matters is whether the
// *shape* of the distribution matches once that scale is divided out, which
// is what these statistics are built to answer.
//
// Symbols are read back from the assembled memory ring rather than from
// individual sections, so a symbol is measured as one complete row no matter
// how many packets or sections it arrived in. Collection stops once the
// per-stream target is met, keeping the cost bounded and both runs
// comparable.

#include "core/c_oran_memory.h"

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fh_iq_analysis fh_iq_analysis_t;

// target_symbols_per_stream: how many complete symbols to measure per stream
// before that stream is considered done. Returns NULL on allocation failure.
fh_iq_analysis_t* fh_iq_analysis_create(int target_symbols_per_stream);
void fh_iq_analysis_destroy(fh_iq_analysis_t* analysis);

// Measures whatever complete symbols have appeared in this stream since the
// last call. Safe to call on every save; it returns immediately once the
// stream has met its target. Call with the memory lock held, as it reads the
// ring directly.
void fh_iq_analysis_observe(fh_iq_analysis_t* analysis,
                            uint16_t eaxc_id,
                            uint8_t data_direction,
                            const fh_oran_memory_t* memory);

// Non-zero once every stream seen so far has met its target.
int fh_iq_analysis_is_complete(const fh_iq_analysis_t* analysis);

// source_label identifies the capture path in the output, e.g. "phy-tap" or
// "lo", so two runs can be told apart when compared side by side.
void fh_iq_analysis_report(const fh_iq_analysis_t* analysis, FILE* out, const char* source_label);

// --- BFP exponent probe ---
// Looks at where the per-PRB exponents are being read from on the network
// path. A block-floating-point exponent should track the signal level, so on
// a steady signal the exponents across a symbol should cluster tightly; a
// flat spread over all sixteen values instead means the byte being read is
// not the exponent at all.
//
// It reads the same bytes two ways: at the offset the decoder currently
// uses, and one byte later. O-RAN places a one-byte udCompHdr after the
// section header when compression is in use, which the decoder does not
// currently skip, so if that is the problem the shifted reading is the one
// that will look like a real exponent.
//
// A single global instance, since the decoder has no context to carry. Inert
// until enabled, and counters are plain increments - with several decode
// threads the totals may be slightly off, which does not affect the shape.
void fh_iq_bfp_probe_enable(int enabled);

// prb_area: first byte of the PRB data. available_bytes is how many bytes
// really remain in the packet from there, so the probe can both stay in
// bounds and report how often a section claims more PRBs than were actually
// captured - reading past that point yields padding or neighbouring memory,
// which decodes as noise.
// section_data: first byte after the 4-byte section header, before any
// compression header is skipped. applied_offset is how many bytes the decoder
// actually skipped, so the report can say whether that choice was right.
void fh_iq_bfp_probe_note_section(unsigned short eaxc_id,
                                  unsigned char data_direction,
                                  const unsigned char* section_data,
                                  int applied_offset,
                                  int num_prbu,
                                  int prb_stride,
                                  int available_bytes);

void fh_iq_bfp_probe_report(FILE* out);

#ifdef __cplusplus
}
#endif
