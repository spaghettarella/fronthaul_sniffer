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

// --- Pacing ---------------------------------------------------------------
// Replay runs on a schedule rather than a sleep per packet: each packet falls
// due one interval after the one before it, and the schedule, not the sleep,
// keeps time. A sleep that overshoots is paid back by the packets after it
// going out without one. That is what holds the average rate at speeds where
// an interval is far shorter than any sleep the kernel can deliver - 3.3 us at
// 300 000 packets/s, against 50 us or more for a nanosleep once timer slack is
// counted - which a sleep per packet capped at roughly 10 000 packets/s.
//
// The price is that at high speed packets leave in short bursts instead of
// evenly spaced: the thread only sleeps once it is PCAP_PACE_MIN_SLEEP_S ahead,
// so a burst is about that long times the speed - 150 packets at 300 000/s,
// comfortably inside the default 4096-packet queue.

// Shortest lead worth sleeping for. Below it, a sleep's overshoot would be a
// large part of the wait itself, so the packet goes out early instead and the
// lead carries over to the next one.
#define PCAP_PACE_MIN_SLEEP_S 0.0005

// Longest single sleep. Keeps a speed change, or a stop request, from waiting
// out a gap scheduled under the old speed - at 1 packet/s that would otherwise
// be a whole second.
#define PCAP_PACE_SLICE_S 0.05

// How far replay may fall behind schedule before the debt is written off. A
// replay asked for more than the file read and queue can deliver would
// otherwise owe an ever-growing backlog, and one that was held up briefly - a
// descheduled thread, a slow read - would pay it all back in a single burst
// big enough to overrun the queue. Short enough to cap that burst at 1500
// packets at 300 000/s; long enough to absorb ordinary scheduling jitter
// without costing any rate.
#define PCAP_PACE_MAX_LAG_S 0.005

static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1.0e9;
}

static double pace_interval_seconds(const fh_pcap_stream_options_t* options) {
    if (options->live_speed_pps != NULL) {
        const int pps = __atomic_load_n(options->live_speed_pps, __ATOMIC_RELAXED);
        if (pps > 0) {
            return 1.0 / (double)pps;
        }
    }
    return options->demo_pcap_timeout;
}

// Holds the next packet until it is due. The due time is recomputed from the
// previous packet's on every pass, with the speed re-read each time, so a new
// speed re-times the gap already in progress instead of waiting it out. The
// first packet on a new schedule goes out at once and starts it.
static void pace_wait_until_due(const fh_pcap_stream_options_t* options,
                                fh_pcap_schedule_t* pace,
                                const volatile sig_atomic_t* stop_flag) {
    if (!pace->started) {
        pace->origin = monotonic_seconds();
        pace->last_due = 0.0;
        pace->started = 1;
        return;
    }

    while (*stop_flag == 0) {
        // Relative to the schedule's origin, so the times stay small and a
        // double resolves them far below a nanosecond however long it runs.
        const double due = pace->last_due + pace_interval_seconds(options);
        const double now = monotonic_seconds() - pace->origin;
        const double lead = due - now;
        if (lead < PCAP_PACE_MIN_SLEEP_S) {
            // Close enough, or late. Taking the due time rather than now keeps
            // the average exact; rebasing on now only once too far behind.
            pace->last_due = (now - due > PCAP_PACE_MAX_LAG_S) ? now : due;
            return;
        }
        sleep_for_timeout(lead < PCAP_PACE_SLICE_S ? lead : PCAP_PACE_SLICE_S);
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

    // The caller's schedule when it keeps one across passes, so the pause
    // between passes of a looped replay is caught up rather than lost - up to
    // PCAP_PACE_MAX_LAG_S of it, beyond which it is written off like any other
    // stall. Otherwise a fresh one for this call.
    fh_pcap_schedule_t local_schedule;
    memset(&local_schedule, 0, sizeof(local_schedule));
    fh_pcap_schedule_t* pace = (options->schedule != NULL) ? options->schedule : &local_schedule;

    while (*stop_flag == 0) {
        struct pcap_pkthdr* header = NULL;
        const u_char* packet_data = NULL;
        const int rc = pcap_next_ex(pcap_handle, &header, &packet_data);
        if (rc == 1) {
            // Dropped before the queue, so a filtered-out channel never gets
            // decoded, never gets a memory ring, and never shows up in the
            // stream selector. Rejected packets are deliberately not scheduled,
            // so filtering does not slow the replay down.
            if (!fh_channel_filter_accepts(&options->filter, (const uint8_t*)packet_data,
                                           (size_t)header->caplen, datalink)) {
                continue;
            }

            pace_wait_until_due(options, pace, stop_flag);
            if (*stop_flag != 0) {
                break;
            }

            // Checked after the schedule rather than before it: an unselected
            // packet still takes its slot in the replay, so ticking "Only
            // decode selected" changes what gets decoded, not how fast the
            // capture plays - one stream out of twelve would otherwise run
            // twelve times faster the moment the box is ticked.
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
            (void)snprintf(pkt.ingress_if, sizeof(pkt.ingress_if), "%s", "pcap");
            pkt.data = (uint8_t*)packet_data;

            if (on_packet(&pkt, user_ctx) != 0) {
                break;
            }
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
