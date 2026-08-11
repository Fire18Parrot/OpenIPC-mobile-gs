// SPDX-License-Identifier: GPL-3.0-only
//
// Ground-station configuration types.
//
// The values here mirror the OpenIPC SBC ground station's wifibroadcast.cfg and
// wfb-ng's master.cfg so that a phone running this app is interchangeable with
// an SBC ground station on the same air unit.

#pragma once

#include <cstdint>
#include <string>

namespace openipc::gs {

// Radio port assignment, from wfb-ng master.cfg.
//
// Down streams (vehicle -> GS) use 0..127, up streams (GS -> vehicle) 128..255.
// Within the downlink range: 0-15 video, 16-31 mavlink, 32-47 tunnel.
enum RadioPort : uint8_t {
    kPortVideo = 0x00,      // default video stream
    kPortMavlinkRx = 0x10,  // vehicle -> GS telemetry
    kPortTunnelRx = 0x20,   // vehicle -> GS tunnel
    kPortMavlinkTx = 0x90,  // GS -> vehicle telemetry (uplink)
    kPortTunnelTx = 0xa0,   // GS -> vehicle tunnel (uplink; carries adaptive-link)
};

enum class Bandwidth : uint8_t {
    k5 = 5,
    k10 = 10,
    k20 = 20,
    k40 = 40,
    k80 = 80,
};

// A wfb-ng channel_id is (link_id << 8) | radio_port. Both ends must agree; the
// air unit's link_id comes from its wfb.yaml.
constexpr uint32_t ChannelId(uint32_t link_id, uint8_t radio_port) {
    return (link_id << 8) | radio_port;
}

// How the ground station gets its packets.
enum class SourceKind {
    // RTL8812AU/EU/CU adapter driven in userspace by devourer over libusb.
    // The full wfb-ng receive path runs in this process.
    kDevourer,
    // APFPV: the air unit is a plain Wi-Fi access point streaming RTP over UDP.
    // No adapter and no wfb-ng layer - we join its network and read the socket.
    kUdp,
    // Offline replay of a radiotap capture, for testing.
    kPcapFile,
};

struct RadioConfig {
    int channel = 161;  // 5825 MHz, the OpenIPC default
    Bandwidth bandwidth = Bandwidth::k20;
    uint32_t link_id = 7669206;
    // Path to gs.key: 32 bytes rx secret key followed by 32 bytes tx public key,
    // exactly the file wfb-ng's Aggregator expects.
    std::string key_path;
    bool ldpc = true;
    bool stbc = true;
};

struct UdpSourceConfig {
    std::string bind_addr = "0.0.0.0";
    int video_port = 5600;  // matches the SBC ground station's gs_video peer
};

// One MAVLink endpoint an external ground control station can attach to.
struct MavlinkEndpoint {
    enum class Kind {
        kUdpOut,     // we send to host:port, and accept replies from it
        kUdpServer,  // we bind port and learn the peer from its first packet
        kTcpServer,  // we listen on port for GCS clients
    };

    Kind kind = Kind::kUdpOut;
    std::string host = "127.0.0.1";
    int port = 14550;
    bool enabled = true;
    // When set, MAVLink arriving from this endpoint is forwarded up to the
    // aircraft over the wfb uplink, so an external GCS can command it.
    bool allow_uplink = true;
};

// Ported from adaptive-link's alink_gs.conf. Defaults match upstream so a link
// tuned against an SBC ground station behaves the same here.
struct AlinkConfig {
    bool enabled = false;

    // Where alink messages go. On a wfb-ng link this is the air unit's address
    // inside the wfb tunnel.
    std::string udp_host = "10.5.0.10";
    int udp_port = 9999;
    int interval_ms = 100;

    double snr_weight = 0.5;
    double rssi_weight = 0.5;

    double snr_min = 12.0;
    double snr_max = 38.0;
    double rssi_min = -80.0;
    double rssi_max = -30.0;

    bool allow_idr = true;
    int idr_max_messages = 20;

    bool allow_penalty = false;
    bool allow_fec_increase = false;

    double min_noise = 0.01;
    double max_noise = 0.1;
    double deduction_exponent = 0.5;
    double min_noise_for_fec_change = 0.01;
    double noise_for_max_fec_change = 0.1;

    double kalman_estimate = 0.005;
    double kalman_error_estimate = 0.1;
    double process_variance = 1e-5;
    double measurement_variance = 0.01;
};

struct GsConfig {
    SourceKind source = SourceKind::kDevourer;
    RadioConfig radio;
    UdpSourceConfig udp;
    AlinkConfig alink;

    // Replay path when source == kPcapFile.
    std::string pcap_path;

    // Video sink: when non-empty, decoded video payload is also mirrored to this
    // UDP address, matching the SBC ground station's gs_video peer of
    // 127.0.0.1:5600 so existing tools keep working.
    std::string video_mirror_host;
    int video_mirror_port = 0;
};

}  // namespace openipc::gs
