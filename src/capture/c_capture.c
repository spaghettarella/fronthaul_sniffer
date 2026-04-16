#include "fronthaul_sniffer/capture/c_capture.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#ifdef FRONTHAUL_ENABLE_PCAP
#include <pcap/pcap.h>
#endif

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
    if (errbuf != NULL && errbuf_len > 0) {
        (void)snprintf(errbuf, errbuf_len, "%s", msg);
    }
    return -1;
}

#ifdef FRONTHAUL_ENABLE_PCAP
static int run_pcap(
    const fh_capture_options_t* options,
    fh_packet_callback_t on_packet,
    void* user_ctx,
    const volatile sig_atomic_t* stop_flag,
    char* errbuf,
    size_t errbuf_len) {
    char pcap_err[PCAP_ERRBUF_SIZE] = {0};
    pcap_t* handle = pcap_open_live(
        options->interface_name,
        options->snaplen,
        options->promiscuous ? 1 : 0,
        options->timeout_ms,
        pcap_err);
    if (handle == NULL) {
        return write_err(errbuf, errbuf_len, pcap_err);
    }

    if (options->bpf_filter != NULL && options->bpf_filter[0] != '\0') {
        struct bpf_program program;
        if (pcap_compile(handle, &program, options->bpf_filter, 1, PCAP_NETMASK_UNKNOWN) < 0) {
            int rc = write_err(errbuf, errbuf_len, pcap_geterr(handle));
            pcap_close(handle);
            return rc;
        }
        if (pcap_setfilter(handle, &program) < 0) {
            int rc = write_err(errbuf, errbuf_len, pcap_geterr(handle));
            pcap_freecode(&program);
            pcap_close(handle);
            return rc;
        }
        pcap_freecode(&program);
    }

    while (*stop_flag == 0) {
        struct pcap_pkthdr* header = NULL;
        const u_char* data = NULL;
        int rc = pcap_next_ex(handle, &header, &data);
        if (rc == 0) {
            continue;
        }
        if (rc == -1) {
            int out = write_err(errbuf, errbuf_len, pcap_geterr(handle));
            pcap_close(handle);
            return out;
        }
        if (rc == -2) {
            break;
        }

        fh_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.timestamp.tv_sec = header->ts.tv_sec;
        pkt.timestamp.tv_nsec = (long)header->ts.tv_usec * 1000L;
        (void)snprintf(pkt.ingress_if, sizeof(pkt.ingress_if), "%s", options->interface_name);
        pkt.data = (uint8_t*)data;
        pkt.caplen = (size_t)header->caplen;
        pkt.len = (size_t)header->len;

        if (on_packet(&pkt, user_ctx) != 0) {
            break;
        }
    }

    pcap_close(handle);
    return 0;
}
#endif

#ifdef __linux__
static int run_socket(
    const fh_capture_options_t* options,
    fh_packet_callback_t on_packet,
    void* user_ctx,
    const volatile sig_atomic_t* stop_flag,
    char* errbuf,
    size_t errbuf_len) {
    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) {
        return write_err(errbuf, errbuf_len, "failed to create AF_PACKET socket");
    }

    struct timeval timeout;
    timeout.tv_sec = options->timeout_ms / 1000;
    timeout.tv_usec = (options->timeout_ms % 1000) * 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    (void)snprintf(ifr.ifr_name, IFNAMSIZ, "%s", options->interface_name);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        close(fd);
        return write_err(errbuf, errbuf_len, "failed to resolve interface index");
    }

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex = ifr.ifr_ifindex;
    if (bind(fd, (struct sockaddr*)&sll, sizeof(sll)) < 0) {
        close(fd);
        return write_err(errbuf, errbuf_len, "failed to bind AF_PACKET socket");
    }

    uint8_t* buf = (uint8_t*)malloc((size_t)options->snaplen);
    if (buf == NULL) {
        close(fd);
        return write_err(errbuf, errbuf_len, "failed to allocate socket receive buffer");
    }

    while (*stop_flag == 0) {
        ssize_t n = recv(fd, buf, (size_t)options->snaplen, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            free(buf);
            close(fd);
            return write_err(errbuf, errbuf_len, "socket recv failed");
        }

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

    free(buf);
    close(fd);
    return 0;
}
#endif

int fh_capture_run(
    const fh_capture_options_t* options,
    fh_packet_callback_t on_packet,
    void* user_ctx,
    const volatile sig_atomic_t* stop_flag,
    char* errbuf,
    size_t errbuf_len) {
    if (options == NULL || on_packet == NULL || stop_flag == NULL) {
        return write_err(errbuf, errbuf_len, "invalid capture arguments");
    }

    if (options->mode == NULL || options->interface_name == NULL) {
        return write_err(errbuf, errbuf_len, "mode and interface_name are required");
    }

    if (strcmp(options->mode, "pcap") == 0) {
#ifdef FRONTHAUL_ENABLE_PCAP
        return run_pcap(options, on_packet, user_ctx, stop_flag, errbuf, errbuf_len);
#else
        return write_err(errbuf, errbuf_len, "pcap mode disabled at build time");
#endif
    }

    if (strcmp(options->mode, "socket") == 0) {
#ifdef __linux__
        return run_socket(options, on_packet, user_ctx, stop_flag, errbuf, errbuf_len);
#else
        return write_err(errbuf, errbuf_len, "socket mode supported on Linux only");
#endif
    }

    return write_err(errbuf, errbuf_len, "unsupported mode (use pcap|socket)");
}
