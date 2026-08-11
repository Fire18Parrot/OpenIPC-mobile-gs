// SPDX-License-Identifier: GPL-3.0-only
//
// MAVLink framing, telemetry extraction and endpoint routing.
//
// The SBC ground station hands telemetry to a single peer configured in
// wifibroadcast.cfg (connect://127.0.0.1:14550). This router does the same job
// but fans out to several endpoints at once, so the built-in OSD, QGroundControl
// and a third-party ground control station can all be attached simultaneously -
// and anything a GCS sends back is routed up to the aircraft.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "openipc/config.h"

namespace openipc::gs {

// A framed MAVLink message. `data` points into the caller's buffer.
struct MavlinkMessage {
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint32_t msgid = 0;
    uint8_t system_id = 0;
    uint8_t component_id = 0;
    bool v2 = false;
};

// Splits a byte stream into whole MAVLink messages. Needed because TCP gives no
// message boundaries, and because a wfb payload can hold several messages.
class MavlinkFramer {
public:
    using MessageCallback = std::function<void(const MavlinkMessage&)>;

    void set_callback(MessageCallback callback) { callback_ = std::move(callback); }
    void Push(const uint8_t* data, size_t size);

    uint64_t messages() const { return messages_; }
    uint64_t dropped_bytes() const { return dropped_bytes_; }

private:
    void Reset();

    std::vector<uint8_t> buffer_;
    MessageCallback callback_;
    uint64_t messages_ = 0;
    uint64_t dropped_bytes_ = 0;
};

// The subset of telemetry the OSD needs. Extracted from the common dialect's
// most-used messages; anything else is forwarded without being interpreted.
struct TelemetryState {
    bool armed = false;
    uint8_t base_mode = 0;
    uint32_t custom_mode = 0;
    uint8_t system_status = 0;
    uint8_t mav_type = 0;

    float roll_rad = 0.0f;
    float pitch_rad = 0.0f;
    float yaw_rad = 0.0f;

    int32_t latitude_e7 = 0;
    int32_t longitude_e7 = 0;
    int32_t altitude_mm = 0;
    int32_t relative_altitude_mm = 0;
    uint8_t satellites = 0;
    uint8_t gps_fix_type = 0;

    float ground_speed_ms = 0.0f;
    float air_speed_ms = 0.0f;
    float climb_ms = 0.0f;
    float heading_deg = 0.0f;
    uint16_t throttle_pct = 0;

    float battery_voltage_v = 0.0f;
    float battery_current_a = 0.0f;
    int8_t battery_remaining_pct = -1;

    uint64_t last_update_ms = 0;
    bool valid = false;
};

// Decodes the handful of messages the OSD displays.
class TelemetryDecoder {
public:
    void Consume(const MavlinkMessage& message);
    TelemetryState Get() const;

private:
    mutable std::mutex mutex_;
    TelemetryState state_;
};

// Routes MAVLink between the aircraft (over wfb) and any number of local or
// network endpoints.
class MavlinkRouter {
public:
    // Called with data that should be sent up to the aircraft on the MAVLink
    // uplink radio port.
    using UplinkCallback = std::function<void(const uint8_t*, size_t)>;

    MavlinkRouter();
    ~MavlinkRouter();

    MavlinkRouter(const MavlinkRouter&) = delete;
    MavlinkRouter& operator=(const MavlinkRouter&) = delete;

    // Replace the endpoint set. Safe to call while running; sockets are rebuilt.
    void SetEndpoints(const std::vector<MavlinkEndpoint>& endpoints);

    void set_uplink_callback(UplinkCallback callback) { uplink_ = std::move(callback); }

    bool Start();
    void Stop();

    // Feed telemetry that arrived from the aircraft. Forwards to every enabled
    // endpoint and updates the telemetry state for the OSD.
    void FeedDownlink(const uint8_t* data, size_t size);

    TelemetryState telemetry() const { return telemetry_.Get(); }

    uint64_t downlink_bytes() const { return downlink_bytes_; }
    uint64_t uplink_bytes() const { return uplink_bytes_; }

private:
    struct Endpoint;

    void PollLoop();
    void RebuildEndpoints();
    void CloseEndpoints();
    void HandleEndpointData(Endpoint& endpoint, const uint8_t* data, size_t size);

    std::vector<MavlinkEndpoint> configured_;
    std::vector<std::unique_ptr<Endpoint>> endpoints_;
    std::mutex endpoints_mutex_;
    bool endpoints_dirty_ = true;

    MavlinkFramer downlink_framer_;
    TelemetryDecoder telemetry_;
    UplinkCallback uplink_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    // Written to from Stop() to break the poll out of its wait.
    int wake_pipe_[2] = {-1, -1};

    std::atomic<uint64_t> downlink_bytes_{0};
    std::atomic<uint64_t> uplink_bytes_{0};
};

}  // namespace openipc::gs
