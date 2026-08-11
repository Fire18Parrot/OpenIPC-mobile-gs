// SPDX-License-Identifier: GPL-3.0-only

#include "openipc/mavlink_router.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>

#include "openipc/link_stats.h"

namespace openipc::gs {
namespace {

constexpr uint8_t kMavlinkV1Magic = 0xfe;
constexpr uint8_t kMavlinkV2Magic = 0xfd;
constexpr size_t kMavlinkV1HeaderSize = 6;
constexpr size_t kMavlinkV2HeaderSize = 10;
constexpr size_t kChecksumSize = 2;
constexpr size_t kSignatureSize = 13;
constexpr size_t kMaxFrameSize = 280;

// MAVLink common dialect message ids we interpret for the OSD.
enum : uint32_t {
    kMsgHeartbeat = 0,
    kMsgSysStatus = 1,
    kMsgAttitude = 30,
    kMsgGlobalPositionInt = 33,
    kMsgGpsRawInt = 24,
    kMsgVfrHud = 74,
    kMsgBatteryStatus = 147,
};

template <typename T>
T ReadLe(const uint8_t* p) {
    T value;
    std::memcpy(&value, p, sizeof(T));
    return value;
}

int SetNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

bool ResolveIpv4(const std::string& host, int port, sockaddr_in* out) {
    std::memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(static_cast<uint16_t>(port));
    if (host.empty() || host == "0.0.0.0") {
        out->sin_addr.s_addr = INADDR_ANY;
        return true;
    }
    return inet_pton(AF_INET, host.c_str(), &out->sin_addr) == 1;
}

}  // namespace

// --- MavlinkFramer -------------------------------------------------------

void MavlinkFramer::Reset() { buffer_.clear(); }

void MavlinkFramer::Push(const uint8_t* data, size_t size) {
    buffer_.insert(buffer_.end(), data, data + size);

    size_t offset = 0;
    while (offset < buffer_.size()) {
        const uint8_t magic = buffer_[offset];
        if (magic != kMavlinkV1Magic && magic != kMavlinkV2Magic) {
            ++offset;
            ++dropped_bytes_;
            continue;
        }

        const bool v2 = magic == kMavlinkV2Magic;
        const size_t header = v2 ? kMavlinkV2HeaderSize : kMavlinkV1HeaderSize;
        if (buffer_.size() - offset < header) {
            break;  // need more bytes to know the length
        }

        const size_t payload_len = buffer_[offset + 1];
        size_t frame_len = header + payload_len + kChecksumSize;
        if (v2) {
            const uint8_t incompat_flags = buffer_[offset + 2];
            if (incompat_flags & 0x01) {  // MAVLINK_IFLAG_SIGNED
                frame_len += kSignatureSize;
            }
        }
        if (frame_len > kMaxFrameSize) {
            ++offset;
            ++dropped_bytes_;
            continue;
        }
        if (buffer_.size() - offset < frame_len) {
            break;  // partial frame, wait for the rest
        }

        MavlinkMessage message;
        message.data = buffer_.data() + offset;
        message.size = frame_len;
        message.v2 = v2;
        if (v2) {
            message.system_id = buffer_[offset + 5];
            message.component_id = buffer_[offset + 6];
            message.msgid = static_cast<uint32_t>(buffer_[offset + 7]) |
                            (static_cast<uint32_t>(buffer_[offset + 8]) << 8) |
                            (static_cast<uint32_t>(buffer_[offset + 9]) << 16);
        } else {
            message.system_id = buffer_[offset + 3];
            message.component_id = buffer_[offset + 4];
            message.msgid = buffer_[offset + 5];
        }

        ++messages_;
        if (callback_) {
            callback_(message);
        }
        offset += frame_len;
    }

    if (offset > 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<long>(offset));
    }
    // Guard against a stream that never syncs pinning memory forever.
    if (buffer_.size() > 4 * kMaxFrameSize) {
        dropped_bytes_ += buffer_.size();
        Reset();
    }
}

// --- TelemetryDecoder ----------------------------------------------------

