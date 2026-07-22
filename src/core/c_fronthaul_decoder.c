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
                    // Raw/uncompressed phy tap samples are only supported at their native
                    // 16-bit width; a different width would need its own unpacking logic.
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
                    const int16_t i_s = (int16_t)i_raw;
                    const int16_t q_s = (int16_t)q_raw;
                    decoded_pkt->sections[0].iq_data[i] = (float)i_s + I * (float)q_s;
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
                int ud_comp_param_length = use_bfp ? 1 : 0;

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

                    if (ud_comp_hdr_present == false) {
                        if (section_count >= MAX_SECTIONS) {
                            // avoid buffer overflow into sections array
                            break;
                        }
                        const uint8_t *ptr_section = ptr_oran + ntohs(ecpri->payload_size) - payload_left;
                        struct oran_iq_section_header *section = (struct oran_iq_section_header *)(ptr_section);
                        uint16_t section_id = ((section->section_id_first & 0x0F) << 8) | (section->section_id_last & 0x0F);
                        uint16_t start_prbu = ((section->start_prbu_first & 0x03) << 8) | (section->start_prbu_last & 0xFF);
                        int num_prbu_corrected = 0;
                        if (section->num_prbu == 0) {
                            //printf("      Warning: 0 PRBUs are to be interpreted full bandwidth (273 PRBUs).\n");
                            section_length = sizeof(struct oran_iq_section_header) + (((ud_iq_width * 2 * 12) / 8 + ud_comp_param_length) * 273);
                            num_prbu_corrected = 273;
                        } else {
                            section_length = sizeof(struct oran_iq_section_header) + (((ud_iq_width * 2 * 12) / 8 + ud_comp_param_length) * section->num_prbu);
                            num_prbu_corrected = section->num_prbu;
                        }
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

                        int iq_chunk_prb_idx = 0;
                        // Handle PRBs
                        for (size_t prb = 0; prb < decoded_pkt->sections[section_count].num_prbu; prb++) {
                            const uint8_t *ptr_prb = ptr_section + sizeof(struct oran_iq_section_header) + prb * ((ud_iq_width * 2 * 12) / 8 + ud_comp_param_length);
                            /*if (strcmp(ud_comp_method, "BFP") == 0) {
                                printf("        UD Compression Param: Reserved: %d, Exponent: %d\n", ptr_prb[0] & 0xF0, ptr_prb[0] & 0x0F);
                            }
                            printf("        IQ Data (%d bytes): ", ud_iq_width * 12 * 2 / 8);
                            for (int i = 0; i < ud_iq_width * 12 * 2 / 8; i++) {
                                printf("%02x ", ptr_prb[i + ud_comp_param_length]);
                            }
                            printf("\n");*/

                            float i_samples[12], q_samples[12];
                            if (use_none) {
                                // Convert IQ data to array
                                int iq_result = extract_iq_samples(
                                    (const char *)(ptr_prb + ud_comp_param_length),
                                    ud_iq_width,
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

                            } else if (use_bfp) {
                                int decomp_result = bfp_decompress_prb(
                                    (const char *)(ptr_prb + ud_comp_param_length),
                                    ud_iq_width_decompressed,
                                    ud_iq_width,
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
                    } else {
                        printf("  IQ Data Compression Header parsing not implemented yet.\n");
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