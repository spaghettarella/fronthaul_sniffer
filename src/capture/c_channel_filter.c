#include "c_channel_filter.h"

#include <arpa/inet.h>
#include <string.h>

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

struct oran_iq_radio_app_header {
    uint8_t filter_index : 4;
    uint8_t payload_version : 3;
    uint8_t data_direction : 1;
    uint8_t frame_id;
    uint8_t slot_id_first : 4;
    uint8_t subframe_id : 4;
    uint8_t start_symbol_id : 6;
    uint8_t slot_id_last : 2;
} __attribute__((packed));

struct fh_packet_payload_view {
    const uint8_t* payload;
    size_t payload_len;
};

// Extract the eCPRI payload pointer & length depending on the datalink framing.
static int get_packet_payload_view(const uint8_t* packet_data,
                                   size_t caplen,
                                   int datalink,
                                   struct fh_packet_payload_view* view) {
    if (packet_data == NULL || view == NULL) {
        return 0;
    }

    const uint8_t* packet = packet_data;
    size_t offset = 0;
    uint16_t ethertype = 0;

    switch (datalink) {
        case FH_LINKTYPE_ETHERNET:
            if (caplen < 14) return 0;
            offset = 14;
            memcpy(&ethertype, packet + 12, sizeof(ethertype));
            ethertype = ntohs(ethertype);
            break;
        case FH_LINKTYPE_LINUX_SLL:
            if (caplen < 16) return 0;
            offset = 16;
            memcpy(&ethertype, packet + 14, sizeof(ethertype));
            ethertype = ntohs(ethertype);
            break;
        case FH_LINKTYPE_LINUX_SLL2:
            if (caplen < 20) return 0;
            offset = 20;
            memcpy(&ethertype, packet + offset - 2, sizeof(ethertype));
            ethertype = ntohs(ethertype);
            break;
        case FH_LINKTYPE_NULL:
            // DLT_NULL: first 4 bytes are AF_*; treat as IPv4/IPv6 passthrough
            if (caplen < 4) return 0;
            offset = 4;
            ethertype = 0; // not used
            break;
        default:
            return 0;
    }

    // Skip VLAN tags
    while ((ethertype == 0x8100 || ethertype == 0x88A8) && caplen >= offset + 4) {
        offset += 4;
        memcpy(&ethertype, packet + offset - 2, sizeof(ethertype));
        ethertype = ntohs(ethertype);
    }

    // eCPRI EtherType check (0xAEFE) unless DLT_NULL path
    if (datalink != FH_LINKTYPE_NULL) {
        if (ethertype != 0xAEFE) return 0;
    }

    if (caplen < offset + sizeof(struct ecpri_common_header)) return 0;

    view->payload = packet + offset;
    view->payload_len = caplen - offset;
    return 1;
}

int fh_channel_filter_is_active(const fh_channel_filter_t* filter) {
    if (filter == NULL) {
        return 0;
    }
    return (filter->has_eaxc || filter->has_direction) ? 1 : 0;
}

int fh_channel_filter_accepts(const fh_channel_filter_t* filter,
                              const uint8_t* packet_data,
                              size_t caplen,
                              int datalink) {
    // Nothing to filter: keep the packet without parsing it at all.
    if (!fh_channel_filter_is_active(filter)) {
        return 1;
    }

    struct fh_packet_payload_view view;
    if (!get_packet_payload_view(packet_data, caplen, datalink, &view)) return 0;
    if (view.payload_len < sizeof(struct ecpri_common_header)) return 0;

    const struct ecpri_common_header* ecpri = (const struct ecpri_common_header*)view.payload;

    // Optional eAxC filter
    if (filter->has_eaxc) {
        if (ntohs(ecpri->eaxc_id) != filter->eaxc) return 0;
    }

    // If direction filtering not requested, accept now
    if (!filter->has_direction) return 1;

    // Require IQ Data (U-Plane) message type
    if (ecpri->message_type != 0x00) return 0;
    if (view.payload_len < sizeof(struct ecpri_common_header) + sizeof(struct oran_iq_radio_app_header)) return 0;

    const struct oran_iq_radio_app_header* oran =
        (const struct oran_iq_radio_app_header*)(view.payload + sizeof(struct ecpri_common_header));
    return (int)oran->data_direction == filter->direction;
}