void TelemetryDecoder::Consume(const MavlinkMessage& message) {
    const size_t header = message.v2 ? kMavlinkV2HeaderSize : kMavlinkV1HeaderSize;
    if (message.size < header + kChecksumSize) {
        return;
    }
    const uint8_t* payload = message.data + header;
    const size_t payload_len = message.size - header - kChecksumSize;

    std::lock_guard<std::mutex> lock(mutex_);
    state_.valid = true;
    state_.last_update_ms = NowMs();

    switch (message.msgid) {
        case kMsgHeartbeat:
            // custom_mode(4) type(1) autopilot(1) base_mode(1) system_status(1)
            if (payload_len >= 8) {
                state_.custom_mode = ReadLe<uint32_t>(payload);
                state_.mav_type = payload[4];
                state_.base_mode = payload[6];
                state_.system_status = payload[7];
                // MAV_MODE_FLAG_SAFETY_ARMED
                state_.armed = (state_.base_mode & 0x80) != 0;
            }
            break;

        case kMsgSysStatus:
            // ... voltage_battery at offset 14 (u16 mV), current_battery 16
            // (i16 cA), battery_remaining at 30 (i8)
            if (payload_len >= 31) {
                state_.battery_voltage_v = ReadLe<uint16_t>(payload + 14) / 1000.0f;
                state_.battery_current_a = ReadLe<int16_t>(payload + 16) / 100.0f;
                state_.battery_remaining_pct = static_cast<int8_t>(payload[30]);
            }
            break;

        case kMsgAttitude:
            // time_boot_ms(4) roll(4) pitch(4) yaw(4) ...
            if (payload_len >= 16) {
                state_.roll_rad = ReadLe<float>(payload + 4);
                state_.pitch_rad = ReadLe<float>(payload + 8);
                state_.yaw_rad = ReadLe<float>(payload + 12);
            }
            break;

        case kMsgGlobalPositionInt:
            // time_boot_ms(4) lat(4) lon(4) alt(4) relative_alt(4) vx vy vz hdg
            if (payload_len >= 28) {
                state_.latitude_e7 = ReadLe<int32_t>(payload + 4);
                state_.longitude_e7 = ReadLe<int32_t>(payload + 8);
                state_.altitude_mm = ReadLe<int32_t>(payload + 12);
                state_.relative_altitude_mm = ReadLe<int32_t>(payload + 16);
                const uint16_t hdg = ReadLe<uint16_t>(payload + 26);
                if (hdg != UINT16_MAX) {
                    state_.heading_deg = hdg / 100.0f;
                }
            }
            break;

        case kMsgGpsRawInt:
            // time_usec(8) lat(4) lon(4) alt(4) eph(2) epv(2) vel(2) cog(2)
            // fix_type(1) satellites_visible(1)
            if (payload_len >= 30) {
                state_.gps_fix_type = payload[28];
                state_.satellites = payload[29];
            }
            break;

        case kMsgVfrHud:
            // airspeed(4) groundspeed(4) alt(4) climb(4) heading(2) throttle(2)
            if (payload_len >= 20) {
                state_.air_speed_ms = ReadLe<float>(payload);
                state_.ground_speed_ms = ReadLe<float>(payload + 4);
                state_.climb_ms = ReadLe<float>(payload + 12);
                state_.throttle_pct = ReadLe<uint16_t>(payload + 18);
            }
            break;

        case kMsgBatteryStatus:
            // id(1) battery_function(1) type(1) temperature(2) voltages[10](20)
            // current_battery(2) current_consumed(4) energy_consumed(4)
            // battery_remaining(1)  -- v2 extension fields follow
            if (payload_len >= 36) {
                const uint16_t cell_mv = ReadLe<uint16_t>(payload + 5);
                if (cell_mv != UINT16_MAX) {
                    state_.battery_voltage_v = cell_mv / 1000.0f;
                }
                state_.battery_current_a = ReadLe<int16_t>(payload + 25) / 100.0f;
                state_.battery_remaining_pct = static_cast<int8_t>(payload[35]);
            }
            break;

        default:
            break;
    }
}

TelemetryState TelemetryDecoder::Get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

// --- MavlinkRouter -------------------------------------------------------

struct MavlinkRouter::Endpoint {
    MavlinkEndpoint config;
    int fd = -1;
    // TCP server: accepted client sockets.
    std::vector<int> clients;
    // UDP: the peer we send to. For kUdpServer this is learned on first receipt.
    sockaddr_in peer{};
    bool have_peer = false;
    // TCP framing is per-connection; UDP datagrams are already whole.
    MavlinkFramer framer;

