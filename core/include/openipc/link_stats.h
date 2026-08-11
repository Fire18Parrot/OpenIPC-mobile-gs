// SPDX-License-Identifier: GPL-3.0-only
//
// Link quality accounting shared by the OSD, the settings UI and adaptive-link.
//
// This is the app's equivalent of what wfb-ng exposes on its JSON API (port
// 8103) and what alink_gs scrapes from it. Because the aggregator runs in our
// own process we read the counters directly instead of over a socket.

#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

namespace openipc::gs {

// Per-antenna signal record. One entry per RF path the adapter reports.
struct AntennaStat {
    int antenna_id = 0;
    int32_t count = 0;
    int8_t rssi_min = 0;
    int8_t rssi_avg = 0;
    int8_t rssi_max = 0;
    int8_t snr_min = 0;
    int8_t snr_avg = 0;
    int8_t snr_max = 0;
};

// One reporting interval's worth of link state.
struct LinkSnapshot {
    // Packet counters over the interval, named as in wfb-ng's Aggregator.
    uint32_t packets_all = 0;
    uint32_t packets_data = 0;
    uint32_t packets_session = 0;
    uint32_t packets_lost = 0;
    uint32_t packets_fec_recovered = 0;
    uint32_t packets_bad = 0;
    uint32_t packets_decrypt_err = 0;
    uint32_t bytes_all = 0;

    // Negotiated FEC parameters from the air unit's session packet.
    int fec_k = -1;
    int fec_n = -1;

    std::vector<AntennaStat> antennas;

    // Convenience aggregates used by the OSD and by adaptive-link.
    int best_rssi = -105;
    int best_snr = 0;
    int num_antennas = 0;

    // True once a session key has been accepted, i.e. we are talking to the
    // right air unit with the right gs.key.
    bool session_established = false;

    // Wall-clock milliseconds when this snapshot was taken.
    uint64_t timestamp_ms = 0;
};

// Thread-safe holder. The wfb receive thread publishes; the UI, the OSD and the
// adaptive-link thread read.
class LinkStats {
public:
    void Publish(const LinkSnapshot& snapshot);
    LinkSnapshot Get() const;

private:
    mutable std::mutex mutex_;
    LinkSnapshot current_;
};

// Wall clock in milliseconds. Matches wfb-ng's get_time_ms().
uint64_t NowMs();

}  // namespace openipc::gs
