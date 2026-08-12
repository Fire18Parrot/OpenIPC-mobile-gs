// SPDX-License-Identifier: GPL-3.0-only
//
// The whole ground station, assembled.
//
// This is the piece that replaces the SBC's init scripts: instead of wfb_rx,
// msposd, alink_gs and a video player as separate processes glued together with
// UDP sockets, one object owns the pipeline and passes buffers between stages
// directly.

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "openipc/alink.h"
#include "openipc/config.h"
#include "openipc/link_stats.h"
#include "openipc/mavlink_router.h"
#include "openipc/msp_osd.h"
#include "openipc/rtp.h"
#include "openipc/wfb_receiver.h"

namespace openipc::gs {

class PacketSource;

// Everything the Android layer needs to be told about, as it happens.
struct GroundStationCallbacks {
    // One Annex-B NAL unit, valid only for the duration of the call. The
    // consumer is expected to hand it straight to MediaCodec.
    std::function<void(const uint8_t* data, size_t size)> on_video_nal;
    // A complete OSD screen, on every DRAW_SCREEN from the air unit.
    std::function<void(const OsdFrame&)> on_osd_frame;
    // Once per stats interval.
    std::function<void(const LinkSnapshot&, const TelemetryState&)> on_stats;
    // Human-readable state changes worth putting in front of the user.
    std::function<void(const std::string&)> on_status;
};

class GroundStation {
public:
    GroundStation();
    ~GroundStation();

    GroundStation(const GroundStation&) = delete;
    GroundStation& operator=(const GroundStation&) = delete;

    // `usb_fd` is the descriptor from Android's UsbDeviceConnection, and is
    // only used when config.source is kDevourer. Ownership stays with the
    // caller; the fd must outlive the session.
    bool Start(const GsConfig& config, int usb_fd, GroundStationCallbacks callbacks);
    void Stop();
    bool running() const { return running_; }

    // Applied without restarting the link.
    void SetMavlinkEndpoints(const std::vector<MavlinkEndpoint>& endpoints);
    void SetAlinkConfig(const AlinkConfig& config);
    void SetVideoCodec(VideoCodec codec);

    // Retune the adapter while running, the way the SBC's wfb_rx would be
    // restarted on a channel change - but without dropping the session.
    bool SetChannel(int channel, Bandwidth bandwidth);

    LinkSnapshot link_stats() const { return stats_.Get(); }

    /**
     * What the RTP depacketiser found on the wire. The decoder cannot guess
     * this for itself: an H.265 stream fed to an H.264 decoder never yields a
     * keyframe, so playback stays black with data flowing.
     */
    VideoCodec detected_codec() const { return depacketizer_.codec(); }
    TelemetryState telemetry() const { return mavlink_.telemetry(); }
    std::string last_error() const;

private:
    void OnVideoPayload(const uint8_t* data, size_t size);
    void OnTelemetryPayload(const uint8_t* data, size_t size);
    void OnTunnelPayload(const uint8_t* data, size_t size);
    void StatsLoop();
    void SendUplink(uint8_t radio_port, const uint8_t* data, size_t size);
    void SetError(const std::string& message);

    GsConfig config_;
    GroundStationCallbacks callbacks_;

    std::unique_ptr<PacketSource> source_;
    std::unique_ptr<WfbReceiver> receiver_;
    RtpDepacketizer depacketizer_;
    MspOsdDecoder osd_;
    MavlinkRouter mavlink_;
    std::unique_ptr<AlinkController> alink_;
    LinkStats stats_;

    // The video mirror, for users who want the stream in another app on the
    // phone the way the SBC pushes it to 127.0.0.1:5600.
    int mirror_fd_ = -1;

    std::thread stats_thread_;
    std::atomic<bool> running_{false};

    mutable std::mutex mutex_;
    std::string last_error_;
};

}  // namespace openipc::gs
