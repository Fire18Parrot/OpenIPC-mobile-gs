// SPDX-License-Identifier: GPL-3.0-only
//
// The wfb-ng receive path.
//
// wfb-ng's own Aggregator is compiled from the submodule and subclassed here so
// that decoded payload is delivered by callback instead of over a UDP socket.
// That keeps the crypto, the Reed-Solomon FEC and the block reassembly exactly
// as upstream wrote them - which is what makes this interoperable with any air
// unit - while removing the loopback hop an SBC ground station needs because
// its wfb_rx and its video player are separate processes.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "openipc/config.h"
#include "openipc/link_stats.h"

// Forward declaration so this header does not drag wfb-ng's rx.hpp - and with
// it pcap and libsodium headers - into every translation unit.
class Aggregator;

namespace openipc::gs {

// Payload of one reassembled packet from a stream, i.e. exactly what the air
// unit fed into wfb_tx on the other end.
using StreamCallback = std::function<void(const uint8_t* data, size_t size)>;

// Per-frame receive metadata, as reported by the driver.
struct FrameMeta {
    // Per RF path. `paths` says how many of the four slots are populated.
    int8_t rssi[4] = {0, 0, 0, 0};
    int8_t snr[4] = {0, 0, 0, 0};
    int paths = 0;
    uint16_t freq_mhz = 0;
    uint8_t mcs_index = 0;
    uint8_t bandwidth = 0;
    uint8_t wlan_idx = 0;
};

class WfbReceiver {
public:
    explicit WfbReceiver(const RadioConfig& radio);
    ~WfbReceiver();

    WfbReceiver(const WfbReceiver&) = delete;
    WfbReceiver& operator=(const WfbReceiver&) = delete;

    // Register a sink for one radio port. Throws std::runtime_error if the key
    // file cannot be read, which is the usual symptom of a missing or wrong
    // gs.key and worth surfacing to the user verbatim.
    void AddStream(uint8_t radio_port, StreamCallback callback);

    // Feed one raw 802.11 frame straight from the driver, including the
    // trailing FCS (devourer's Packet::Data contract).
    void ProcessFrame(const uint8_t* frame, size_t size, const FrameMeta& meta);

    // Take the counters accumulated since the last call and reset them, the way
    // wfb-ng's own stats interval works.
    LinkSnapshot TakeSnapshot();

    // True once a session packet has been decrypted, meaning the gs.key matches
    // the air unit's drone.key.
    bool session_established() const;

private:
    class Sink;

    // Which radio port this frame belongs to, or -1 if it is not ours.
    int ClassifyFrame(const uint8_t* frame, size_t size) const;

    RadioConfig radio_;
    mutable std::mutex mutex_;
    std::map<uint8_t, std::unique_ptr<Sink>> sinks_;

    // The port whose counters drive the OSD and adaptive-link: video, because
    // it carries by far the most packets and so gives the best signal estimate.
    uint8_t primary_port_ = kPortVideo;

    // Signal accounting, kept here rather than read out of wfb-ng's internal
    // antenna map so a snapshot is cheap and lock-scoped.
    struct PathAccumulator {
        int32_t count = 0;
        int32_t rssi_sum = 0;
        int32_t snr_sum = 0;
        int8_t rssi_min = 0;
        int8_t rssi_max = 0;
        int8_t snr_min = 0;
        int8_t snr_max = 0;
    };
    PathAccumulator paths_[4];

    void AccumulateSignal(const FrameMeta& meta);
    void ResetSignal();
};

// Key handling for gs.key, in the layout wfb-ng's Aggregator expects:
// 32 bytes of ground-station secret key followed by 32 bytes of the air unit's
// public key.
struct KeyValidation {
    bool ok = false;
    std::string error;
};

KeyValidation ValidateGsKey(const std::string& path);

// Generate a fresh pair of key files, matching wfb-ng's keygen layout:
//   gs.key    = gs secret    + drone public
//   drone.key = drone secret + gs public
// The drone.key must then be copied to the air unit.
bool GenerateKeyPair(const std::string& gs_key_path, const std::string& drone_key_path,
                     std::string* error);

}  // namespace openipc::gs
