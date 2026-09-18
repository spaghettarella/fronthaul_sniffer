#include "c_fronthaul_decoder.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <math.h>
#include <complex.h>
#include <arpa/inet.h>
#include <net/ethernet.h>

#include "bfp_decompress.h"
#include "analysis/c_iq_analysis.h"

enum {
    FH_PHY_TAP_PACKET_MAGIC = 0x46505450u,
    FH_PHY_TAP_PACKET_VERSION = 2,
};

struct fh_phy_tap_packet_header {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t eaxc_id;
    uint32_t frame_id;
    uint32_t subframe_id;
    uint32_t slot_id;
    uint32_t symbol_id;
    uint32_t numerology;
    uint32_t stream_index;
    uint32_t sample_count;
    uint32_t start_prbu;
    uint32_t num_prbu;
    /// O-RAN data direction of the tapped samples. 0 = uplink, 1 = downlink.
    uint8_t data_direction;
    /// Bit width of each I or Q component in the payload.
    uint8_t iq_bit_width;
    /// IQ compression method of the payload (matches ocudu::ofh::compression_type). 0 = none.
    uint8_t compression_method;
    uint8_t reserved2;
    uint64_t timestamp_ns;
} __attribute__((packed));

// This layout is written by fh_phy_tap_packet_header_t in
// c_capture_ocudu_phy_tap.c and read here, from two independent definitions
// with no shared header between them. The size check is the only thing that
// notices if one side gains or resizes a field: everything downstream would
// otherwise read the payload at the wrong offset and still look plausible.
_Static_assert(sizeof(struct fh_phy_tap_packet_header) == 60,
               "phy tap packet header layout changed - update the matching "
               "definition in c_capture_ocudu_phy_tap.c");

// The phy tap queue stores resource-grid samples as ocudu cbf16_t: a pair of
// bfloat16 values, i.e. the top 16 bits of an IEEE-754 float32 (1 sign, 8
// exponent, 7 mantissa). They are NOT Q15 fixed point, even though the record
// advertises iq_bit_width=16 and compression_method=0 - those describe the
// component width and the absence of O-RAN compression, not an integer format.
//
// Reading the bit pattern as an int16 is silent and destructive: the exponent
// lands in the high bits, so every sample collapses into a narrow band around
// +/-16000 regardless of its real amplitude, and the true dynamic range
// disappears. Widening back to float32 is exact, no scaling required - the
// values are already normalized to roughly [-1, +1), matching what the network
// U-Plane path produces after its 2^-15 scaling.
static inline float bf16_to_float(uint16_t bits) {
    uint32_t widened = (uint32_t)bits << 16;
    float out;
    memcpy(&out, &widened, sizeof(out));
    return out;
}

struct ecpri_common_header {
    uint8_t concat_ind : 1;
    uint8_t reserved : 3;
    uint8_t protocol_revision : 4;
    uint8_t message_type;
    uint16_t payload_size;
    uint16_t eaxc_id;
    uint8_t sequence_id;
    uint8_t subsequence_id : 7;
    uint8_t e_bit : 1;
} __attribute__((packed));

struct oran_rtc_radio_app_header {
    uint8_t filter_index : 4;
    uint8_t payload_version : 3;
    uint8_t data_direction : 1;
    uint8_t frame_id;
    uint8_t slot_id_first: 4;
    uint8_t subframe_id : 4;
    uint8_t start_symbol_id : 6;
    uint8_t slot_id_last : 2;
    uint8_t number_of_sections;
    uint8_t section_type;
} __attribute__((packed));

struct oran_iq_radio_app_header {
    uint8_t filter_index : 4;
    uint8_t payload_version : 3;
    uint8_t data_direction : 1;
    uint8_t frame_id;
    uint8_t slot_id_first: 4;
    uint8_t subframe_id : 4;
    uint8_t start_symbol_id : 6;
    uint8_t slot_id_last : 2;
} __attribute__((packed));

struct oran_iq_section_header {
    uint8_t section_id_first;
    uint8_t start_prbu_first : 2;
    uint8_t sym_inc : 1;
    uint8_t rb : 1;
    uint8_t section_id_last : 4;
    uint8_t start_prbu_last;
    uint8_t num_prbu;
} __attribute__((packed));

