// SPDX-License-Identifier: GPL-3.0-only
//
// MAVLink framing, telemetry extraction, and the endpoint router an external
// ground control station attaches to. The router tests use real sockets on the
// loopback interface so the code under test is the code that ships.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "openipc/mavlink_router.h"
#include "test_util.h"

namespace {

using openipc::gs::MavlinkEndpoint;
using openipc::gs::MavlinkFramer;
using openipc::gs::MavlinkMessage;
using openipc::gs::MavlinkRouter;
using openipc::gs::TelemetryDecoder;
using openipc::gs::TelemetryState;

// Build a MAVLink v1 frame. The checksum is not verified by the framer - the
// radio link already authenticates - so it is left as filler.
std::vector<uint8_t> BuildV1(uint8_t sequence, uint8_t system_id, uint8_t msgid,
                             const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame = {0xfe, static_cast<uint8_t>(payload.size()), sequence,
                                  system_id, 1, msgid};
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(0x00);
    frame.push_back(0x00);
    return frame;
}

std::vector<uint8_t> BuildV2(uint8_t sequence, uint8_t system_id, uint32_t msgid,
                             const std::vector<uint8_t>& payload, bool signed_frame = false) {
    std::vector<uint8_t> frame = {0xfd,
                                  static_cast<uint8_t>(payload.size()),
                                  static_cast<uint8_t>(signed_frame ? 0x01 : 0x00),
                                  0x00,
                                  sequence,
                                  system_id,
                                  1,
                                  static_cast<uint8_t>(msgid & 0xff),
                                  static_cast<uint8_t>((msgid >> 8) & 0xff),
                                  static_cast<uint8_t>((msgid >> 16) & 0xff)};
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(0x00);
    frame.push_back(0x00);
    if (signed_frame) {
        frame.insert(frame.end(), 13, 0x00);
    }
    return frame;
}

template <typename T>
void AppendLe(std::vector<uint8_t>* out, T value) {
    uint8_t bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T));
    out->insert(out->end(), bytes, bytes + sizeof(T));
}

int OpenUdpSocket(int* port) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    *port = ntohs(addr.sin_port);

    timeval timeout{};
    timeout.tv_sec = 2;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    return fd;
}

int FreePort() {
    int port = 0;
    const int fd = OpenUdpSocket(&port);
    ::close(fd);
    return port;
}

}  // namespace

