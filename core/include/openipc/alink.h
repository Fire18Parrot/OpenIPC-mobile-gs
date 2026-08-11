// SPDX-License-Identifier: GPL-3.0-only
//
// Adaptive link: a port of OpenIPC/adaptive-link's alink_gs to C++.
//
// alink_gs is a Python daemon on the SBC ground station. It scrapes wfb-ng's
// JSON stats API, turns RSSI/SNR/loss into a single link score, and sends that
// score to the air unit over the wfb tunnel; alink_drone there adjusts bitrate,
// FEC and MCS in response.
//
// This port keeps the scoring maths and the wire format byte-for-byte
// compatible with upstream, so it drives an unmodified alink_drone. The only
// thing that changes is where the stats come from: our own aggregator rather
// than a TCP scrape of another process.

#pragma once

#include <cstdint>
#include <string>

#include "openipc/config.h"
#include "openipc/link_stats.h"

namespace openipc::gs {

// The score alink reports lives in [1000, 2000]: 1000 is "as bad as it gets",
// 2000 is a perfect link. alink_drone maps it onto its txprofiles.
inline constexpr int kAlinkScoreMin = 1000;
inline constexpr int kAlinkScoreMax = 2000;

// The message alink_drone falls back to when the ground station goes quiet.
inline constexpr const char* kAlinkFallbackMessage = "999";

struct AlinkResult {
    // The colon-separated message, exactly as alink_drone's process_message()
    // parses it:
    //   ts:score:score:recovered:lost:rssi:snr:antennas:penalty:fec_change[:idr]
    std::string message;

    // Fields kept separately so the UI can show them without re-parsing.
    int score = kAlinkScoreMin;
    double raw_score = kAlinkScoreMin;
    double penalty = 0.0;
    int fec_change = 0;
    double error_ratio = 0.0;
    double filtered_noise = 0.0;
    std::string idr_code;
};

// Scoring is a pure function of the snapshot plus the Kalman state carried
// between calls, which makes it straightforward to test against upstream's
// numbers without any radio involved.
class AlinkController {
public:
    explicit AlinkController(const AlinkConfig& config);

    // Compute the next message from a stats snapshot. The snapshot holds one
    // reporting interval's counters, not running totals - the same shape
    // alink_gs receives from wfb-ng's stats stream. `unix_time_s` is the
    // timestamp field of the message; taking it as a parameter keeps the
    // function deterministic under test.
    AlinkResult Update(const LinkSnapshot& snapshot, uint64_t unix_time_s);

    // Frame a message for the wire: a 4-byte big-endian length followed by the
    // ASCII payload, as alink_gs's send_udp() does.
    static std::string Frame(const std::string& message);

    const AlinkConfig& config() const { return config_; }
    void set_config(const AlinkConfig& config) { config_ = config; }

private:
    double KalmanUpdate(double measurement);
    double AdjustFecRecovered(double fec_recovered, int fec_k, int fec_n) const;

    AlinkConfig config_;

    // Kalman state, carried across calls exactly as the Python globals are.
    double kalman_estimate_;
    double kalman_error_estimate_;

    // An IDR (keyframe) request is repeated across several messages so it
    // survives packet loss on the uplink; the drone de-duplicates by code.
    std::string idr_code_;
    int idr_remaining_ = 0;
};

}  // namespace openipc::gs