int extract_iq_samples(const char *ptr_prb, int bit_width, float i_samples[12], float q_samples[12]);

// When the compression parameters travel in band, an O-RAN U-Plane section
// header is followed by a udCompHdr octet - high nibble udIqWidth, low nibble
// udCompMeth - and then a reserved octet, so the PRB data starts two bytes
// later. Whether a sender includes them is not something the receiver can
// assume, and getting it wrong is silent and destructive: every PRB is then
// read at the wrong offset, the per-PRB exponent comes out of mantissa data,
// and each PRB gets scaled by an arbitrary power of two.
//
// Returns the number of bytes to skip: 2 when present, 0 when not.
//
// The caller's flag forces "present" when set. Otherwise the pair is checked
// against what the configured settings imply - a fixed udCompHdr value and a
// zero reserved octet - and has to match repeatedly before it is trusted, so
// a chance collision with mantissa data cannot switch parsing over.
//
// Tracked per eAxC and direction rather than globally: senders need not agree
// with each other, and a single shared decision would apply one stream's
// layout to every other one. The decision latches per stream so parsing
// cannot flip mid-capture. The table is written without locking, so with
// several decode threads two of them may race on the same stream's streak;
// that can only delay the latch, never corrupt the decision.
#define UDCOMPHDR_MAX_STREAMS 16

