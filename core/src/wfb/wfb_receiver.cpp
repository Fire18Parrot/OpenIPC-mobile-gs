// SPDX-License-Identifier: GPL-3.0-only

#include "openipc/wfb_receiver.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <stdexcept>

// wfb-ng headers. rx.hpp pulls in wifibroadcast.hpp, libsodium and pcap.
#include "rx.hpp"
#include "wifibroadcast.hpp"

namespace openipc::gs {
namespace {

// 802.11 header layout used by wfb-ng, from wifibroadcast.hpp's
// ieee80211_header[]: a data frame whose transmitter and destination addresses
// both encode "W" "B" followed by the 4-byte big-endian channel_id.
constexpr size_t kIeee80211HeaderSize = 24;
constexpr size_t kFcsSize = 4;
constexpr uint8_t kWfbMacByte0 = 0x57;  // 'W'
constexpr uint8_t kWfbMacByte1 = 0x42;  // 'B'

constexpr size_t kSrcMacOffset = 10;
constexpr size_t kDstMacOffset = 16;

}  // namespace

// A wfb-ng Aggregator whose output goes to a callback rather than a socket.
// Everything else - session key handling, ChaCha20-Poly1305, the FEC block ring
// - is upstream's implementation, untouched.
class WfbReceiver::Sink : public Aggregator {
public:
    Sink(const std::string& keypair, uint64_t epoch, uint32_t channel_id, StreamCallback callback)
        : Aggregator(keypair, epoch, channel_id), callback_(std::move(callback)) {}

