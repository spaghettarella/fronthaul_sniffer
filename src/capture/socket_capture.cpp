#include "fronthaul_sniffer/capture/socket_capture.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <stdexcept>

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

namespace fronthaul {

SocketCapture::SocketCapture(SocketConfig config)
    : config_(std::move(config)) {}

void SocketCapture::run(const PacketHandler& on_packet, std::atomic<bool>& stop_flag) {
#ifndef __linux__
    (void)on_packet;
    (void)stop_flag;
    throw std::runtime_error("socket backend currently supports Linux only");
#else
    const int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) {
        throw std::runtime_error("failed to create AF_PACKET raw socket (need root/CAP_NET_RAW)");
    }

    timeval timeout{};
    timeout.tv_sec = config_.recv_timeout_ms / 1000;
    timeout.tv_usec = (config_.recv_timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    ifreq ifr{};
    std::strncpy(ifr.ifr_name, config_.interface_name.c_str(), IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        close(fd);
        throw std::runtime_error("failed to resolve interface index for " + config_.interface_name);
    }

    sockaddr_ll sll{};
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex = ifr.ifr_ifindex;
    if (bind(fd, reinterpret_cast<sockaddr*>(&sll), sizeof(sll)) < 0) {
        close(fd);
        throw std::runtime_error("failed to bind socket to interface " + config_.interface_name);
    }

    std::vector<std::uint8_t> buf(static_cast<std::size_t>(config_.snaplen));
    while (!stop_flag.load()) {
        const ssize_t n = recv(fd, buf.data(), buf.size(), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            close(fd);
            throw std::runtime_error("socket recv failed");
        }

        RawPacket pkt;
        pkt.ingress_if = config_.interface_name;
        pkt.timestamp = std::chrono::system_clock::now();
        pkt.original_len = static_cast<std::size_t>(n);
        pkt.bytes.assign(buf.begin(), buf.begin() + n);
        on_packet(std::move(pkt));
    }

    close(fd);
#endif
}

}  // namespace fronthaul
