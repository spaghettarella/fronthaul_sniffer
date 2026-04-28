#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fh_decoded_packet {
    struct timespec timestamp;
    char ingress_if[32];
    size_t caplen;
    uint16_t eaxc_id;
    uint8_t data_direction;
    uint8_t frame_id;
    uint8_t subframe_id;
    uint8_t slot_id;
    uint8_t start_symbol_id;
    uint16_t start_prbu;
    uint16_t num_prbu;
    float complex iq_data[];
} fh_decoded_packet_t;

typedef struct fh_packet {
    struct timespec timestamp;
    char ingress_if[32];
    uint8_t* data;
    size_t caplen;
    size_t len;
} fh_packet_t;

int fh_decode_packet(const fh_packet_t* raw_pkt, fh_decoded_packet_t* decoded_pkt, bool ud_comp_hdr_present, int ud_iq_width, int ud_iq_width_decompressed, char *ud_comp_method);

#ifdef __cplusplus
}
#endif
