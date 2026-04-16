#pragma once

#include "fronthaul_sniffer/capture/capture_backend.hpp"

#include <string>

namespace fronthaul {

struct CaptureOptions {
    std::string mode{"pcap"};
    std::string interface_name{"lo"};
    std::string bpf_filter;
    int snaplen{65535};
    bool promiscuous{false};
    int timeout_ms{100};
};

CapturePtr make_capture_backend(const CaptureOptions& options);

}  // namespace fronthaul
