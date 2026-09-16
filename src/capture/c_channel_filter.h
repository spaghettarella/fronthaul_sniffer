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

typedef struct fh_channel_filter {
    int has_eaxc;       // when non-zero, only packets whose eAxC matches pass
    uint16_t eaxc;
    int has_direction;  // when non-zero, only packets whose direction matches pass
    int direction;      // 0 = UL, 1 = DL
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
// dropped when a direction filter is set.
int fh_channel_filter_accepts(const fh_channel_filter_t* filter,
                              const uint8_t* packet_data,
                              size_t caplen,
                              int datalink);

#ifdef __cplusplus
}
#endif
