#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <stdbool.h>

#ifdef __cplusplus
#include <complex>
typedef std::complex<float> fh_complex_t;
#else
#include <complex.h>
typedef float complex fh_complex_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fh_section {
    uint16_t eaxc_id;
    uint8_t data_direction;
    uint8_t frame_id;
    uint8_t subframe_id;
    uint8_t slot_id;
    uint8_t start_symbol_id;
    uint16_t start_prbu;
    uint16_t num_prbu;
    /// 3GPP numerology (mu) of this section, e.g. 1 => 30 kHz SCS. Only populated when the source
    /// capture reports it directly (currently the ocudu PHY tap); see has_numerology.
    uint8_t numerology;
    /// Whether numerology was reported by the source capture. Ethernet/eCPRI O-RAN frames don't
    /// carry numerology in the U-Plane header, so this stays 0 for those.
    uint8_t has_numerology;
    /// Resource elements for this section, indexed 0..3275 across the 273-PRB grid.
    ///
    /// Normalized to roughly [-1, +1) regardless of which capture produced it, so
    /// magnitudes are comparable between sources and no consumer needs to know
    /// where a sample came from. The two paths reach that scale differently:
    /// the Ethernet/eCPRI path unpacks fixed-point components and applies 2^-15
    /// (BFP: mantissa * 2^exponent restores the 16-bit sample, which then gets
    /// the same 2^-15), while the PHY tap carries bfloat16 values that are
    /// already normalized and are only widened to float.
    ///
    /// Treating the PHY tap's samples as fixed point instead is silent and
    /// destructive - see bf16_to_float in c_fronthaul_decoder.c - so anything
    /// adding a capture source has to land on this scale too.
    fh_complex_t *iq_data;
} fh_section_t;

typedef struct fh_decoded_packet {
    struct timespec timestamp;
    char ingress_if[32];
    size_t caplen;
    int section_count;
    fh_section_t *sections;
} fh_decoded_packet_t;

typedef struct fh_packet {
    struct timespec timestamp;
    char ingress_if[32];
    uint8_t* data;
    size_t caplen;
    size_t len;
    void (*release_cb)(void* release_ctx, struct fh_packet* pkt);
    void* release_ctx;
} fh_packet_t;

int fh_decode_packet(const fh_packet_t* raw_pkt, fh_decoded_packet_t* decoded_pkt, bool ud_comp_hdr_present, int ud_iq_width, int ud_iq_width_decompressed, const char *ud_comp_method);

void fh_decoded_packet_free(fh_decoded_packet_t *pkt);

#ifdef __cplusplus
}
#endif
