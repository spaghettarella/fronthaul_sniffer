#pragma once

#include "fronthaul_sniffer/capture/capture_backend.hpp"

#include <string>

namespace fronthaul {

struct PcapConfig {
    std::string interface_name{"lo"};
    std::string bpf_filter;
    int snaplen{65535};
    bool promiscuous{false};
    int timeout_ms{100};
};

class PcapLiveCapture final : public ICaptureBackend {
public:
    explicit PcapLiveCapture(PcapConfig config);

    void run(const PacketHandler& on_packet, std::atomic<bool>& stop_flag) override;

private:
    PcapConfig config_;
};

}  // namespace fronthaul
