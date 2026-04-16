#include "fronthaul_sniffer/capture/capture_factory.hpp"

#include "fronthaul_sniffer/capture/pcap_live_capture.hpp"
#include "fronthaul_sniffer/capture/socket_capture.hpp"

#include <stdexcept>

namespace fronthaul {

CapturePtr make_capture_backend(const CaptureOptions& options) {
    if (options.mode == "pcap") {
        PcapConfig cfg;
        cfg.interface_name = options.interface_name;
        cfg.bpf_filter = options.bpf_filter;
        cfg.snaplen = options.snaplen;
        cfg.promiscuous = options.promiscuous;
        cfg.timeout_ms = options.timeout_ms;
        return std::make_unique<PcapLiveCapture>(cfg);
    }

    if (options.mode == "socket") {
        SocketConfig cfg;
        cfg.interface_name = options.interface_name;
        cfg.recv_timeout_ms = options.timeout_ms;
        cfg.snaplen = options.snaplen;
        return std::make_unique<SocketCapture>(cfg);
    }

    throw std::invalid_argument("Unsupported capture mode: " + options.mode);
}

}  // namespace fronthaul
