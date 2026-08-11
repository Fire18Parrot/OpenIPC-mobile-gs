// SPDX-License-Identifier: GPL-3.0-only
//
// APFPV mode: the air unit is an ordinary Wi-Fi access point pushing RTP to a
// UDP port. The phone joins its network and reads the socket - no adapter, no
// wfb layer, no keys. Lower range than wfb-ng, but it works with hardware the
// user already owns, which is the whole point of the mode.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "packet_source.h"

namespace openipc::gs {
namespace {

class UdpSource : public PacketSource {
public:
    explicit UdpSource(const UdpSourceConfig& config) : config_(config) {}

    ~UdpSource() override { Stop(); }

    bool Start(std::string* error) override {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) {
            *error = "could not create the video socket";
            return false;
        }

        const int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        // A generous receive buffer: video bursts on every keyframe, and a
        // dropped datagram here is a visible artefact.
        const int receive_buffer = 1 << 20;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(config_.video_port));
        if (config_.bind_addr.empty() || config_.bind_addr == "0.0.0.0") {
            addr.sin_addr.s_addr = INADDR_ANY;
        } else if (::inet_pton(AF_INET, config_.bind_addr.c_str(), &addr.sin_addr) != 1) {
            *error = "invalid bind address: " + config_.bind_addr;
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            *error = "could not bind UDP port " + std::to_string(config_.video_port);
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        running_ = true;
        thread_ = std::thread([this] { Loop(); });
        return true;
    }

    void Stop() override {
        if (!running_.exchange(false)) {
            return;
        }
        // Shutting the socket down breaks the blocking poll immediately.
        if (fd_ >= 0) {
            ::shutdown(fd_, SHUT_RDWR);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    void Loop() {
        std::vector<uint8_t> buffer(2048);
        while (running_) {
            pollfd entry{fd_, POLLIN, 0};
            const int ready = ::poll(&entry, 1, 200);
            if (ready <= 0) {
                continue;
            }
            const ssize_t received = ::recv(fd_, buffer.data(), buffer.size(), 0);
            if (received <= 0) {
                continue;
            }
            if (on_datagram_) {
                on_datagram_(buffer.data(), static_cast<size_t>(received));
            }
        }
    }

    UdpSourceConfig config_;
    int fd_ = -1;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace

std::unique_ptr<PacketSource> MakeUdpSource(const UdpSourceConfig& config) {
    return std::make_unique<UdpSource>(config);
}

}  // namespace openipc::gs
