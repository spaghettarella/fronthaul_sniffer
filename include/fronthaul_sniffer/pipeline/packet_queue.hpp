#pragma once

#include "fronthaul_sniffer/core/raw_packet.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

namespace fronthaul {

struct QueueStats {
    std::size_t dropped_packets{0};
    std::size_t dropped_bytes{0};
    std::size_t queued_packets{0};
    std::size_t queued_bytes{0};
};

class PacketQueue {
public:
    PacketQueue(std::size_t max_packets, std::size_t max_bytes)
        : max_packets_(max_packets), max_bytes_(max_bytes) {}

    void push(RawPacket&& pkt) {
        std::unique_lock<std::mutex> lock(mtx_);
        const auto pkt_bytes = pkt.bytes.size();

        // Keep memory bounded: drop oldest packets until the new one fits.
        while (!queue_.empty() && ((queue_.size() >= max_packets_) || (queued_bytes_ + pkt_bytes > max_bytes_))) {
            dropped_bytes_ += queue_.front().bytes.size();
            queued_bytes_ -= queue_.front().bytes.size();
            ++dropped_packets_;
            queue_.pop_front();
        }

        if (pkt_bytes > max_bytes_) {
            dropped_bytes_ += pkt_bytes;
            ++dropped_packets_;
            return;
        }

        queued_bytes_ += pkt_bytes;
        queue_.push_back(std::move(pkt));
        cv_.notify_one();
    }

    bool pop(RawPacket& out, std::atomic<bool>& stop_flag) {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [&] { return !queue_.empty() || stop_flag.load(); });

        if (queue_.empty()) {
            return false;
        }

        queued_bytes_ -= queue_.front().bytes.size();
        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void notify_all() {
        cv_.notify_all();
    }

    QueueStats stats() const {
        std::scoped_lock<std::mutex> lock(mtx_);
        return QueueStats{dropped_packets_, dropped_bytes_, queue_.size(), queued_bytes_};
    }

private:
    std::size_t max_packets_{0};
    std::size_t max_bytes_{0};

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<RawPacket> queue_;

    std::size_t dropped_packets_{0};
    std::size_t dropped_bytes_{0};
    std::size_t queued_bytes_{0};
};

}  // namespace fronthaul
