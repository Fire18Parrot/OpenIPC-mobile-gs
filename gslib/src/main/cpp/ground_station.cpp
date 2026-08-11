// SPDX-License-Identifier: GPL-3.0-only

#include "ground_station.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

#include "packet_source.h"

namespace openipc::gs {
namespace {

// wfb-ng's injected 802.11 header, rebuilt here for the uplink. Kept local so
// the uplink does not depend on wfb-ng's headers leaking into this file.
constexpr uint8_t kIeee80211Header[] = {
    0x08, 0x01, 0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x57, 0x42, 0xaa, 0xbb, 0xcc, 0xdd,
    0x57, 0x42, 0xaa, 0xbb, 0xcc, 0xdd,
    0x00, 0x00,
};

constexpr int kStatsIntervalMs = 100;

}  // namespace

GroundStation::GroundStation() = default;

GroundStation::~GroundStation() { Stop(); }

void GroundStation::SetError(const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_ = message;
    }
    if (callbacks_.on_status) {
        callbacks_.on_status(message);
    }
}

std::string GroundStation::last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

bool GroundStation::Start(const GsConfig& config, int usb_fd, GroundStationCallbacks callbacks) {
    if (running_) {
        return true;
    }
    config_ = config;
    callbacks_ = std::move(callbacks);

    depacketizer_.set_callback([this](const NalUnit& nal) {
        if (callbacks_.on_video_nal) {
            callbacks_.on_video_nal(nal.data, nal.size);
        }
        if (mirror_fd_ >= 0) {
            ssize_t ignored = ::send(mirror_fd_, nal.data, nal.size, 0);
            (void)ignored;
        }
    });

    osd_.set_callback([this](const OsdFrame& frame) {
        if (callbacks_.on_osd_frame) {
            callbacks_.on_osd_frame(frame);
        }
    });

    // Anything a ground control station sends goes up on the MAVLink uplink
    // radio port.
    mavlink_.set_uplink_callback([this](const uint8_t* data, size_t size) {
        SendUplink(kPortMavlinkTx, data, size);
    });
    mavlink_.Start();

    if (!config_.video_mirror_host.empty() && config_.video_mirror_port > 0) {
        mirror_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (mirror_fd_ >= 0) {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(static_cast<uint16_t>(config_.video_mirror_port));
            if (::inet_pton(AF_INET, config_.video_mirror_host.c_str(), &addr.sin_addr) != 1 ||
                ::connect(mirror_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                ::close(mirror_fd_);
                mirror_fd_ = -1;
            }
        }
    }

    if (config_.alink.enabled) {
        alink_ = std::make_unique<AlinkController>(config_.alink);
    }

    std::string error;
    switch (config_.source) {
        case SourceKind::kUdp:
            // APFPV: RTP straight off the socket, no wfb layer at all.
            source_ = MakeUdpSource(config_.udp);
            source_->set_datagram_callback(
                [this](const uint8_t* data, size_t size) { OnVideoPayload(data, size); });
            break;

        case SourceKind::kDevourer:
        default: {
            receiver_ = std::make_unique<WfbReceiver>(config_.radio);
            try {
                receiver_->AddStream(kPortVideo, [this](const uint8_t* data, size_t size) {
                    OnVideoPayload(data, size);
                });
                receiver_->AddStream(kPortMavlinkRx, [this](const uint8_t* data, size_t size) {
                    OnTelemetryPayload(data, size);
                });
                receiver_->AddStream(kPortTunnelRx, [this](const uint8_t* data, size_t size) {
                    OnTunnelPayload(data, size);
                });
            } catch (const std::exception& e) {
                // Almost always a missing or unreadable gs.key, which is worth
                // saying plainly rather than as a generic failure.
                SetError(std::string("could not load gs.key: ") + e.what());
                receiver_.reset();
                mavlink_.Stop();
                return false;
            }

            source_ = MakeDevourerSource(config_.radio, usb_fd);
            source_->set_frame_callback(
                [this](const uint8_t* frame, size_t size, const FrameMeta& meta) {
                    receiver_->ProcessFrame(frame, size, meta);
                });
            break;
        }
    }

    if (!source_->Start(&error)) {
        SetError(error);
        source_.reset();
        receiver_.reset();
        mavlink_.Stop();
        return false;
    }

    running_ = true;
    stats_thread_ = std::thread([this] { StatsLoop(); });
    if (callbacks_.on_status) {
        callbacks_.on_status("ground station running");
    }
    return true;
}

void GroundStation::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (stats_thread_.joinable()) {
        stats_thread_.join();
    }
    if (source_) {
        source_->Stop();
        source_.reset();
    }
    mavlink_.Stop();
    receiver_.reset();
    alink_.reset();
    if (mirror_fd_ >= 0) {
        ::close(mirror_fd_);
        mirror_fd_ = -1;
    }
}

