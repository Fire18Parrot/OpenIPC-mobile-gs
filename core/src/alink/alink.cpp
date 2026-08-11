// SPDX-License-Identifier: GPL-3.0-only
//
// Port of OpenIPC/adaptive-link alink_gs (calculate_link / generate_message /
// send_udp). Line-for-line equivalent to the Python so that an unmodified
// alink_drone sees identical input.

#include "openipc/alink.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

namespace openipc::gs {
namespace {

// alink_gs uses random.choices(string.ascii_lowercase, k=4).
std::string MakeIdrCode() {
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(0, 25);
    std::string code(4, 'a');
    for (char& c : code) {
        c = static_cast<char>('a' + dist(rng));
    }
    return code;
}

double Clamp01(double v) { return std::max(0.0, std::min(1.0, v)); }

}  // namespace

AlinkController::AlinkController(const AlinkConfig& config)
    : config_(config),
      kalman_estimate_(config.kalman_estimate),
      kalman_error_estimate_(config.kalman_error_estimate) {}

double AlinkController::KalmanUpdate(double measurement) {
    const double predicted_estimate = kalman_estimate_;
    const double predicted_error = kalman_error_estimate_ + config_.process_variance;

    const double kalman_gain = predicted_error / (predicted_error + config_.measurement_variance);

    kalman_estimate_ = predicted_estimate + kalman_gain * (measurement - predicted_estimate);
    kalman_error_estimate_ = (1.0 - kalman_gain) * predicted_error;

    return kalman_estimate_;
}

double AlinkController::AdjustFecRecovered(double fec_recovered, int fec_k, int fec_n) const {
    // With high redundancy more recoveries are expected, so each one says less
    // about link health. Upstream picks 6 so that the common 8/12 FEC is
    // neutral (redundancy 4 -> weight 6/5 ... note upstream's own comment).
    if (fec_k < 0 || fec_n <= 0) {
        return fec_recovered;
    }
    const double redundancy = static_cast<double>(fec_n - fec_k);
    const double weight = 6.0 / (1.0 + redundancy);
    return fec_recovered * weight;
}

AlinkResult AlinkController::Update(const LinkSnapshot& snapshot, uint64_t unix_time_s) {
    AlinkResult result;

    const double lost_packets = static_cast<double>(snapshot.packets_lost);
    const double fec_rec_packets = static_cast<double>(snapshot.packets_fec_recovered);
    const double all_packets = static_cast<double>(snapshot.packets_all);
    const int num_antennas = snapshot.num_antennas;

    // A loss burst is what triggers a keyframe request: the decoder needs a
    // fresh IDR to resynchronise rather than showing smeared macroblocks.
    if (snapshot.packets_lost > 0 && config_.allow_idr) {
        idr_code_ = MakeIdrCode();
        idr_remaining_ = config_.idr_max_messages;
    }

    double error_ratio = 0.0;
    double filtered_noise = 0.0;
    if (all_packets > 0.0 && num_antennas > 0) {
        const double adjusted_fec_rec = AdjustFecRecovered(fec_rec_packets, snapshot.fec_k, snapshot.fec_n);
        error_ratio = (5.0 * lost_packets + adjusted_fec_rec) / (all_packets / num_antennas);
        filtered_noise = KalmanUpdate(error_ratio);
    }

    const double snr_span = config_.snr_max - config_.snr_min;
    const double rssi_span = config_.rssi_max - config_.rssi_min;
    const double snr_normalized =
        snr_span != 0.0 ? Clamp01((snapshot.best_snr - config_.snr_min) / snr_span) : 0.0;
    const double rssi_normalized =
        rssi_span != 0.0 ? Clamp01((snapshot.best_rssi - config_.rssi_min) / rssi_span) : 0.0;

    const double score_normalized =
        config_.snr_weight * snr_normalized + config_.rssi_weight * rssi_normalized;
    const double raw_score = 1000.0 + score_normalized * 1000.0;

    double deduction_ratio;
    if (filtered_noise < config_.min_noise) {
        deduction_ratio = 0.0;
    } else {
        const double noise_span = config_.max_noise - config_.min_noise;
        const double t = noise_span != 0.0 ? (filtered_noise - config_.min_noise) / noise_span : 1.0;
        deduction_ratio = std::min(std::pow(t, config_.deduction_exponent), 1.0);
    }

    const double final_score =
        config_.allow_penalty ? 1000.0 + (raw_score - 1000.0) * (1.0 - deduction_ratio) : raw_score;
    const double penalty = config_.allow_penalty ? (final_score - raw_score) : 0.0;

    int fec_change;
    if (!config_.allow_fec_increase || filtered_noise <= config_.min_noise_for_fec_change) {
        fec_change = 0;
    } else if (filtered_noise >= config_.noise_for_max_fec_change) {
        fec_change = 5;
    } else {
        const double span = config_.max_noise - config_.min_noise_for_fec_change;
        const double t = span != 0.0 ? (filtered_noise - config_.min_noise_for_fec_change) / span : 0.0;
        fec_change = static_cast<int>(std::lround(t * 5.0));
    }

    // Python's int() truncates toward zero; std::lround would round away from
    // it and shift the score by one against upstream on half values.
    const int score = static_cast<int>(final_score);

    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%llu:%d:%d:%d:%d:%d:%d:%d:%d:%d",
                  static_cast<unsigned long long>(unix_time_s), score, score,
                  static_cast<int>(fec_rec_packets), static_cast<int>(lost_packets),
                  snapshot.best_rssi, snapshot.best_snr, num_antennas,
                  static_cast<int>(penalty), fec_change);
    result.message = buffer;

    if (!idr_code_.empty() && idr_remaining_ > 0) {
        result.message += ":" + idr_code_;
        result.idr_code = idr_code_;
        if (--idr_remaining_ == 0) {
            idr_code_.clear();
        }
    }

    result.score = score;
    result.raw_score = raw_score;
    result.penalty = penalty;
    result.fec_change = fec_change;
    result.error_ratio = error_ratio;
    result.filtered_noise = filtered_noise;
    return result;
}

std::string AlinkController::Frame(const std::string& message) {
    const uint32_t size = static_cast<uint32_t>(message.size());
    std::string framed;
    framed.reserve(message.size() + 4);
    framed.push_back(static_cast<char>((size >> 24) & 0xff));
    framed.push_back(static_cast<char>((size >> 16) & 0xff));
    framed.push_back(static_cast<char>((size >> 8) & 0xff));
    framed.push_back(static_cast<char>(size & 0xff));
    framed += message;
    return framed;
}

}  // namespace openipc::gs
