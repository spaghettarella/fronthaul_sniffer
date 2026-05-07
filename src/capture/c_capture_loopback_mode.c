#include "c_capture_loopback_mode.h"

#include <errno.h>
#include <arpa/inet.h>
#include <pcap/pcap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FH_LOOPBACK_FILTER_EAXC_ID 0x0000u

enum fh_pcap_linktype {
    FH_DLT_NULL = 0,
    FH_DLT_EN10MB = 1,
    FH_DLT_LINUX_SLL = 113,
    FH_DLT_LINUX_SLL2 = 276,
};

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

struct fh_packet_payload_view {
    const uint8_t* payload;
    size_t payload_len;
};

static int write_err(char* errbuf, size_t errbuf_len, const char* msg) {
    if (errbuf != NULL && errbuf_len > 0) {
        (void)snprintf(errbuf, errbuf_len, "%s", msg);
    }
    return -1;
}

static int get_packet_payload_view(const u_char* packet_data, size_t caplen, int datalink, struct fh_packet_payload_view* view) {
    if (packet_data == NULL || view == NULL) {
        return 0;
    }

    const uint8_t* packet = (const uint8_t*)packet_data;
    size_t offset = 0;
    uint16_t ethertype = 0;

    switch (datalink) {
        case FH_DLT_EN10MB:
            if (caplen < 14) {
                return 0;
            }
            offset = 14;
            memcpy(&ethertype, packet + 12, sizeof(ethertype));
            ethertype = ntohs(ethertype);
            break;
        case FH_DLT_LINUX_SLL:
            if (caplen < 16) {
                return 0;
            }
            offset = 16;
            memcpy(&ethertype, packet + 14, sizeof(ethertype));
            ethertype = ntohs(ethertype);
            break;
        case FH_DLT_LINUX_SLL2:
            if (caplen < 20) {
                return 0;
            }
            offset = 20;
            memcpy(&ethertype, packet + 0, sizeof(ethertype));
            ethertype = ntohs(ethertype);
            break;
        default:
            return 0;
    }

    while (ethertype == 0x8100 || ethertype == 0x88A8) {
        if (caplen < offset + 4) {
            return 0;
        }
        offset += 4;
        memcpy(&ethertype, packet + offset - 2, sizeof(ethertype));
        ethertype = ntohs(ethertype);
    }

    if (ethertype != 0xAEFE) {
        return 0;
    }

    if (caplen < offset + sizeof(struct ecpri_common_header)) {
        return 0;
    }

    view->payload = packet + offset;
    view->payload_len = caplen - offset;
    return 1;
}

static int packet_matches_loopback_filter(const u_char* packet_data, size_t caplen, int datalink) {
    struct fh_packet_payload_view view;
    if (!get_packet_payload_view(packet_data, caplen, datalink, &view)) {
        return 0;
    }

    if (view.payload_len < sizeof(struct ecpri_common_header)) {
        return 0;
    }

    const struct ecpri_common_header* ecpri = (const struct ecpri_common_header*)view.payload;
    return ntohs(ecpri->eaxc_id) == FH_LOOPBACK_FILTER_EAXC_ID;
}

int fh_capture_loopback_run(
    const fh_capture_loopback_options_t* options,
    fh_packet_callback_t on_packet,
    void* user_ctx,
    const volatile sig_atomic_t* stop_flag,
    char* errbuf,
    size_t errbuf_len) {
    
    // Validate inputs
    if (options == NULL || on_packet == NULL || stop_flag == NULL) {
        return write_err(errbuf, errbuf_len, "invalid loopback capture arguments");
    }

    if (options->interface_name == NULL || options->interface_name[0] == '\0') {
        return write_err(errbuf, errbuf_len, "interface_name is required");
    }

    // Use libpcap to open the interface live (works for loopback via kernel's packet tapping)
    char pcap_errbuf[PCAP_ERRBUF_SIZE];
    memset(pcap_errbuf, 0, sizeof(pcap_errbuf));
    
    pcap_t* pcap_handle = pcap_open_live(
        options->interface_name,
        options->snaplen,
        1,  // promisc=1 for promiscuous mode (may help capture loopback)
        options->timeout_ms,
        pcap_errbuf);
    
    if (pcap_handle == NULL) {
        return write_err(errbuf, errbuf_len, 
                        pcap_errbuf[0] != '\0' ? pcap_errbuf : "failed to open pcap on interface");
    }

    const int datalink = pcap_datalink(pcap_handle);

    // Main capture loop
    while (*stop_flag == 0) {
        struct pcap_pkthdr* header = NULL;
        const u_char* packet_data = NULL;
        const int rc = pcap_next_ex(pcap_handle, &header, &packet_data);

        if (rc == 1) {
            if (!packet_matches_loopback_filter(packet_data, (size_t)header->caplen, datalink)) {
                continue;
            }

            // Got a packet
            fh_packet_t pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.timestamp.tv_sec = (time_t)header->ts.tv_sec;
            pkt.timestamp.tv_nsec = (long)header->ts.tv_usec * 1000L;
            pkt.caplen = (size_t)header->caplen;
            pkt.len = (size_t)header->len;
            (void)snprintf(pkt.ingress_if, sizeof(pkt.ingress_if), "%s", options->interface_name);
            pkt.data = (uint8_t*)packet_data;

            if (on_packet(&pkt, user_ctx) != 0) {
                break;
            }
        } else if (rc == 0) {
            // Timeout, check stop_flag and retry
            continue;
        } else if (rc == -1) {
            // Error reading from pcap stream
            pcap_close(pcap_handle);
            return write_err(errbuf, errbuf_len, pcap_geterr(pcap_handle));
        } else if (rc == -2) {
            // End of file (shouldn't happen with live capture, but just in case)
            break;
        }
    }

    // Cleanup
    pcap_close(pcap_handle);
    return 0;
}
