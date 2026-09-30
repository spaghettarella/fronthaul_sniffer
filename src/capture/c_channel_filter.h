#pragma once

// Pre-queue eAxC / direction filtering, shared by every capture backend that
// sees eCPRI-framed packets.
//
// Filtering here rather than after decode keeps unwanted streams out of the
// packet queue entirely, so they never get a memory ring and never appear in
// the stream selector - which is what the startup screen's Channel filter
// promises. A backend that skips this will happily surface channels the user
// asked to filter out.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Framing of the packets handed to fh_channel_filter_accepts. The values match
// libpcap's DLT_* numbering, so a pcap-backed source can pass pcap_datalink()
// straight through. A raw AF_PACKET/SOCK_RAW socket delivers whole Ethernet
// frames starting at the destination MAC, which is FH_LINKTYPE_ETHERNET.
enum fh_linktype {
    FH_LINKTYPE_NULL = 0,
    FH_LINKTYPE_ETHERNET = 1,
    FH_LINKTYPE_LINUX_SLL = 113,
    FH_LINKTYPE_LINUX_SLL2 = 276
};

// Directions a whitelist entry lets through, as bits.
#define FH_CHANNEL_DIR_UL 0x1
#define FH_CHANNEL_DIR_DL 0x2
#define FH_CHANNEL_DIR_BOTH (FH_CHANNEL_DIR_UL | FH_CHANNEL_DIR_DL)

// Longest whitelist. Several times the eAxC count of a large multi-antenna,
// multi-carrier setup, and small enough that the filter stays a plain value
// every backend can copy into its options.
#define FH_CHANNEL_WHITELIST_MAX 64

typedef struct fh_channel_whitelist_entry {
    uint16_t eaxc;
    uint8_t directions;  // FH_CHANNEL_DIR_* bits
} fh_channel_whitelist_entry_t;

typedef enum fh_channel_filter_method {
    // One optional eAxC and one optional direction. The zero value, so a
    // zeroed filter keeps everything.
    FH_CHANNEL_FILTER_SINGLE = 0,
    // Only the channels listed in whitelist[], each with its own directions.
    FH_CHANNEL_FILTER_WHITELIST = 1
} fh_channel_filter_method_t;

typedef struct fh_channel_filter {
    fh_channel_filter_method_t method;

    // FH_CHANNEL_FILTER_SINGLE
    int has_eaxc;       // when non-zero, only packets whose eAxC matches pass
    uint16_t eaxc;
    int has_direction;  // when non-zero, only packets whose direction matches pass
    int direction;      // 0 = UL, 1 = DL

    // FH_CHANNEL_FILTER_WHITELIST: a packet passes when its eAxC is listed with
    // its direction. Listing an eAxC twice lets through both entries'
    // directions. An empty list keeps everything, the same as a single-channel
    // filter with nothing set - a filter that passes nothing is never what
    // anyone wants.
    int whitelist_count;
    fh_channel_whitelist_entry_t whitelist[FH_CHANNEL_WHITELIST_MAX];
} fh_channel_filter_t;

// Non-zero when the filter would reject something, so callers can skip the
// per-packet parse entirely when nothing is being filtered.
int fh_channel_filter_is_active(const fh_channel_filter_t* filter);

// Non-zero when the packet should be kept. A NULL or inactive filter keeps
// everything. datalink is the libpcap DLT_* value for the capture source;
// packets whose framing cannot be parsed are rejected.
//
// Direction filtering requires a U-plane (IQ data) packet, since that is the
// only message type carrying a direction bit; control-plane packets are
// dropped when a direction filter is set. Likewise a whitelist entry limited
// to UL or DL only ever matches U-plane packets, while one allowing both
// directions matches the eAxC regardless.
int fh_channel_filter_accepts(const fh_channel_filter_t* filter,
                              const uint8_t* packet_data,
                              size_t caplen,
                              int datalink);

// The same test for a source that reports each record's eAxC and direction
// itself rather than carrying eCPRI frames, such as the ocudu phy tap.
// direction: 0 = UL, 1 = DL.
int fh_channel_filter_accepts_stream(const fh_channel_filter_t* filter, uint16_t eaxc, int direction);

// --- Running selection ---
// The running UI's "Only decode selected": the channels on screen, applied on
// top of - never instead of - the capture's own filter, so a packet has to
// pass both. At most FH_CHANNEL_SELECTION_MAX channels: one for the waterfall
// and curve views, the DL/UL pair the radar reads.
//
// Packed into one 64-bit word, so a capture thread can read it per packet
// with a single __atomic_load_n while the render thread replaces it whenever
// the selection moves - no lock, and never half of an old selection mixed
// with half of a new one. 0 selects nothing out and keeps every channel.
#define FH_CHANNEL_SELECTION_MAX 2

// Entries beyond FH_CHANNEL_SELECTION_MAX, and entries with no directions, are
// left out.
uint64_t fh_channel_selection_pack(const fh_channel_whitelist_entry_t* entries, int count);

// Whether a packet is one of the selected channels; always so for selection 0.
// Parses the packet the same way fh_channel_filter_accepts does.
int fh_channel_selection_accepts(uint64_t selection, const uint8_t* packet_data, size_t caplen, int datalink);

// Stream-level form, for sources that report eAxC and direction directly.
int fh_channel_selection_accepts_stream(uint64_t selection, uint16_t eaxc, int direction);

#ifdef __cplusplus
}
#endif