int main() {
    // --- Framing: whole, split and concatenated messages -------------------
    {
        MavlinkFramer framer;
        std::vector<uint32_t> ids;
        framer.set_callback([&](const MavlinkMessage& message) { ids.push_back(message.msgid); });

        const auto heartbeat = BuildV1(0, 1, 0, std::vector<uint8_t>(9, 0));
        const auto attitude = BuildV2(1, 1, 30, std::vector<uint8_t>(28, 0));

        // Two messages in one buffer, as a wfb payload can carry.
        std::vector<uint8_t> both = heartbeat;
        both.insert(both.end(), attitude.begin(), attitude.end());
        framer.Push(both.data(), both.size());
        CHECK_EQ(ids.size(), static_cast<size_t>(2));
        CHECK_EQ(ids[0], static_cast<uint32_t>(0));
        CHECK_EQ(ids[1], static_cast<uint32_t>(30));

        // One message split across reads, as TCP delivers.
        ids.clear();
        framer.Push(heartbeat.data(), 3);
        CHECK_EQ(ids.size(), static_cast<size_t>(0));
        framer.Push(heartbeat.data() + 3, heartbeat.size() - 3);
        CHECK_EQ(ids.size(), static_cast<size_t>(1));

        // Leading garbage is skipped.
        ids.clear();
        std::vector<uint8_t> noisy = {0x11, 0x22, 0x33};
        noisy.insert(noisy.end(), heartbeat.begin(), heartbeat.end());
        framer.Push(noisy.data(), noisy.size());
        CHECK_EQ(ids.size(), static_cast<size_t>(1));
        CHECK(framer.dropped_bytes() >= 3);
    }

    // --- A signed v2 frame is 13 bytes longer -----------------------------
    {
        MavlinkFramer framer;
        int count = 0;
        size_t size = 0;
        framer.set_callback([&](const MavlinkMessage& message) {
            ++count;
            size = message.size;
        });
        const auto signed_frame = BuildV2(0, 1, 30, std::vector<uint8_t>(28, 0), true);
        framer.Push(signed_frame.data(), signed_frame.size());
        CHECK_EQ(count, 1);
        CHECK_EQ(size, signed_frame.size());
    }

    // --- Telemetry extraction ---------------------------------------------
    {
        TelemetryDecoder decoder;
        MavlinkFramer framer;
        framer.set_callback([&](const MavlinkMessage& message) { decoder.Consume(message); });

        // HEARTBEAT: custom_mode, type, autopilot, base_mode, system_status.
        std::vector<uint8_t> heartbeat;
        AppendLe<uint32_t>(&heartbeat, 5);
        heartbeat.push_back(2);     // MAV_TYPE_QUADROTOR
        heartbeat.push_back(3);     // autopilot
        heartbeat.push_back(0x80);  // base_mode with SAFETY_ARMED
        heartbeat.push_back(4);     // system_status
        heartbeat.push_back(3);     // mavlink_version
        const auto heartbeat_frame = BuildV1(0, 1, 0, heartbeat);
        framer.Push(heartbeat_frame.data(), heartbeat_frame.size());

        TelemetryState state = decoder.Get();
        CHECK(state.armed);
        CHECK_EQ(static_cast<int>(state.mav_type), 2);
        CHECK_EQ(state.custom_mode, static_cast<uint32_t>(5));

        // ATTITUDE: time_boot_ms then roll, pitch, yaw as floats.
        std::vector<uint8_t> attitude;
        AppendLe<uint32_t>(&attitude, 1000);
        AppendLe<float>(&attitude, 0.5f);
        AppendLe<float>(&attitude, -0.25f);
        AppendLe<float>(&attitude, 1.5f);
        AppendLe<float>(&attitude, 0.0f);
        AppendLe<float>(&attitude, 0.0f);
        AppendLe<float>(&attitude, 0.0f);
        const auto attitude_frame = BuildV2(1, 1, 30, attitude);
        framer.Push(attitude_frame.data(), attitude_frame.size());

        state = decoder.Get();
        CHECK_NEAR(state.roll_rad, 0.5, 1e-6);
        CHECK_NEAR(state.pitch_rad, -0.25, 1e-6);
        CHECK_NEAR(state.yaw_rad, 1.5, 1e-6);

        // GLOBAL_POSITION_INT.
        std::vector<uint8_t> position;
        AppendLe<uint32_t>(&position, 2000);
        AppendLe<int32_t>(&position, 473566000);
        AppendLe<int32_t>(&position, 85278000);
        AppendLe<int32_t>(&position, 120000);
        AppendLe<int32_t>(&position, 35000);
        AppendLe<int16_t>(&position, 0);
        AppendLe<int16_t>(&position, 0);
        AppendLe<int16_t>(&position, 0);
        AppendLe<uint16_t>(&position, 18000);  // heading, centidegrees
        const auto position_frame = BuildV2(2, 1, 33, position);
        framer.Push(position_frame.data(), position_frame.size());

        state = decoder.Get();
        CHECK_EQ(state.latitude_e7, 473566000);
        CHECK_EQ(state.relative_altitude_mm, 35000);
        CHECK_NEAR(state.heading_deg, 180.0, 1e-3);

        // GPS_RAW_INT: fix type and satellite count sit at the end.
        std::vector<uint8_t> gps;
        AppendLe<uint64_t>(&gps, 0);
        AppendLe<int32_t>(&gps, 0);
        AppendLe<int32_t>(&gps, 0);
        AppendLe<int32_t>(&gps, 0);
        AppendLe<uint16_t>(&gps, 0);
        AppendLe<uint16_t>(&gps, 0);
        AppendLe<uint16_t>(&gps, 0);
        AppendLe<uint16_t>(&gps, 0);
        gps.push_back(3);   // fix_type: 3D
        gps.push_back(14);  // satellites
        const auto gps_frame = BuildV2(3, 1, 24, gps);
        framer.Push(gps_frame.data(), gps_frame.size());

        state = decoder.Get();
        CHECK_EQ(static_cast<int>(state.gps_fix_type), 3);
        CHECK_EQ(static_cast<int>(state.satellites), 14);

        // VFR_HUD.
        std::vector<uint8_t> hud;
        AppendLe<float>(&hud, 12.0f);
        AppendLe<float>(&hud, 10.0f);
        AppendLe<float>(&hud, 100.0f);
        AppendLe<float>(&hud, 1.5f);
        AppendLe<int16_t>(&hud, 90);
        AppendLe<uint16_t>(&hud, 55);
        const auto hud_frame = BuildV2(4, 1, 74, hud);
        framer.Push(hud_frame.data(), hud_frame.size());

        state = decoder.Get();
        CHECK_NEAR(state.air_speed_ms, 12.0, 1e-6);
        CHECK_NEAR(state.ground_speed_ms, 10.0, 1e-6);
        CHECK_NEAR(state.climb_ms, 1.5, 1e-6);
        CHECK_EQ(static_cast<int>(state.throttle_pct), 55);
    }

    // --- Downlink fans out to a UDP endpoint ------------------------------
    {
        int listen_port = 0;
        const int listener = OpenUdpSocket(&listen_port);

        MavlinkRouter router;
        MavlinkEndpoint endpoint;
        endpoint.kind = MavlinkEndpoint::Kind::kUdpOut;
        endpoint.host = "127.0.0.1";
        endpoint.port = listen_port;
        router.SetEndpoints({endpoint});
        CHECK(router.Start());
        // Give the poll loop a moment to build its sockets.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        const auto heartbeat = BuildV1(0, 1, 0, std::vector<uint8_t>(9, 0));
        router.FeedDownlink(heartbeat.data(), heartbeat.size());

        uint8_t buffer[512];
        const ssize_t received = ::recv(listener, buffer, sizeof(buffer), 0);
        CHECK(received > 0);
        CHECK_EQ(static_cast<size_t>(received), heartbeat.size());
        CHECK(std::memcmp(buffer, heartbeat.data(), heartbeat.size()) == 0);

        router.Stop();
        ::close(listener);
    }

    // --- A UDP-server endpoint learns its peer and routes uplink ----------
    //
    // This is the path an external ground control station uses: it sends to us
    // first, we learn where it lives, and anything it sends goes up to the
    // aircraft while telemetry comes back down to it.
    {
        const int server_port = FreePort();

        MavlinkRouter router;
        std::vector<std::vector<uint8_t>> uplink;
        router.set_uplink_callback([&](const uint8_t* data, size_t size) {
            uplink.emplace_back(data, data + size);
        });

        MavlinkEndpoint endpoint;
        endpoint.kind = MavlinkEndpoint::Kind::kUdpServer;
        endpoint.host = "127.0.0.1";
        endpoint.port = server_port;
        endpoint.allow_uplink = true;
        router.SetEndpoints({endpoint});
        CHECK(router.Start());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        int gcs_port = 0;
        const int gcs = OpenUdpSocket(&gcs_port);
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(static_cast<uint16_t>(server_port));
        ::inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);

        // The GCS sends a command up.
        const auto command = BuildV2(0, 255, 76 /* COMMAND_LONG */, std::vector<uint8_t>(33, 0));
        ::sendto(gcs, command.data(), command.size(), 0, reinterpret_cast<sockaddr*>(&target),
                 sizeof(target));

        for (int i = 0; i < 50 && uplink.empty(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK_EQ(uplink.size(), static_cast<size_t>(1));
        if (!uplink.empty()) {
            CHECK(uplink[0] == command);
        }

        // Telemetry now flows back to the peer we learned.
        const auto heartbeat = BuildV1(0, 1, 0, std::vector<uint8_t>(9, 0));
        router.FeedDownlink(heartbeat.data(), heartbeat.size());

        uint8_t buffer[512];
        const ssize_t received = ::recv(gcs, buffer, sizeof(buffer), 0);
        CHECK(received > 0);
        CHECK_EQ(static_cast<size_t>(received), heartbeat.size());

        router.Stop();
        ::close(gcs);
    }

    // --- A TCP-server endpoint serves a connecting GCS --------------------
    {
        const int server_port = FreePort();

        MavlinkRouter router;
        std::vector<std::vector<uint8_t>> uplink;
        router.set_uplink_callback([&](const uint8_t* data, size_t size) {
            uplink.emplace_back(data, data + size);
        });

        MavlinkEndpoint endpoint;
        endpoint.kind = MavlinkEndpoint::Kind::kTcpServer;
        endpoint.host = "127.0.0.1";
        endpoint.port = server_port;
        router.SetEndpoints({endpoint});
        CHECK(router.Start());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        const int client = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(static_cast<uint16_t>(server_port));
        ::inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);
        CHECK(::connect(client, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0);

        timeval timeout{};
        timeout.tv_sec = 2;
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        const auto heartbeat = BuildV1(0, 1, 0, std::vector<uint8_t>(9, 0));
        router.FeedDownlink(heartbeat.data(), heartbeat.size());

        uint8_t buffer[512];
        const ssize_t received = ::recv(client, buffer, sizeof(buffer), 0);
        CHECK(received > 0);
        CHECK_EQ(static_cast<size_t>(received), heartbeat.size());

        // And the other direction.
        const auto command = BuildV2(0, 255, 76, std::vector<uint8_t>(33, 0));
        ::send(client, command.data(), command.size(), 0);
        for (int i = 0; i < 50 && uplink.empty(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK_EQ(uplink.size(), static_cast<size_t>(1));

        router.Stop();
        ::close(client);
    }

    // --- An endpoint with uplink disabled is receive-only -----------------
    {
        const int server_port = FreePort();

        MavlinkRouter router;
        std::vector<std::vector<uint8_t>> uplink;
        router.set_uplink_callback([&](const uint8_t* data, size_t size) {
            uplink.emplace_back(data, data + size);
        });

        MavlinkEndpoint endpoint;
        endpoint.kind = MavlinkEndpoint::Kind::kUdpServer;
        endpoint.host = "127.0.0.1";
        endpoint.port = server_port;
        endpoint.allow_uplink = false;
        router.SetEndpoints({endpoint});
        CHECK(router.Start());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        int gcs_port = 0;
        const int gcs = OpenUdpSocket(&gcs_port);
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(static_cast<uint16_t>(server_port));
        ::inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);

        const auto command = BuildV2(0, 255, 76, std::vector<uint8_t>(33, 0));
        ::sendto(gcs, command.data(), command.size(), 0, reinterpret_cast<sockaddr*>(&target),
                 sizeof(target));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        CHECK_EQ(uplink.size(), static_cast<size_t>(0));

        router.Stop();
        ::close(gcs);
    }

    return openipc::test::Summary("mavlink");
}
