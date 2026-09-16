#include "c_pcap_demo_stream.h"

#include <errno.h>
#include <pcap/pcap.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int write_err(char* errbuf, size_t errbuf_len, const char* msg) {
    if (errbuf != NULL && errbuf_len > 0) {
        (void)snprintf(errbuf, errbuf_len, "%s", msg);
    }
    return -1;
}

static void sleep_for_timeout(double seconds) {
    if (seconds <= 0.0) {
        return;
    }

    struct timespec req;
    req.tv_sec = (time_t)seconds;
    req.tv_nsec = (long)((seconds - (double)req.tv_sec) * 1000000000.0);
    if (req.tv_nsec < 0) {
        req.tv_nsec = 0;
    }
    if (req.tv_nsec >= 1000000000L) {
        req.tv_sec += req.tv_nsec / 1000000000L;
        req.tv_nsec %= 1000000000L;
    }

    while (nanosleep(&req, &req) != 0 && errno == EINTR) {
        continue;
    }
}

int fh_pcap_stream_run(
    const fh_pcap_stream_options_t* options,
    fh_packet_callback_t on_packet,
    void* user_ctx,
    const volatile sig_atomic_t* stop_flag,
    char* errbuf,
    size_t errbuf_len) {
    if (options == NULL || options->pcap_path == NULL || on_packet == NULL || stop_flag == NULL) {
        return write_err(errbuf, errbuf_len, "invalid pcap stream arguments");
    }

    if (options->pcap_path[0] == '\0') {
        return write_err(errbuf, errbuf_len, "pcap_path is required");
    }

    char pcap_errbuf[PCAP_ERRBUF_SIZE];
    memset(pcap_errbuf, 0, sizeof(pcap_errbuf));
    pcap_t* pcap_handle = pcap_open_offline(options->pcap_path, pcap_errbuf);
    if (pcap_handle == NULL) {
        return write_err(errbuf, errbuf_len, pcap_errbuf[0] != '\0' ? pcap_errbuf : "failed to open pcap file");
    }

    // Framing of the recorded packets, needed to find the eCPRI payload the
    // channel filter inspects.
    const int datalink = pcap_datalink(pcap_handle);

    while (*stop_flag == 0) {
        struct pcap_pkthdr* header = NULL;
        const u_char* packet_data = NULL;
        const int rc = pcap_next_ex(pcap_handle, &header, &packet_data);
        if (rc == 1) {
            // Dropped before the queue, so a filtered-out channel never gets
            // decoded, never gets a memory ring, and never shows up in the
            // stream selector. Replay pacing is deliberately skipped for
            // rejected packets so filtering does not slow the replay down.
            if (!fh_channel_filter_accepts(&options->filter, (const uint8_t*)packet_data,
                                           (size_t)header->caplen, datalink)) {
                continue;
            }

            fh_packet_t pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.timestamp.tv_sec = (time_t)header->ts.tv_sec;
            pkt.timestamp.tv_nsec = (long)header->ts.tv_usec * 1000L;
            pkt.caplen = (size_t)header->caplen;
            pkt.len = (size_t)header->len;
            (void)snprintf(pkt.ingress_if, sizeof(pkt.ingress_if), "%s", "pcap");
            pkt.data = (uint8_t*)packet_data;

            if (on_packet(&pkt, user_ctx) != 0) {
                break;
            }

            sleep_for_timeout(options->demo_pcap_timeout);
            continue;
        }

        if (rc == PCAP_ERROR_BREAK || rc == -2) {
            break;
        }

        if (rc == PCAP_ERROR) {
            const char* message = pcap_geterr(pcap_handle);
            pcap_close(pcap_handle);
            return write_err(errbuf, errbuf_len, message != NULL ? message : "pcap read failed");
        }

        if (rc == 0) {
            continue;
        }

        pcap_close(pcap_handle);
        return write_err(errbuf, errbuf_len, "unexpected pcap read state");
    }

    pcap_close(pcap_handle);

    return 0;
}
