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
    if (filter->method == FH_CHANNEL_FILTER_WHITELIST) {
        return (filter->whitelist_count > 0) ? 1 : 0;
    }
    return (filter->has_eaxc || filter->has_direction) ? 1 : 0;
}

// Reads the data direction of a U-plane (IQ data) packet: 0 = UL, 1 = DL.
// Returns 0 when there is none to read - any other message type, or a payload
// too short to hold the radio application header.
static int read_uplane_direction(const struct fh_packet_payload_view* view, int* out_direction) {
    const struct ecpri_common_header* ecpri = (const struct ecpri_common_header*)view->payload;
    if (ecpri->message_type != 0x00) {
        return 0;
    }
    if (view->payload_len < sizeof(struct ecpri_common_header) + sizeof(struct oran_iq_radio_app_header)) {
        return 0;
    }
    const struct oran_iq_radio_app_header* oran =
        (const struct oran_iq_radio_app_header*)(view->payload + sizeof(struct ecpri_common_header));
    *out_direction = (int)oran->data_direction;
    return 1;
}

// A packet's direction, read at most once and only if some rule asks for it.
// A source that already knows it (the phy tap) starts out resolved.
struct direction_source {
    const struct fh_packet_payload_view* view;
    int direction;
    int state;  // 0 = not read yet, 1 = known, -1 = none to read
};

static int direction_get(struct direction_source* source, int* out_direction) {
    if (source->state == 0) {
        source->state = read_uplane_direction(source->view, &source->direction) ? 1 : -1;
    }
    if (source->state < 0) {
        return 0;
    }
    *out_direction = source->direction;
    return 1;
}

static int entries_accept(const fh_channel_whitelist_entry_t* entries,
                          int count,
                          uint16_t eaxc,
                          struct direction_source* direction_source) {
    for (int i = 0; i < count; i++) {
        const fh_channel_whitelist_entry_t* entry = &entries[i];
        if (entry->eaxc != eaxc) {
            continue;
        }
        if ((entry->directions & FH_CHANNEL_DIR_BOTH) == FH_CHANNEL_DIR_BOTH) {
            return 1;
        }
        int direction = 0;
        if (!direction_get(direction_source, &direction)) {
            continue;  // a later entry for this eAxC may still allow both
        }
        const uint8_t bit = (direction == 1) ? FH_CHANNEL_DIR_DL : FH_CHANNEL_DIR_UL;
        if (entry->directions & bit) {
            return 1;
        }
    }
    return 0;
}

static int filter_accepts(const fh_channel_filter_t* filter, uint16_t eaxc, struct direction_source* direction_source) {
    if (!fh_channel_filter_is_active(filter)) {
        return 1;
    }

    if (filter->method == FH_CHANNEL_FILTER_WHITELIST) {
        const int count = (filter->whitelist_count < FH_CHANNEL_WHITELIST_MAX) ? filter->whitelist_count
                                                                                 : FH_CHANNEL_WHITELIST_MAX;
        return entries_accept(filter->whitelist, count, eaxc, direction_source);
    }

    // Optional eAxC filter
    if (filter->has_eaxc && eaxc != filter->eaxc) {
        return 0;
    }

    // If direction filtering not requested, accept now
    if (!filter->has_direction) {
        return 1;
    }

    // Require a direction to compare, which only U-plane packets carry
    int direction = 0;
    if (!direction_get(direction_source, &direction)) {
        return 0;
    }
    return direction == filter->direction;
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
    struct direction_source direction_source = { &view, 0, 0 };
    return filter_accepts(filter, ntohs(ecpri->eaxc_id), &direction_source);
}

int fh_channel_filter_accepts_stream(const fh_channel_filter_t* filter, uint16_t eaxc, int direction) {
    struct direction_source direction_source = { NULL, direction ? 1 : 0, 1 };
    return filter_accepts(filter, eaxc, &direction_source);
}

// --- Running selection ---
// Entry i sits in bits [32*i, 32*i + 18): the eAxC in the low 16, its
// FH_CHANNEL_DIR_* bits in the next 2. An entry with no directions is absent,
// which is also what makes an all-zero word mean "no selection".

uint64_t fh_channel_selection_pack(const fh_channel_whitelist_entry_t* entries, int count) {
    uint64_t word = 0;
    int packed = 0;
    for (int i = 0; entries != NULL && i < count && packed < FH_CHANNEL_SELECTION_MAX; i++) {
        const uint64_t directions = (uint64_t)(entries[i].directions & FH_CHANNEL_DIR_BOTH);
        if (directions == 0) {
            continue;
        }
        word |= ((uint64_t)entries[i].eaxc | (directions << 16)) << (32 * packed);
        packed++;
    }
    return word;
}

static int selection_unpack(uint64_t selection, fh_channel_whitelist_entry_t* out_entries) {
    int count = 0;
    for (int i = 0; i < FH_CHANNEL_SELECTION_MAX; i++) {
        const uint64_t slot = (selection >> (32 * i)) & 0x3FFFFu;
        const uint8_t directions = (uint8_t)((slot >> 16) & FH_CHANNEL_DIR_BOTH);
        if (directions == 0) {
            continue;
        }
        out_entries[count].eaxc = (uint16_t)(slot & 0xFFFFu);
        out_entries[count].directions = directions;
        count++;
    }
    return count;
}

int fh_channel_selection_accepts(uint64_t selection, const uint8_t* packet_data, size_t caplen, int datalink) {
    if (selection == 0) {
        return 1;
    }

    struct fh_packet_payload_view view;
    if (!get_packet_payload_view(packet_data, caplen, datalink, &view)) return 0;
    if (view.payload_len < sizeof(struct ecpri_common_header)) return 0;

    const struct ecpri_common_header* ecpri = (const struct ecpri_common_header*)view.payload;
    fh_channel_whitelist_entry_t entries[FH_CHANNEL_SELECTION_MAX];
    const int count = selection_unpack(selection, entries);
    struct direction_source direction_source = { &view, 0, 0 };
    return entries_accept(entries, count, ntohs(ecpri->eaxc_id), &direction_source);
}

int fh_channel_selection_accepts_stream(uint64_t selection, uint16_t eaxc, int direction) {
    if (selection == 0) {
        return 1;
    }
    fh_channel_whitelist_entry_t entries[FH_CHANNEL_SELECTION_MAX];
    const int count = selection_unpack(selection, entries);
    struct direction_source direction_source = { NULL, direction ? 1 : 0, 1 };
    return entries_accept(entries, count, eaxc, &direction_source);
}
