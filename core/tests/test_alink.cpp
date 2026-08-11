// SPDX-License-Identifier: GPL-3.0-only
//
// Checks the adaptive-link port against values computed by hand from
// alink_gs's own arithmetic, and pins the wire format alink_drone parses.

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "openipc/alink.h"
#include "test_util.h"

namespace {

using openipc::gs::AlinkConfig;
using openipc::gs::AlinkController;
using openipc::gs::AlinkResult;
using openipc::gs::LinkSnapshot;

std::vector<std::string> Split(const std::string& value, char delimiter) {
    std::vector<std::string> parts;
    std::stringstream stream(value);
    std::string item;
    while (std::getline(stream, item, delimiter)) {
        parts.push_back(item);
    }
    return parts;
}

LinkSnapshot PerfectLink() {
    LinkSnapshot snapshot;
    snapshot.best_rssi = -30;  // RSSI_MAX
    snapshot.best_snr = 38;    // SNR_MAX
    snapshot.num_antennas = 1;
    snapshot.packets_all = 1000;
    snapshot.fec_k = 8;
    snapshot.fec_n = 12;
    return snapshot;
}

}  // namespace

int main() {
    // --- A perfect link scores the maximum -------------------------------
    {
        AlinkConfig config;
        AlinkController controller(config);
        const AlinkResult result = controller.Update(PerfectLink(), 1700000000);
        CHECK_EQ(result.score, openipc::gs::kAlinkScoreMax);

        const std::vector<std::string> fields = Split(result.message, ':');
        // ts:score:score:recovered:lost:rssi:snr:antennas:penalty:fec_change
        CHECK_EQ(fields.size(), static_cast<size_t>(10));
        CHECK_STREQ(fields[0], "1700000000");
        CHECK_STREQ(fields[1], "2000");
        CHECK_STREQ(fields[2], "2000");
        CHECK_STREQ(fields[3], "0");
        CHECK_STREQ(fields[4], "0");
        CHECK_STREQ(fields[5], "-30");
        CHECK_STREQ(fields[6], "38");
        CHECK_STREQ(fields[7], "1");
        CHECK_STREQ(fields[8], "0");
        CHECK_STREQ(fields[9], "0");
    }

    // --- A dead link floors at 1000 --------------------------------------
    {
        AlinkConfig config;
        AlinkController controller(config);
        LinkSnapshot snapshot;
        snapshot.best_rssi = -95;  // below RSSI_MIN, so clamps to 0
        snapshot.best_snr = 2;     // below SNR_MIN
        snapshot.num_antennas = 1;
        snapshot.packets_all = 100;
        const AlinkResult result = controller.Update(snapshot, 1);
        CHECK_EQ(result.score, openipc::gs::kAlinkScoreMin);
    }

    // --- The midpoint is 1500 --------------------------------------------
    //
    // snr 25 of [12,38] is 0.5, rssi -55 of [-80,-30] is 0.5, each weighted
    // 0.5, so score_normalized is 0.5 and raw_score is 1500.
    {
        AlinkConfig config;
        AlinkController controller(config);
        LinkSnapshot snapshot;
        snapshot.best_rssi = -55;
        snapshot.best_snr = 25;
        snapshot.num_antennas = 1;
        snapshot.packets_all = 100;
        const AlinkResult result = controller.Update(snapshot, 1);
        CHECK_EQ(result.score, 1500);
    }

    // --- Loss raises a keyframe request that repeats then stops -----------
    {
        AlinkConfig config;
        config.allow_idr = true;
        config.idr_max_messages = 3;
        AlinkController controller(config);

        LinkSnapshot lossy = PerfectLink();
        lossy.packets_lost = 4;

        const AlinkResult first = controller.Update(lossy, 10);
        CHECK(!first.idr_code.empty());
        CHECK_EQ(Split(first.message, ':').size(), static_cast<size_t>(11));
        const std::string code = first.idr_code;

        // The same code repeats so it survives uplink loss.
        LinkSnapshot clean = PerfectLink();
        const AlinkResult second = controller.Update(clean, 11);
        CHECK_STREQ(second.idr_code, code);
        const AlinkResult third = controller.Update(clean, 12);
        CHECK_STREQ(third.idr_code, code);

        // After idr_max_messages the request is dropped from the message.
        const AlinkResult fourth = controller.Update(clean, 13);
        CHECK(fourth.idr_code.empty());
        CHECK_EQ(Split(fourth.message, ':').size(), static_cast<size_t>(10));
    }

    // --- Keyframe requests can be disabled -------------------------------
    {
        AlinkConfig config;
        config.allow_idr = false;
        AlinkController controller(config);
        LinkSnapshot lossy = PerfectLink();
        lossy.packets_lost = 10;
        const AlinkResult result = controller.Update(lossy, 1);
        CHECK(result.idr_code.empty());
        CHECK_EQ(Split(result.message, ':').size(), static_cast<size_t>(10));
    }

    // --- The noise penalty only applies when enabled ----------------------
    {
        LinkSnapshot noisy = PerfectLink();
        noisy.packets_lost = 50;
        noisy.packets_fec_recovered = 50;

        AlinkConfig without_penalty;
        without_penalty.allow_penalty = false;
        AlinkController a(without_penalty);
        const AlinkResult unpenalised = a.Update(noisy, 1);
        CHECK_EQ(unpenalised.score, openipc::gs::kAlinkScoreMax);
        CHECK_NEAR(unpenalised.penalty, 0.0, 1e-9);

        AlinkConfig with_penalty;
        with_penalty.allow_penalty = true;
        AlinkController b(with_penalty);
        // Feed the same measurement repeatedly so the Kalman estimate rises to
        // meet it, rather than asserting on a single filtered sample.
        AlinkResult penalised;
        for (int i = 0; i < 200; ++i) {
            penalised = b.Update(noisy, 1);
        }
        CHECK(penalised.score < openipc::gs::kAlinkScoreMax);
        CHECK(penalised.penalty < 0.0);
    }

    // --- FEC escalation is bounded to 0..5 -------------------------------
    {
        AlinkConfig config;
        config.allow_fec_increase = true;
        AlinkController controller(config);
        LinkSnapshot bad = PerfectLink();
        bad.packets_lost = 500;
        AlinkResult result;
        for (int i = 0; i < 200; ++i) {
            result = controller.Update(bad, 1);
        }
        CHECK(result.fec_change >= 0);
        CHECK(result.fec_change <= 5);
        CHECK_EQ(result.fec_change, 5);
    }

    // --- Framing: 4-byte big-endian length, then the ASCII payload --------
    {
        const std::string framed = AlinkController::Frame("hello");
        CHECK_EQ(framed.size(), static_cast<size_t>(9));
        CHECK_EQ(static_cast<int>(static_cast<uint8_t>(framed[0])), 0);
        CHECK_EQ(static_cast<int>(static_cast<uint8_t>(framed[1])), 0);
        CHECK_EQ(static_cast<int>(static_cast<uint8_t>(framed[2])), 0);
        CHECK_EQ(static_cast<int>(static_cast<uint8_t>(framed[3])), 5);
        CHECK_STREQ(framed.substr(4), "hello");
    }

    // --- No traffic at all must not divide by zero ------------------------
    {
        AlinkConfig config;
        AlinkController controller(config);
        LinkSnapshot empty;
        empty.num_antennas = 0;
        empty.packets_all = 0;
        const AlinkResult result = controller.Update(empty, 1);
        CHECK_NEAR(result.error_ratio, 0.0, 1e-9);
        CHECK(result.score >= openipc::gs::kAlinkScoreMin);
    }

    return openipc::test::Summary("alink");
}