static int section_udcomphdr_length(const uint8_t* candidate, bool flag_says_present,
                                    int configured_iq_width, int configured_comp_code,
                                    uint16_t eaxc_id, uint8_t data_direction) {
    if (flag_says_present) {
        return 2;
    }

    static struct {
        int in_use;
        uint16_t eaxc_id;
        uint8_t data_direction;
        int latched;
        int streak;
    } streams[UDCOMPHDR_MAX_STREAMS];

    int slot = -1;
    for (int i = 0; i < UDCOMPHDR_MAX_STREAMS; i++) {
        if (streams[i].in_use && streams[i].eaxc_id == eaxc_id &&
            streams[i].data_direction == data_direction) {
            slot = i;
            break;
        }
        if (!streams[i].in_use) {
            streams[i].in_use = 1;
            streams[i].eaxc_id = eaxc_id;
            streams[i].data_direction = data_direction;
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return 0;  // more streams than tracked; leave the layout unchanged
    }
    if (streams[slot].latched) {
        return 2;
    }

    // udIqWidth is encoded modulo 16, so a 16-bit width travels as 0.
    const int width_nibble = (configured_iq_width >= 16) ? 0 : (configured_iq_width & 0x0F);
    const uint8_t expected = (uint8_t)((width_nibble << 4) | (configured_comp_code & 0x0F));

    if (candidate[0] == expected && candidate[1] == 0x00) {
        streams[slot].streak++;
        if (streams[slot].streak >= 8) {
            streams[slot].latched = 1;
            fprintf(stderr,
                    "Note: udCompHdr 0x%02X + reserved octet detected on eAxC 0x%04X %s "
                    "(iqWidth=%d, compMeth=%d); parsing them even though "
                    "--ud-comp-hdr-present was not set\n",
                    expected, eaxc_id, (data_direction == 0) ? "UL" : "DL",
                    configured_iq_width, configured_comp_code & 0x0F);
            return 2;
        }
    } else {
        streams[slot].streak = 0;
    }
    return 0;
}

int fh_decode_packet(const fh_packet_t* raw_pkt, fh_decoded_packet_t* decoded_pkt, bool ud_comp_hdr_present, int ud_iq_width, int ud_iq_width_decompressed, const char *ud_comp_method) {
    if (raw_pkt == NULL || decoded_pkt == NULL) {
        return -1;
    }

    memset(decoded_pkt, 0, sizeof(*decoded_pkt));
    decoded_pkt->timestamp = raw_pkt->timestamp;
    decoded_pkt->caplen = raw_pkt->caplen;
    memcpy(decoded_pkt->ingress_if, raw_pkt->ingress_if, sizeof(raw_pkt->ingress_if));

    decoded_pkt->section_count = 0;

    if (raw_pkt->data != NULL && raw_pkt->caplen > 0) {
        if (raw_pkt->caplen >= sizeof(struct fh_phy_tap_packet_header)) {
            const struct fh_phy_tap_packet_header* phy_tap = (const struct fh_phy_tap_packet_header*)raw_pkt->data;
            if (phy_tap->magic == FH_PHY_TAP_PACKET_MAGIC && phy_tap->version == FH_PHY_TAP_PACKET_VERSION) {
                const size_t iq_pairs = (size_t)phy_tap->sample_count;
                const size_t iq_bytes = iq_pairs * sizeof(uint16_t) * 2U;
                const size_t expected_bytes = sizeof(struct fh_phy_tap_packet_header) + iq_bytes;
                if (raw_pkt->caplen < expected_bytes) {
                    return -1;
                }
                if (phy_tap->compression_method != 0) {
                    // ocudu self-reports its own compression method per record (matches
                    // ocudu::ofh::compression_type; see fh_phy_tap_record_header_t in
                    // c_capture_ocudu_phy_tap.c) - trust that rather than the CLI/UI's
                    // --ud-comp-method, which describes the *fronthaul link's* compression,
                    // not ocudu's internal tap point.
                    //
                    // However, the phy tap shared-memory queue always sizes its IQ payload as
                    // a fixed sample_count * 4 bytes (2x uint16 per I/Q pair, see
                    // c_capture_ocudu_phy_tap.c), unlike the network U-Plane path's tightly
                    // bit-packed PRB layout, and fh_phy_tap_record_header_t has no dedicated
                    // per-PRB exponent field. The actual compressed wire layout (exponent
                    // placement, mantissa packing) is therefore not known here, so reject
                    // rather than guess at a layout that could silently produce wrong IQ
                    // values. Once the real layout is confirmed, this is where to call
                    // bfp_decompress_prb, mirroring the network U-Plane path below.
                    static int warned = 0;
                    if (!warned) {
                        fprintf(stderr,
                                "Warning: phy tap reports compression_method=%u (iq_bit_width=%u); "
                                "decoding this format is not yet implemented, dropping packet(s)\n",
                                (unsigned)phy_tap->compression_method, (unsigned)phy_tap->iq_bit_width);
                        warned = 1;
                    }
                    return -1;
                }
                if (phy_tap->iq_bit_width != 16) {
                    // Uncompressed phy tap samples are only supported at their native
                    // bfloat16 width; a different width is not a bfloat16 and would need
                    // its own unpacking logic.
                    return -1;
                }

                decoded_pkt->sections = calloc(1, sizeof(*decoded_pkt->sections));
                if (decoded_pkt->sections == NULL) {
                    return -1;
                }

                size_t prb_count = (size_t)phy_tap->num_prbu;
                if (prb_count == 0) {
                    prb_count = (iq_pairs + 11U) / 12U;
                    if (prb_count == 0) {
                        prb_count = 1;
                    }
                }
                if (prb_count > 273U) {
                    prb_count = 273U;
                }

                const size_t iq_count = prb_count * 12U;
                decoded_pkt->sections[0].iq_data = calloc(iq_count, sizeof(*decoded_pkt->sections[0].iq_data));
                if (decoded_pkt->sections[0].iq_data == NULL) {
                    free(decoded_pkt->sections);
                    decoded_pkt->sections = NULL;
                    return -1;
                }

                decoded_pkt->sections[0].eaxc_id = (uint16_t)(phy_tap->eaxc_id & 0xFFFFu);
                decoded_pkt->sections[0].data_direction = phy_tap->data_direction;
                decoded_pkt->sections[0].frame_id = (uint8_t)(phy_tap->frame_id & 0xFFu);
                decoded_pkt->sections[0].subframe_id = (uint8_t)(phy_tap->subframe_id & 0x0Fu);
                decoded_pkt->sections[0].slot_id = (uint8_t)(phy_tap->slot_id & 0x3Fu);
                decoded_pkt->sections[0].start_symbol_id = (uint8_t)(phy_tap->symbol_id & 0x3Fu);
                decoded_pkt->sections[0].start_prbu = (uint16_t)(phy_tap->start_prbu & 0x03FFu);
                decoded_pkt->sections[0].num_prbu = (uint16_t)prb_count;
                decoded_pkt->sections[0].numerology = (uint8_t)(phy_tap->numerology & 0xFFu);
                decoded_pkt->sections[0].has_numerology = 1;
                decoded_pkt->section_count = 1;

                const uint8_t* iq_payload = raw_pkt->data + sizeof(struct fh_phy_tap_packet_header);
                size_t samples_to_copy = iq_pairs;
                if (samples_to_copy > iq_count) {
                    samples_to_copy = iq_count;
                }

                for (size_t i = 0; i < samples_to_copy; ++i) {
                    const uint16_t i_raw = (uint16_t)((uint16_t)iq_payload[i * 4 + 0] | ((uint16_t)iq_payload[i * 4 + 1] << 8));
                    const uint16_t q_raw = (uint16_t)((uint16_t)iq_payload[i * 4 + 2] | ((uint16_t)iq_payload[i * 4 + 3] << 8));
                    decoded_pkt->sections[0].iq_data[i] = bf16_to_float(i_raw) + I * bf16_to_float(q_raw);
                }

                return 0;
            }
        }

        uint8_t *packet = raw_pkt->data;

        // Handle Ethernet header
        struct ether_header *ether_header = (struct ether_header *) packet;
        /*printf("\n----------------------------------------\n");
        printf("Packet received (%d bytes):\n", raw_pkt->caplen);
        printf("  Src MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
               ether_header->ether_shost[0], ether_header->ether_shost[1],
               ether_header->ether_shost[2], ether_header->ether_shost[3],
               ether_header->ether_shost[4], ether_header->ether_shost[5]);
        printf("  Dst MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
               ether_header->ether_dhost[0], ether_header->ether_dhost[1],
               ether_header->ether_dhost[2], ether_header->ether_dhost[3],
               ether_header->ether_dhost[4], ether_header->ether_dhost[5]);
        printf("  EtherType: 0x%04x (%s)\n",
               ntohs(ether_header->ether_type),
               ntohs(ether_header->ether_type) == ETHERTYPE_IP ? "IPv4" :
               ntohs(ether_header->ether_type) == ETHERTYPE_IPV6 ? "IPv6" :
               ntohs(ether_header->ether_type) == ETHERTYPE_ARP ? "ARP" : "Other");
        printf("  Payload length: %d bytes\n\n", (unsigned int)(raw_pkt->caplen - sizeof(struct ether_header)));*/

        /*printf("  Bytes after Ethernet header: ");
        for (int i = 0; i < 8; i++) {
            printf("%02x ", *(packet + sizeof(struct ether_header) + i));
        }
        printf("\n");*/

        // Handle VLAN tags
        const uint8_t *ptr = packet + sizeof(struct ether_header) - 2;
        uint16_t ethertype = ntohs(*(uint16_t *)ptr);
        int vlan_count = 0;

        // VLAN TPIDs to check
        while (ethertype == 0x8100 || ethertype == 0x88A8) {
            vlan_count++;
            ptr += 4;  // Skip VLAN tag (TPID + TCI)
            ethertype = ntohs(*(uint16_t *)ptr);
        }
        
        //printf("  VLAN tags skipped: %d\n", vlan_count);

        // Handle eCPRI
        if (ethertype == 0xAEFE) {  // eCPRI EtherType
            //printf("  eCPRI detected!\n");
            
            // eCPRI starts after Ethernet header (14 bytes)
            struct ecpri_common_header *ecpri = (struct ecpri_common_header *)(ptr + 2);

            /*printf("  RAW eCPRI Header: ");
            for (int i = 0; i < 8; i++) {
                printf("%02x ", ptr[i+2]);
            }
            printf("\n");
            
            // Print all fields with proper names and conversions
            printf("  Protocol Revision: %u\n", ecpri->protocol_revision);      // 4 bits → decimal
            printf("  Reserved: %u\n", ecpri->reserved);                       // 3 bits → decimal  
            printf("  Concat: %u\n", ecpri->concat_ind);                           // 1 bit → decimal
            printf("  Message Type: 0x%02x\n", ecpri->message_type);           // 8 bits → hex
            printf("  Payload Size: 0x%04x (%u bytes)\n", ntohs(ecpri->payload_size), ntohs(ecpri->payload_size));    // 16 bits → hex + decimal
            printf("  EAXC-ID: 0x%04x\n", ntohs(ecpri->eaxc_id));             // 16 bits → hex
            printf("  Sequence ID: 0x%02x\n", ecpri->sequence_id);             // 8 bits → hex
            printf("  E-bit: %u\n", ecpri->e_bit);                             // 1 bit → decimal
            printf("  Subsequence ID: %u\n", ecpri->subsequence_id);           // 7 bits → decimal*/
            
            // eCPRI payload starts after common header (8 bytes)
            const uint8_t *ecpri_payload = packet + sizeof(struct ether_header) + 8;
            /*printf("  eCPRI Payload (%u bytes): ", ntohs(ecpri->payload_size));
            for (int i = 0; i < ntohs(ecpri->payload_size) && i < 32; i++) {
                printf("%02x ", ecpri_payload[i]);
            }
            printf("\n\n");*/

            const uint8_t *ptr_oran = ptr + 2 + sizeof(struct ecpri_common_header);

            /*printf("  RAW ORAN Radio App Header: ");
            for (int i = 0; i < 6; i++) {
                printf("%02x ", ptr_oran[i]);
            }
            printf("\n");*/

            // Handle RTC and IQ Data Message Types
            if (ecpri->message_type == 0x02) {          // RTC Message (C-Plane)
                struct oran_rtc_radio_app_header *oran = (struct oran_rtc_radio_app_header *)(ptr_oran);
                uint8_t slot_id = ((oran->slot_id_first & 0x0F) << 2) | ((oran->slot_id_last & 0x03));
                /*printf("    ORAN RTC Radio App Header:\n");
                printf("    Data Direction: %u\n", oran->data_direction);
                printf("    Payload Version: %u\n", oran->payload_version);
                printf("    Filter Index: %u\n", oran->filter_index);
                printf("    Frame ID: 0x%02x\n", oran->frame_id);
                printf("    Subframe ID: %u\n", oran->subframe_id);
                printf("    Slot ID: 0x%02x\n", slot_id);
                printf("    Start Symbol ID: %u\n", oran->start_symbol_id);
                printf("    Number of Sections: 0x%02x\n", oran->number_of_sections);
                printf("    Section Type: 0x%02x\n", oran->section_type);
                printf("\n");*/
                // RTC parsing not neccesary for IQ data extraction

            } else if (ecpri->message_type == 0x00) {   // IQ Data Message (U-Plane)
                struct oran_iq_radio_app_header *oran = (struct oran_iq_radio_app_header *)(ptr_oran);
                uint8_t slot_id = ((oran->slot_id_first & 0x0F) << 2) | ((oran->slot_id_last & 0x03));
                /*printf("    ORAN IQ Radio App Header:\n");
                printf("    Data Direction: %u\n", oran->data_direction);
                printf("    Payload Version: %u\n", oran->payload_version);
                printf("    Filter Index: %u\n", oran->filter_index);
                printf("    Frame ID: 0x%02x\n", oran->frame_id);
                printf("    Subframe ID: %u\n", oran->subframe_id);
                printf("    Slot ID: 0x%02x\n", slot_id);
                printf("    Start Symbol ID: %u\n", oran->start_symbol_id);
                printf("\n");*/

                const bool use_bfp = (strcmp(ud_comp_method, "BFP") == 0);
                const bool use_none = (strcmp(ud_comp_method, "NONE") == 0);
                // udCompMeth codes: 0 = no compression, 1 = block floating point.
                const int configured_comp_code = use_bfp ? 1 : 0;

                // Handle Sections
                int section_length = 0;
                int payload_left = ntohs(ecpri->payload_size) - sizeof(struct oran_iq_radio_app_header);
                int section_count = 0;
                /* Allocate an initial sections array so we can store parsed sections.
                 * We don't know the exact number of sections up-front, so use a
                 * reasonable upper bound and prevent overruns. If more sections
                 * are required, the parser will stop adding new sections.
                 */
                const int MAX_SECTIONS = 256;
                if (decoded_pkt->sections == NULL) {
                    decoded_pkt->sections = calloc((size_t)MAX_SECTIONS, sizeof(*decoded_pkt->sections));
                    if (decoded_pkt->sections == NULL) {
                        return -1;
                    }
                }
                while (payload_left - section_length > 0) {

                    {
                        if (section_count >= MAX_SECTIONS) {
                            // avoid buffer overflow into sections array
                            break;
                        }
                        const uint8_t *ptr_section = ptr_oran + ntohs(ecpri->payload_size) - payload_left;
                        struct oran_iq_section_header *section = (struct oran_iq_section_header *)(ptr_section);
                        uint16_t section_id = ((section->section_id_first & 0x0F) << 8) | (section->section_id_last & 0x0F);
                        uint16_t start_prbu = ((section->start_prbu_first & 0x03) << 8) | (section->start_prbu_last & 0xFF);

                        // Resolve the compression parameters for this section
                        // before any offset arithmetic: when the udCompHdr is
                        // present it both shifts the PRB data along by a byte
                        // and states the real width and method, which the
                        // configured settings can disagree with.
                        const uint8_t *ptr_after_section_hdr = ptr_section + sizeof(struct oran_iq_section_header);
                        const int udcomphdr_length = section_udcomphdr_length(
                            ptr_after_section_hdr, ud_comp_hdr_present, ud_iq_width, configured_comp_code,
                            ntohs(ecpri->eaxc_id), oran->data_direction);

                        int sec_iq_width = ud_iq_width;
                        bool sec_use_bfp = use_bfp;
                        bool sec_use_none = use_none;
                        if (udcomphdr_length) {
                            const int hdr_iq_width = (ptr_after_section_hdr[0] >> 4) & 0x0F;
                            const int hdr_comp_meth = ptr_after_section_hdr[0] & 0x0F;
                            // The wire describes itself, so prefer it over the
                            // configured values. udIqWidth 0 means 16 bits.
                            sec_iq_width = (hdr_iq_width == 0) ? 16 : hdr_iq_width;
                            sec_use_bfp = (hdr_comp_meth == 1);
                            sec_use_none = (hdr_comp_meth == 0);
                        }

                        const int ud_comp_param_length = sec_use_bfp ? 1 : 0;
                        const int prb_stride = (sec_iq_width * 2 * 12) / 8 + ud_comp_param_length;
                        const uint8_t *ptr_prb_area = ptr_after_section_hdr + udcomphdr_length;

                        int num_prbu_corrected = 0;
                        if (section->num_prbu == 0) {
                            //printf("      Warning: 0 PRBUs are to be interpreted full bandwidth (273 PRBUs).\n");
                            num_prbu_corrected = 273;
                        } else {
                            num_prbu_corrected = section->num_prbu;
                        }
                        section_length = (int)sizeof(struct oran_iq_section_header) + udcomphdr_length +
                                         prb_stride * num_prbu_corrected;
                        /*printf("      ORAN IQ Section Header:\n");
                        printf("      Section ID: 0x%03x (%d)\n", section_id, section_id);
                        printf("      RB: %u\n", section->rb);
                        printf("      SymInc: %u\n", section->sym_inc);
                        printf("      Start PRBu: 0x%03x (%d)\n", start_prbu, start_prbu);
                        printf("      Num PRBu: 0x%02x (%d)\n", section->num_prbu, section->num_prbu);
                        printf("      Section Length: %d bytes\n", section_length);
                        printf("\n");*/

                        // Allocate IQ data buffer for max 273 PRBus * 12 samples each
                        decoded_pkt->sections[section_count].iq_data = (fh_complex_t *)malloc(3276 * sizeof(*decoded_pkt->sections[section_count].iq_data));
                        if (decoded_pkt->sections[section_count].iq_data == NULL) {
                            return -1;
                        }

                        decoded_pkt->sections[section_count].eaxc_id = ntohs(ecpri->eaxc_id);
                        decoded_pkt->sections[section_count].data_direction = oran->data_direction;
                        decoded_pkt->sections[section_count].frame_id = oran->frame_id;
                        decoded_pkt->sections[section_count].subframe_id = oran->subframe_id;
                        decoded_pkt->sections[section_count].slot_id = slot_id;
                        decoded_pkt->sections[section_count].start_symbol_id = oran->start_symbol_id;
                        decoded_pkt->sections[section_count].start_prbu = start_prbu;
                        if (section->num_prbu == 0) {
                            decoded_pkt->sections[section_count].num_prbu = 273;
                            //printf("      Corrected Num PRBu: %d\n", decoded_pkt->sections[section_count].num_prbu);
                        } else {
                            decoded_pkt->sections[section_count].num_prbu = section->num_prbu;
                        }
                        decoded_pkt->section_count++;

                        if (sec_use_bfp) {
                            // Diagnostic only: records where the per-PRB
                            // exponents appear to actually live. Inert unless
                            // the probe was enabled. Measured from the same
                            // place the decode below reads, so it reports on
                            // the corrected layout rather than the old one.
                            //
                            // The bound is what the packet really holds, not
                            // what the section claims, so the probe can report
                            // sections asking for more PRBs than were captured.
                            const uint8_t *payload_end = ptr_oran + ntohs(ecpri->payload_size);
                            const uint8_t *capture_end = raw_pkt->data + raw_pkt->caplen;
                            const uint8_t *limit = (payload_end < capture_end) ? payload_end : capture_end;
                            const int available = (limit > ptr_prb_area) ? (int)(limit - ptr_prb_area) : 0;
                            fh_iq_bfp_probe_note_section(
                                decoded_pkt->sections[section_count].eaxc_id,
                                decoded_pkt->sections[section_count].data_direction,
                                ptr_after_section_hdr,
                                udcomphdr_length,
                                decoded_pkt->sections[section_count].num_prbu,
                                prb_stride,
                                available + udcomphdr_length);
                        }

                        int iq_chunk_prb_idx = 0;
                        // Handle PRBs
                        for (size_t prb = 0; prb < decoded_pkt->sections[section_count].num_prbu; prb++) {
                            const uint8_t *ptr_prb = ptr_prb_area + prb * prb_stride;
                            /*if (strcmp(ud_comp_method, "BFP") == 0) {
                                printf("        UD Compression Param: Reserved: %d, Exponent: %d\n", ptr_prb[0] & 0xF0, ptr_prb[0] & 0x0F);
                            }
                            printf("        IQ Data (%d bytes): ", ud_iq_width * 12 * 2 / 8);
                            for (int i = 0; i < ud_iq_width * 12 * 2 / 8; i++) {
                                printf("%02x ", ptr_prb[i + ud_comp_param_length]);
                            }
                            printf("\n");*/

                            float i_samples[12], q_samples[12];
                            if (sec_use_none) {
                                // Convert IQ data to array
                                int iq_result = extract_iq_samples(
                                    (const char *)(ptr_prb + ud_comp_param_length),
                                    sec_iq_width,
                                    i_samples, q_samples);

                                if (iq_result == 0) {
                                    // Successfully extracted
                                    //printf("        Extracted IQ Samples:\n");
                                    for (int i = 0; i < 12; i++) {
                                        decoded_pkt->sections[section_count].iq_data[iq_chunk_prb_idx * 12 + i] = i_samples[i] + I * q_samples[i];
                                        //printf("        Sample %d: I = %f, Q = %f\n", i, crealf(decoded_pkt->sections[section_count].iq_data[iq_chunk_prb_idx * 12 + i]), cimagf(decoded_pkt->sections[section_count].iq_data[iq_chunk_prb_idx * 12 + i]));
                                    }
                                } else {
                                    printf("        IQ Sample Extraction failed with error code: %d\n", iq_result);
                                }

                            } else if (sec_use_bfp) {
                                int decomp_result = bfp_decompress_prb(
                                    (const char *)(ptr_prb + ud_comp_param_length),
                                    ud_iq_width_decompressed,
                                    sec_iq_width,
                                    ptr_prb[0] & 0x0F,
                                    i_samples, q_samples);

                                if (decomp_result == 0) {
                                    // Successfully decompressed
                                    //printf("        Decompressed IQ Samples:\n");
                                    for (int i = 0; i < 12; i++) {
                                        decoded_pkt->sections[section_count].iq_data[iq_chunk_prb_idx * 12 + i] = i_samples[i] + I * q_samples[i];
                                        //printf("        Sample %d: I = %f, Q = %f\n", i, crealf(decoded_pkt->sections[section_count].iq_data[iq_chunk_prb_idx * 12 + i]), cimagf(decoded_pkt->sections[section_count].iq_data[iq_chunk_prb_idx * 12 + i]));
                                    }
                                } else {
                                    printf("        BFP Decompression failed with error code: %d\n", decomp_result);
                                }
                            }
                            iq_chunk_prb_idx++;
                        }
                        //printf("\n");
                        //free(ptr_iq_chunk);
                        payload_left -= section_length;
                    }
                    section_count++;
                }

            } else {
                printf("  Unknown eCPRI Message Type for ORAN Radio App Header parsing.\n\n");
            }

        } else {
            printf("  EtherType: 0x%04x (not eCPRI)\n\n", ethertype);
        }
    }

    return 0;
}

void fh_decoded_packet_free(fh_decoded_packet_t *pkt) {
    if (pkt == NULL) {
        return;
    }
    for (int i = 0; i < pkt->section_count; i++) {
        free(pkt->sections[i].iq_data);
        pkt->sections[i].iq_data = NULL;
    }
    free(pkt->sections);
    pkt->sections = NULL;
}

static int64_t extract_signed_bits(const uint8_t *buffer, uint32_t bit_offset, int bit_width) {
    int64_t value = 0;

    // Extract bits from buffer, MSB first
    for (int i = 0; i < bit_width; i++) {
        uint32_t byte_idx = (bit_offset + i) / 8;
        uint32_t bit_idx = (bit_offset + i) % 8;

        // Extract bit (bit 7 is MSB, bit 0 is LSB in each byte)
        uint8_t bit = (buffer[byte_idx] >> (7 - bit_idx)) & 1;
        value = (value << 1) | bit;
    }

    // Handle 2's complement sign extension for negative numbers
    if (bit_width < 64) {
        int64_t sign_bit = 1LL << (bit_width - 1);
        if (value & sign_bit) {
            // Negative number - sign extend all upper bits
            value |= (-sign_bit);
        }
    }

    return value;
}

int extract_iq_samples(const char *ptr_prb, int bit_width, float i_samples[12], float q_samples[12]) {
    // Validate inputs
    if (!ptr_prb || !i_samples || !q_samples || !bit_width) {
        return -1; // Invalid input pointers
    }

    // Calculate required bits and bytes for this PRB
    // 12 IQ pairs * 2 samples/pair * compressed_bit_width bits/sample
    uint32_t bits_needed = 12 * 2 * bit_width;
    uint32_t bytes_needed = bits_needed / 8;

    // Collect the bytes we need for this PRB
    uint8_t *prb_bytes = (uint8_t *)malloc(bytes_needed);
    if (!prb_bytes) {
        return -4; // Memory allocation failure
    }

    for (uint32_t i = 0; i < bytes_needed; i++) {
        prb_bytes[i] = ptr_prb[i];
    }

    uint32_t bit_pos = 0;

    for (int sample_idx = 0; sample_idx < 12; sample_idx++) {
        // Extract I-sample mantissa (2's complement)
        int64_t i_sample = extract_signed_bits(prb_bytes, bit_pos, bit_width);
        bit_pos += bit_width;

        // Extract Q-sample mantissa (2's complement)
        int64_t q_sample = extract_signed_bits(prb_bytes, bit_pos, bit_width);
        bit_pos += bit_width;

        i_samples[sample_idx] = (float)(i_sample * pow(2.0, -15.0));
        q_samples[sample_idx] = (float)(q_sample * pow(2.0, -15.0));
    }
    free(prb_bytes);
    return 0;
}