    ~Endpoint() {
        for (int client : clients) {
            ::close(client);
        }
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

MavlinkRouter::MavlinkRouter() {
    downlink_framer_.set_callback([this](const MavlinkMessage& message) { telemetry_.Consume(message); });
}

MavlinkRouter::~MavlinkRouter() { Stop(); }

void MavlinkRouter::SetEndpoints(const std::vector<MavlinkEndpoint>& endpoints) {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    configured_ = endpoints;
    endpoints_dirty_ = true;
}

bool MavlinkRouter::Start() {
    if (running_.exchange(true)) {
        return true;
    }
    if (pipe(wake_pipe_) != 0) {
        running_ = false;
        return false;
    }
    SetNonBlocking(wake_pipe_[0]);
    SetNonBlocking(wake_pipe_[1]);
    thread_ = std::thread([this] { PollLoop(); });
    return true;
}

void MavlinkRouter::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (wake_pipe_[1] >= 0) {
        const uint8_t byte = 0;
        ssize_t ignored = ::write(wake_pipe_[1], &byte, 1);
        (void)ignored;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    for (int& fd : wake_pipe_) {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
    CloseEndpoints();
}

void MavlinkRouter::CloseEndpoints() {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    endpoints_.clear();
}

void MavlinkRouter::RebuildEndpoints() {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    if (!endpoints_dirty_) {
        return;
    }
    endpoints_dirty_ = false;
    endpoints_.clear();

    for (const MavlinkEndpoint& config : configured_) {
        if (!config.enabled) {
            continue;
        }
        auto endpoint = std::make_unique<Endpoint>();
        endpoint->config = config;

        if (config.kind == MavlinkEndpoint::Kind::kTcpServer) {
            endpoint->fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (endpoint->fd < 0) {
                continue;
            }
            const int one = 1;
            ::setsockopt(endpoint->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            sockaddr_in addr{};
            if (!ResolveIpv4(config.host, config.port, &addr)) {
                continue;
            }
            if (::bind(endpoint->fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
                ::listen(endpoint->fd, 4) != 0) {
                continue;
            }
            SetNonBlocking(endpoint->fd);
        } else {
            endpoint->fd = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (endpoint->fd < 0) {
                continue;
            }
            SetNonBlocking(endpoint->fd);
            if (config.kind == MavlinkEndpoint::Kind::kUdpServer) {
                const int one = 1;
                ::setsockopt(endpoint->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
                sockaddr_in addr{};
                if (!ResolveIpv4(config.host, config.port, &addr)) {
                    continue;
                }
                if (::bind(endpoint->fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                    continue;
                }
            } else {
                // kUdpOut: fixed destination, known up front.
                if (!ResolveIpv4(config.host, config.port, &endpoint->peer)) {
                    continue;
                }
                endpoint->have_peer = true;
            }
        }
        endpoints_.push_back(std::move(endpoint));
    }
}

void MavlinkRouter::HandleEndpointData(Endpoint& endpoint, const uint8_t* data, size_t size) {
    if (!endpoint.config.allow_uplink || !uplink_) {
        return;
    }
    // Re-frame before sending up: the radio link is precious, and forwarding
    // half a message would only be discarded by the aircraft.
    endpoint.framer.set_callback([this](const MavlinkMessage& message) {
        uplink_bytes_ += message.size;
        uplink_(message.data, message.size);
    });
    endpoint.framer.Push(data, size);
}

void MavlinkRouter::PollLoop() {
    // Each polled descriptor is tagged with what it belongs to, so no index
    // arithmetic has to stay in step with the order things were added in.
    struct Slot {
        Endpoint* endpoint;
        int fd;
        bool is_client;
    };

    std::vector<pollfd> fds;
    std::vector<Slot> slots;
    std::vector<uint8_t> buffer(2048);

    while (running_) {
        RebuildEndpoints();

        fds.clear();
        slots.clear();
        fds.push_back(pollfd{wake_pipe_[0], POLLIN, 0});
        slots.push_back(Slot{nullptr, wake_pipe_[0], false});

        {
            std::lock_guard<std::mutex> lock(endpoints_mutex_);
            for (const auto& endpoint : endpoints_) {
                if (endpoint->fd < 0) {
                    continue;
                }
                fds.push_back(pollfd{endpoint->fd, POLLIN, 0});
                slots.push_back(Slot{endpoint.get(), endpoint->fd, false});
                for (int client : endpoint->clients) {
                    fds.push_back(pollfd{client, POLLIN, 0});
                    slots.push_back(Slot{endpoint.get(), client, true});
                }
            }
        }

        const int ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), 200);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (!running_) {
            break;
        }
        if (ready == 0) {
            continue;
        }

        if (fds[0].revents & POLLIN) {
            uint8_t drain[64];
            while (::read(wake_pipe_[0], drain, sizeof(drain)) > 0) {
            }
            continue;
        }

        // Closing a client invalidates nothing else, but the endpoint list must
        // not be rebuilt underneath us while we walk it.
        std::lock_guard<std::mutex> lock(endpoints_mutex_);
        std::vector<int> to_close;

        for (size_t i = 1; i < fds.size(); ++i) {
            const short revents = fds[i].revents;
            if (revents == 0) {
                continue;
            }
            const Slot& slot = slots[i];
            // The endpoint may have been torn down by a concurrent
            // SetEndpoints; skip anything no longer present.
            bool still_present = false;
            for (const auto& endpoint : endpoints_) {
                if (endpoint.get() == slot.endpoint) {
                    still_present = true;
                    break;
                }
            }
            if (!still_present) {
                continue;
            }

            if (slot.is_client) {
                if (revents & (POLLERR | POLLHUP)) {
                    to_close.push_back(slot.fd);
                    continue;
                }
                const ssize_t received = ::recv(slot.fd, buffer.data(), buffer.size(), 0);
                if (received > 0) {
                    HandleEndpointData(*slot.endpoint, buffer.data(), static_cast<size_t>(received));
                } else if (received == 0) {
                    to_close.push_back(slot.fd);
                }
                continue;
            }

            if (!(revents & POLLIN)) {
                continue;
            }
            if (slot.endpoint->config.kind == MavlinkEndpoint::Kind::kTcpServer) {
                const int client = ::accept(slot.fd, nullptr, nullptr);
                if (client >= 0) {
                    SetNonBlocking(client);
                    const int one = 1;
                    ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    slot.endpoint->clients.push_back(client);
                }
            } else {
                sockaddr_in from{};
                socklen_t from_len = sizeof(from);
                const ssize_t received = ::recvfrom(slot.fd, buffer.data(), buffer.size(), 0,
                                                    reinterpret_cast<sockaddr*>(&from), &from_len);
                if (received > 0) {
                    if (slot.endpoint->config.kind == MavlinkEndpoint::Kind::kUdpServer) {
                        slot.endpoint->peer = from;
                        slot.endpoint->have_peer = true;
                    }
                    HandleEndpointData(*slot.endpoint, buffer.data(), static_cast<size_t>(received));
                }
            }
        }

        for (int fd : to_close) {
            for (const auto& endpoint : endpoints_) {
                auto it = std::find(endpoint->clients.begin(), endpoint->clients.end(), fd);
                if (it != endpoint->clients.end()) {
                    ::close(*it);
                    endpoint->clients.erase(it);
                    break;
                }
            }
        }
    }
}

void MavlinkRouter::FeedDownlink(const uint8_t* data, size_t size) {
    if (size == 0) {
        return;
    }
    downlink_bytes_ += size;
    downlink_framer_.Push(data, size);

    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    for (const auto& endpoint : endpoints_) {
        if (endpoint->fd < 0) {
            continue;
        }
        if (endpoint->config.kind == MavlinkEndpoint::Kind::kTcpServer) {
            for (int client : endpoint->clients) {
                ssize_t ignored = ::send(client, data, size, MSG_NOSIGNAL);
                (void)ignored;
            }
        } else if (endpoint->have_peer) {
            ssize_t ignored = ::sendto(endpoint->fd, data, size, 0,
                                       reinterpret_cast<const sockaddr*>(&endpoint->peer),
                                       sizeof(endpoint->peer));
            (void)ignored;
        }
    }
}

}  // namespace openipc::gs
