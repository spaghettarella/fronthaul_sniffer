#include "c_capture_socket_mode.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#ifdef __linux__
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

static int write_err(char* errbuf, size_t errbuf_len, const char* msg) {
    // Write a short error message into the caller-provided buffer.
    if (errbuf != NULL && errbuf_len > 0) {
        (void)snprintf(errbuf, errbuf_len, "%s", msg);
    }
    return -1;
}

#ifdef __linux__
int fh_capture_socket_run(
    const fh_capture_socket_options_t* options,
    fh_packet_callback_t on_packet,
    void* user_ctx,
    const volatile sig_atomic_t* stop_flag,
    char* errbuf,
    size_t errbuf_len) {
    // Validate the inputs before touching any OS resources.
    if (options == NULL || on_packet == NULL || stop_flag == NULL) {
        return write_err(errbuf, errbuf_len, "invalid capture arguments");
    }

    if (options->interface_name == NULL) {
        return write_err(errbuf, errbuf_len, "interface_name is required");
    }

    // Open a raw Ethernet socket on Linux and subscribe to all EtherTypes.
    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) {
        return write_err(errbuf, errbuf_len, "failed to create AF_PACKET socket");
    }

    // Apply a receive timeout so the loop can periodically check stop_flag.
    // A zero timeval means "no timeout" to the kernel, which would leave recv
    // blocked past shutdown once the stream goes quiet, so this is always the
    // fixed poll interval and never the user's stall setting.
    struct timeval timeout;
    timeout.tv_sec = FH_CAPTURE_POLL_TIMEOUT_MS / 1000;
    timeout.tv_usec = (FH_CAPTURE_POLL_TIMEOUT_MS % 1000) * 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // Resolve the interface index for the selected interface name.
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    (void)snprintf(ifr.ifr_name, IFNAMSIZ, "%s", options->interface_name);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        close(fd);
        return write_err(errbuf, errbuf_len, "failed to resolve interface index");
    }

    // Bind the raw socket to the requested interface.
    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex = ifr.ifr_ifindex;
    if (bind(fd, (struct sockaddr*)&sll, sizeof(sll)) < 0) {
        close(fd);
        return write_err(errbuf, errbuf_len, "failed to bind AF_PACKET socket");
    }

    // Allocate one receive buffer and reuse it for every packet read.
    uint8_t* buf = (uint8_t*)malloc((size_t)options->snaplen);
    if (buf == NULL) {
        close(fd);
        return write_err(errbuf, errbuf_len, "failed to allocate socket receive buffer");
    }

    int result = 0;
    int stall_armed = 0;
    struct timespec last_packet_time;
    fh_capture_monotonic_now(&last_packet_time);

    // Main receive loop: read packets, timestamp them, and hand them off.
    while (*stop_flag == 0) {
        ssize_t n = recv(fd, buf, (size_t)options->snaplen, 0);
        if (n < 0) {
            // Timeout just means we should check for shutdown and try again.
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (options->stall_timeout_ms > 0 && stall_armed &&
                    fh_capture_elapsed_ms(&last_packet_time) >= (long)options->stall_timeout_ms) {
                    result = FH_CAPTURE_RC_STREAM_STOPPED;
                    break;
                }
                continue;
            }
            free(buf);
            close(fd);
            return write_err(errbuf, errbuf_len, "socket recv failed");
        }

        // Dropped before the queue, so a filtered-out channel never gets
        // decoded and never appears in the stream selector. Ahead of the stall
        // timer deliberately, matching the loopback backend: the timeout is
        // meant to measure silence on the channel being watched, and feeding it
        // from traffic that was just discarded would keep a dead stream looking
        // alive on the strength of its neighbours.
        if (!fh_channel_filter_accepts(&options->filter, buf, (size_t)n, FH_LINKTYPE_ETHERNET)) {
            continue;
        }

        fh_capture_monotonic_now(&last_packet_time);
        stall_armed = 1;

        // Stamp the packet and populate the lightweight packet view.
        struct timespec ts;
        (void)timespec_get(&ts, TIME_UTC);

        fh_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.timestamp = ts;
        (void)snprintf(pkt.ingress_if, sizeof(pkt.ingress_if), "%s", options->interface_name);
        pkt.data = buf;
        pkt.caplen = (size_t)n;
        pkt.len = (size_t)n;

        if (on_packet(&pkt, user_ctx) != 0) {
            break;
        }
    }

    // Release resources before returning to the caller.
    free(buf);
    close(fd);
    return result;
}
#else
int fh_capture_socket_run(
    const fh_capture_socket_options_t* options,
    fh_packet_callback_t on_packet,
    void* user_ctx,
    const volatile sig_atomic_t* stop_flag,
    char* errbuf,
    size_t errbuf_len) {
    (void)options;
    (void)on_packet;
    (void)user_ctx;
    (void)stop_flag;
    return write_err(errbuf, errbuf_len, "socket capture supported on Linux only");
}
#endif