void GroundStation::OnVideoPayload(const uint8_t* data, size_t size) {
    depacketizer_.Push(data, size);
}

void GroundStation::OnTelemetryPayload(const uint8_t* data, size_t size) {
    // The telemetry port carries MAVLink, and on OpenIPC air units running
    // msposd it carries the MSP DisplayPort stream too. Both parsers are
    // resynchronising, so feeding each the whole stream is safe and saves
    // having to know which the air unit is configured for.
    mavlink_.FeedDownlink(data, size);
    osd_.Push(data, size);
}

void GroundStation::OnTunnelPayload(const uint8_t* data, size_t size) {
    // The tunnel is what adaptive-link and msposd use when the air unit is
    // configured to keep them off the MAVLink stream.
    osd_.Push(data, size);
}

void GroundStation::SendUplink(uint8_t radio_port, const uint8_t* data, size_t size) {
    if (!source_ || config_.source != SourceKind::kDevourer) {
        return;
    }
    // Build the 802.11 frame wfb-ng expects, with the channel_id in both
    // addresses. The payload is sent unencrypted on the uplink here, which
    // matches how the SBC's alink_gs reaches the air unit through the tunnel.
    const uint32_t channel_id = ChannelId(config_.radio.link_id, radio_port);
    std::vector<uint8_t> frame(sizeof(kIeee80211Header));
    std::memcpy(frame.data(), kIeee80211Header, sizeof(kIeee80211Header));

    const uint8_t channel_bytes[4] = {
        static_cast<uint8_t>((channel_id >> 24) & 0xff),
        static_cast<uint8_t>((channel_id >> 16) & 0xff),
        static_cast<uint8_t>((channel_id >> 8) & 0xff),
        static_cast<uint8_t>(channel_id & 0xff),
    };
    std::memcpy(frame.data() + 12, channel_bytes, sizeof(channel_bytes));
    std::memcpy(frame.data() + 18, channel_bytes, sizeof(channel_bytes));
    frame.insert(frame.end(), data, data + size);

    source_->Transmit(frame.data(), frame.size());
}

void GroundStation::StatsLoop() {
    uint64_t next_alink_ms = NowMs();

    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kStatsIntervalMs));
        if (!running_) {
            break;
        }

        LinkSnapshot snapshot;
        if (receiver_) {
            snapshot = receiver_->TakeSnapshot();
        } else {
            snapshot.timestamp_ms = NowMs();
        }
        stats_.Publish(snapshot);

        const TelemetryState telemetry = mavlink_.telemetry();
        if (callbacks_.on_stats) {
            callbacks_.on_stats(snapshot, telemetry);
        }

        if (alink_ && receiver_) {
            const uint64_t now = NowMs();
            if (now >= next_alink_ms) {
                next_alink_ms = now + static_cast<uint64_t>(config_.alink.interval_ms);
                const AlinkResult result =
                    alink_->Update(snapshot, static_cast<uint64_t>(now / 1000));
                const std::string framed = AlinkController::Frame(result.message);
                SendUplink(kPortTunnelTx, reinterpret_cast<const uint8_t*>(framed.data()),
                           framed.size());
            }
        }
    }
}

void GroundStation::SetMavlinkEndpoints(const std::vector<MavlinkEndpoint>& endpoints) {
    mavlink_.SetEndpoints(endpoints);
}

void GroundStation::SetAlinkConfig(const AlinkConfig& config) {
    config_.alink = config;
    if (!config.enabled) {
        alink_.reset();
    } else if (alink_) {
        alink_->set_config(config);
    } else {
        alink_ = std::make_unique<AlinkController>(config);
    }
}

void GroundStation::SetVideoCodec(VideoCodec codec) { depacketizer_.set_codec(codec); }

bool GroundStation::SetChannel(int channel, Bandwidth bandwidth) {
    if (!source_) {
        return false;
    }
    config_.radio.channel = channel;
    config_.radio.bandwidth = bandwidth;
    return source_->SetChannel(channel, bandwidth);
}

}  // namespace openipc::gs