    bool session_established() const { return count_p_session > 0 || session_seen_; }

protected:
    void send_to_socket(const uint8_t* payload, uint16_t packet_size) override {
        session_seen_ = true;
        if (callback_) {
            callback_(payload, packet_size);
        }
    }

private:
    StreamCallback callback_;
    bool session_seen_ = false;
};

WfbReceiver::WfbReceiver(const RadioConfig& radio) : radio_(radio) { ResetSignal(); }

WfbReceiver::~WfbReceiver() = default;

void WfbReceiver::AddStream(uint8_t radio_port, StreamCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t channel_id = ChannelId(radio_.link_id, radio_port);
    // Epoch 0 means "accept any epoch"; the air unit stamps its own and the
    // aggregator only uses it to reject stale session packets.
    sinks_[radio_port] =
        std::make_unique<Sink>(radio_.key_path, 0, channel_id, std::move(callback));
}

int WfbReceiver::ClassifyFrame(const uint8_t* frame, size_t size) const {
    if (size < kIeee80211HeaderSize + kFcsSize) {
        return -1;
    }
    // Data frame, matching wfb-ng's injected frame control bytes.
    if (frame[0] != 0x08 || frame[1] != 0x01) {
        return -1;
    }
    // Both addresses must carry the wfb marker and the same channel_id, which
    // is how a wfb frame is told apart from other traffic on the channel.
    if (frame[kSrcMacOffset] != kWfbMacByte0 || frame[kSrcMacOffset + 1] != kWfbMacByte1 ||
        frame[kDstMacOffset] != kWfbMacByte0 || frame[kDstMacOffset + 1] != kWfbMacByte1) {
        return -1;
    }
    if (std::memcmp(frame + kSrcMacOffset + 2, frame + kDstMacOffset + 2, 4) != 0) {
        return -1;
    }

    const uint32_t channel_id = (static_cast<uint32_t>(frame[kSrcMacOffset + 2]) << 24) |
                                (static_cast<uint32_t>(frame[kSrcMacOffset + 3]) << 16) |
                                (static_cast<uint32_t>(frame[kSrcMacOffset + 4]) << 8) |
                                static_cast<uint32_t>(frame[kSrcMacOffset + 5]);

    if ((channel_id >> 8) != radio_.link_id) {
        return -1;  // another vehicle on the same channel
    }
    return static_cast<int>(channel_id & 0xff);
}

void WfbReceiver::AccumulateSignal(const FrameMeta& meta) {
    const int paths = std::min(meta.paths, 4);
    for (int i = 0; i < paths; ++i) {
        PathAccumulator& acc = paths_[i];
        if (acc.count == 0) {
            acc.rssi_min = acc.rssi_max = meta.rssi[i];
            acc.snr_min = acc.snr_max = meta.snr[i];
        } else {
            acc.rssi_min = std::min(acc.rssi_min, meta.rssi[i]);
            acc.rssi_max = std::max(acc.rssi_max, meta.rssi[i]);
            acc.snr_min = std::min(acc.snr_min, meta.snr[i]);
            acc.snr_max = std::max(acc.snr_max, meta.snr[i]);
        }
        acc.rssi_sum += meta.rssi[i];
        acc.snr_sum += meta.snr[i];
        ++acc.count;
    }
}

void WfbReceiver::ResetSignal() {
    for (PathAccumulator& acc : paths_) {
        acc = PathAccumulator{};
    }
}

void WfbReceiver::ProcessFrame(const uint8_t* frame, size_t size, const FrameMeta& meta) {
    const int radio_port = ClassifyFrame(frame, size);
    if (radio_port < 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sinks_.find(static_cast<uint8_t>(radio_port));
    if (it == sinks_.end()) {
        return;  // a stream we are not subscribed to
    }

    if (static_cast<uint8_t>(radio_port) == primary_port_) {
        AccumulateSignal(meta);
    }

    // wfb-ng wants the payload after the 802.11 header, without the FCS.
    const uint8_t* payload = frame + kIeee80211HeaderSize;
    const size_t payload_size = size - kIeee80211HeaderSize - kFcsSize;

    // The aggregator reports SNR as rssi - noise, so hand it a synthetic noise
    // floor derived from the driver's real per-path SNR. Passing a constant
    // here, as some ports do, makes every SNR figure downstream meaningless.
    uint8_t antenna[RX_ANT_MAX];
    int8_t rssi[RX_ANT_MAX];
    int8_t noise[RX_ANT_MAX];
    const int paths = std::max(1, std::min(meta.paths, RX_ANT_MAX));
    for (int i = 0; i < RX_ANT_MAX; ++i) {
        if (i < paths) {
            antenna[i] = static_cast<uint8_t>(i);
            rssi[i] = meta.rssi[i];
            noise[i] = static_cast<int8_t>(meta.rssi[i] - meta.snr[i]);
        } else {
            antenna[i] = 0xff;  // unused slot, per wrxfwd_t
            rssi[i] = 0;
            noise[i] = SCHAR_MAX;
        }
    }

    it->second->process_packet(payload, payload_size, meta.wlan_idx, antenna, rssi, noise,
                               meta.freq_mhz, meta.mcs_index, meta.bandwidth, nullptr);
}

LinkSnapshot WfbReceiver::TakeSnapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    LinkSnapshot snapshot;
    snapshot.timestamp_ms = NowMs();

    auto it = sinks_.find(primary_port_);
    if (it != sinks_.end()) {
        Sink& sink = *it->second;
        snapshot.packets_all = sink.count_p_all;
        snapshot.bytes_all = sink.count_b_all;
        snapshot.packets_data = sink.count_p_data;
        snapshot.packets_session = sink.count_p_session;
        snapshot.packets_lost = sink.count_p_lost;
        snapshot.packets_fec_recovered = sink.count_p_fec_recovered;
        snapshot.packets_bad = sink.count_p_bad;
        snapshot.packets_decrypt_err = sink.count_p_dec_err;
        snapshot.session_established = sink.session_established();
        sink.clear_stats();
    }

    int best_rssi = -105;
    int best_snr = 0;
    int active = 0;
    for (const PathAccumulator& acc : paths_) {
        if (acc.count == 0) {
            continue;
        }
        AntennaStat stat;
        stat.antenna_id = active;
        stat.count = acc.count;
        stat.rssi_min = acc.rssi_min;
        stat.rssi_max = acc.rssi_max;
        stat.rssi_avg = static_cast<int8_t>(acc.rssi_sum / acc.count);
        stat.snr_min = acc.snr_min;
        stat.snr_max = acc.snr_max;
        stat.snr_avg = static_cast<int8_t>(acc.snr_sum / acc.count);
        snapshot.antennas.push_back(stat);

        best_rssi = std::max(best_rssi, static_cast<int>(stat.rssi_avg));
        best_snr = std::max(best_snr, static_cast<int>(stat.snr_avg));
        ++active;
    }
    snapshot.best_rssi = best_rssi;
    snapshot.best_snr = best_snr;
    snapshot.num_antennas = active;
    ResetSignal();

    return snapshot;
}

bool WfbReceiver::session_established() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sinks_.find(primary_port_);
    return it != sinks_.end() && it->second->session_established();
}

}  // namespace openipc::gs
