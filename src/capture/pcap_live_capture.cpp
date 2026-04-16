#include "fronthaul_sniffer/capture/pcap_live_capture.hpp"

#include <chrono>
#include <stdexcept>

#ifdef FRONTHAUL_ENABLE_PCAP
#include <pcap/pcap.h>
#endif

namespace fronthaul {

PcapLiveCapture::PcapLiveCapture(PcapConfig config)
    : config_(std::move(config)) {}

void PcapLiveCapture::run(const PacketHandler& on_packet, std::atomic<bool>& stop_flag) {
#ifndef FRONTHAUL_ENABLE_PCAP
    (void)on_packet;
    (void)stop_flag;
    throw std::runtime_error("pcap backend disabled at build time");
#else
    char errbuf[PCAP_ERRBUF_SIZE] = {0};
    pcap_t* handle = pcap_open_live(
        config_.interface_name.c_str(),
        config_.snaplen,
        config_.promiscuous ? 1 : 0,
        config_.timeout_ms,
        errbuf);

    if (!handle) {
        throw std::runtime_error(std::string("pcap_open_live failed: ") + errbuf);
    }

    if (!config_.bpf_filter.empty()) {
        bpf_program program{};
        if (pcap_compile(handle, &program, config_.bpf_filter.c_str(), 1, PCAP_NETMASK_UNKNOWN) < 0) {
            const auto msg = std::string("pcap_compile failed: ") + pcap_geterr(handle);
            pcap_close(handle);
            throw std::runtime_error(msg);
        }

        if (pcap_setfilter(handle, &program) < 0) {
            const auto msg = std::string("pcap_setfilter failed: ") + pcap_geterr(handle);
            pcap_freecode(&program);
            pcap_close(handle);
            throw std::runtime_error(msg);
        }
        pcap_freecode(&program);
    }

    while (!stop_flag.load()) {
        pcap_pkthdr* header = nullptr;
        const u_char* data = nullptr;
        const int rc = pcap_next_ex(handle, &header, &data);

        if (rc == 0) {
            continue;
        }
        if (rc == -1) {
            const auto msg = std::string("pcap_next_ex error: ") + pcap_geterr(handle);
            pcap_close(handle);
            throw std::runtime_error(msg);
        }
        if (rc == -2) {
            break;
        }

        RawPacket pkt;
        pkt.ingress_if = config_.interface_name;
        pkt.timestamp = std::chrono::system_clock::from_time_t(header->ts.tv_sec) + std::chrono::microseconds(header->ts.tv_usec);
        pkt.original_len = header->len;
        pkt.bytes.assign(data, data + header->caplen);
        on_packet(std::move(pkt));
    }

    pcap_close(handle);
#endif
}

}  // namespace fronthaul
