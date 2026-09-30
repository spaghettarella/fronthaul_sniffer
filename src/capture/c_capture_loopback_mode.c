#include "c_capture_loopback_mode.h"

#include <errno.h>
#include <arpa/inet.h>
#include <pcap/pcap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int write_err(char* errbuf, size_t errbuf_len, const char* msg) {
    if (errbuf != NULL && errbuf_len > 0) {
        (void)snprintf(errbuf, errbuf_len, "%s", msg);
    }
    return -1;
}

int fh_capture_loopback_run(
    const fh_capture_loopback_options_t* options,
    fh_packet_callback_t on_packet,
    void* user_ctx,
    const volatile sig_atomic_t* stop_flag,
    char* errbuf,
    size_t errbuf_len) {

    if (options == NULL || on_packet == NULL || stop_flag == NULL) {
        return write_err(errbuf, errbuf_len, "invalid loopback capture arguments");
    }
    if (options->interface_name == NULL || options->interface_name[0] == '\0') {
        return write_err(errbuf, errbuf_len, "interface_name is required");
    }

    char pcap_errbuf[PCAP_ERRBUF_SIZE];
    memset(pcap_errbuf, 0, sizeof(pcap_errbuf));

    // Built with pcap_create/pcap_activate rather than pcap_open_live so
    // immediate mode can be requested: a fronthaul stream should reach the
    // decoder as it arrives, not once a kernel ring block happens to fill.
    pcap_t* pcap_handle = pcap_create(options->interface_name, pcap_errbuf);
    if (pcap_handle == NULL) {
        return write_err(errbuf, errbuf_len, pcap_errbuf[0] != '\0' ? pcap_errbuf : "failed to open pcap on interface");
    }

    (void)pcap_set_snaplen(pcap_handle, options->snaplen);
    (void)pcap_set_promisc(pcap_handle, 1);
    (void)pcap_set_immediate_mode(pcap_handle, 1);
    // Never zero: pcap_set_timeout(3) leaves that undefined, and on Linux it
    // blocks in poll() forever, so a silent stream would strand this thread
    // past the stop_flag check and hang the join on shutdown.
    (void)pcap_set_timeout(pcap_handle, FH_CAPTURE_POLL_TIMEOUT_MS);

    if (pcap_activate(pcap_handle) < 0) {
        // pcap_geterr reads out of the handle, so copy the message before the
        // handle is freed.
        char activate_err[PCAP_ERRBUF_SIZE];
        (void)snprintf(activate_err, sizeof(activate_err), "%s", pcap_geterr(pcap_handle));
        pcap_close(pcap_handle);
        return write_err(errbuf, errbuf_len, activate_err[0] != '\0' ? activate_err : "failed to activate pcap on interface");
    }

    const int datalink = pcap_datalink(pcap_handle);

    int result = 0;
    int stall_armed = 0;
    struct timespec last_packet_time;
    fh_capture_monotonic_now(&last_packet_time);

    while (*stop_flag == 0) {
        struct pcap_pkthdr* header = NULL;
        const u_char* packet_data = NULL;
        const int rc = pcap_next_ex(pcap_handle, &header, &packet_data);

        if (rc == 1) {
            if (!fh_channel_filter_accepts(&options->filter, (const uint8_t*)packet_data,
                                           (size_t)header->caplen, datalink)) continue;

            fh_capture_monotonic_now(&last_packet_time);
            stall_armed = 1;

            // The running selection only decides what gets decoded, so it comes
            // after the stall timer: a quiet stream picked on screen must not
            // look like a dead capture and end the run.
            if (options->live_selection != NULL &&
                !fh_channel_selection_accepts(__atomic_load_n(options->live_selection, __ATOMIC_RELAXED),
                                              (const uint8_t*)packet_data, (size_t)header->caplen, datalink)) {
                continue;
            }

            fh_packet_t pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.timestamp.tv_sec = (time_t)header->ts.tv_sec;
            pkt.timestamp.tv_nsec = (long)header->ts.tv_usec * 1000L;
            pkt.caplen = (size_t)header->caplen;
            pkt.len = (size_t)header->len;
            (void)snprintf(pkt.ingress_if, sizeof(pkt.ingress_if), "%s", options->interface_name);
            pkt.data = (uint8_t*)packet_data;

            if (on_packet(&pkt, user_ctx) != 0) break;
        } else if (rc == 0) {
            // Read timeout expired: the only place a silent stream shows up.
            if (options->stall_timeout_ms > 0 && stall_armed &&
                fh_capture_elapsed_ms(&last_packet_time) >= (long)options->stall_timeout_ms) {
                result = FH_CAPTURE_RC_STREAM_STOPPED;
                break;
            }
            continue;
        } else if (rc == -1) {
            char read_err[PCAP_ERRBUF_SIZE];
            (void)snprintf(read_err, sizeof(read_err), "%s", pcap_geterr(pcap_handle));
            pcap_close(pcap_handle);
            return write_err(errbuf, errbuf_len, read_err);
        } else if (rc == -2) {
            break;
        }
    }

    pcap_close(pcap_handle);
    return result;
}
