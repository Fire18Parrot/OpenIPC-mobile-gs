// SPDX-License-Identifier: GPL-3.0-only
//
// End-to-end interoperability test for the wfb-ng receive path.
//
// This does not test our receiver against our own idea of the protocol - it
// drives it with wfb-ng's *own* Transmitter, compiled from the submodule. A
// packet therefore goes through upstream's session-key exchange, its
// ChaCha20-Poly1305 encryption and its Reed-Solomon encoder before our
// aggregator subclass sees it. If upstream changes the wire format, this test
// fails and the app is known to be incompatible before it reaches a field.

#include "openipc/config.h"
#include "openipc/wfb_receiver.h"
#include "test_util.h"

// wfb-ng's tx.hpp is not self-contained: it is written to be included partway
// down tx.cpp, after that file's system headers and its `using namespace std`.
// Reproducing that prelude here is what lets us reuse upstream's transmitter
// without patching the submodule.
#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/random.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sodium.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include "zfex.h"
}

using namespace std;

#include "tx.hpp"
#include "wifibroadcast.hpp"

namespace {

using openipc::gs::ChannelId;
using openipc::gs::FrameMeta;
using openipc::gs::RadioConfig;
using openipc::gs::WfbReceiver;

// Captures what wfb_tx would have injected, and wraps it in the 802.11 header
// a real adapter would have carried, so the receiver sees a full frame.
class CapturingTransmitter : public Transmitter {
public:
    CapturingTransmitter(int k, int n, const std::string& keypair, uint64_t epoch,
                         uint32_t channel_id, std::vector<tags_item_t>& tags)
        : Transmitter(k, n, keypair, epoch, channel_id, 0, tags), channel_id_(channel_id) {}

    std::vector<std::vector<uint8_t>> frames;

    void select_output(int) override {}
    void dump_stats(uint64_t, uint32_t&, uint32_t&, uint32_t&) override {}
    void update_radiotap_header(radiotap_header_t&) override {}
    radiotap_header_t get_radiotap_header(void) override { return radiotap_header_t{}; }

protected:
    void set_mark(uint32_t) override {}

    void inject_packet(const uint8_t* buf, size_t size) override {
        std::vector<uint8_t> frame(sizeof(ieee80211_header));
        std::memcpy(frame.data(), ieee80211_header, sizeof(ieee80211_header));

        // Both the transmitter and destination address carry the channel_id in
        // their last four bytes, big endian.
        const uint32_t be = htobe32(channel_id_);
        std::memcpy(frame.data() + SRC_MAC_THIRD_BYTE, &be, sizeof(be));
        std::memcpy(frame.data() + DST_MAC_THIRD_BYTE, &be, sizeof(be));

        frame.insert(frame.end(), buf, buf + size);
        // A real adapter delivers the frame with its FCS still attached.
        frame.insert(frame.end(), {0xde, 0xad, 0xbe, 0xef});
        frames.push_back(std::move(frame));
    }

private:
    uint32_t channel_id_;
};

std::string TempPath(const char* name) {
    return std::string("/tmp/openipc_gs_test_") + std::to_string(getpid()) + "_" + name;
}

FrameMeta MakeMeta() {
    FrameMeta meta;
    meta.paths = 2;
    meta.rssi[0] = -50;
    meta.rssi[1] = -60;
    meta.snr[0] = 30;
    meta.snr[1] = 22;
    meta.freq_mhz = 5825;
    meta.mcs_index = 1;
    meta.bandwidth = 20;
    return meta;
}

// Push a set of frames through the receiver, optionally dropping some.
void Deliver(WfbReceiver& receiver, const std::vector<std::vector<uint8_t>>& frames,
             const std::vector<size_t>& drop_indices = {}) {
    const FrameMeta meta = MakeMeta();
    for (size_t i = 0; i < frames.size(); ++i) {
        bool dropped = false;
        for (size_t drop : drop_indices) {
            if (drop == i) {
                dropped = true;
                break;
            }
        }
        if (dropped) {
            continue;
        }
        receiver.ProcessFrame(frames[i].data(), frames[i].size(), meta);
    }
}

}  // namespace

int main() {
    CHECK(sodium_init() >= 0);

    const std::string gs_key = TempPath("gs.key");
    const std::string drone_key = TempPath("drone.key");
    std::string error;
    CHECK(openipc::gs::GenerateKeyPair(gs_key, drone_key, &error));
    CHECK_STREQ(error, "");

    const openipc::gs::KeyValidation validation = openipc::gs::ValidateGsKey(gs_key);
    CHECK(validation.ok);

    constexpr uint32_t kLinkId = 7669206;
    constexpr int kFecK = 8;
    constexpr int kFecN = 12;
    const uint32_t video_channel = ChannelId(kLinkId, openipc::gs::kPortVideo);

    RadioConfig radio;
    radio.link_id = kLinkId;
    radio.key_path = gs_key;

    // --- 1. A clean block round-trips ------------------------------------
    {
        std::vector<std::vector<uint8_t>> received;
        WfbReceiver receiver(radio);
        receiver.AddStream(openipc::gs::kPortVideo, [&](const uint8_t* data, size_t size) {
            received.emplace_back(data, data + size);
        });

        std::vector<tags_item_t> tags;
        CapturingTransmitter transmitter(kFecK, kFecN, drone_key, 0, video_channel, tags);
        // wfb_tx announces the session key on a timer; without it the receiver
        // has nothing to decrypt with. frames[0] is therefore the session packet.
        transmitter.send_session_key();

        std::vector<std::vector<uint8_t>> sent;
        for (int i = 0; i < kFecK; ++i) {
            std::vector<uint8_t> payload(200 + i);
            for (size_t j = 0; j < payload.size(); ++j) {
                payload[j] = static_cast<uint8_t>((i * 31 + j) & 0xff);
            }
            sent.push_back(payload);
            CHECK(transmitter.send_packet(payload.data(), payload.size(), 0));
        }

        Deliver(receiver, transmitter.frames);

        CHECK_EQ(received.size(), sent.size());
        for (size_t i = 0; i < received.size() && i < sent.size(); ++i) {
            CHECK_EQ(received[i].size(), sent[i].size());
            CHECK(received[i] == sent[i]);
        }
        CHECK(receiver.session_established());
    }

    // --- 2. FEC recovers a block with lost fragments ----------------------
    //
    // With k=8/n=12 the code tolerates any four missing fragments. Dropping
    // exactly four data fragments proves the Reed-Solomon decoder is wired up
    // and that our reassembly hands the recovered payload on intact.
    {
        std::vector<std::vector<uint8_t>> received;
        WfbReceiver receiver(radio);
        receiver.AddStream(openipc::gs::kPortVideo, [&](const uint8_t* data, size_t size) {
            received.emplace_back(data, data + size);
        });

        std::vector<tags_item_t> tags;
        CapturingTransmitter transmitter(kFecK, kFecN, drone_key, 0, video_channel, tags);
        // wfb_tx announces the session key on a timer; without it the receiver
        // has nothing to decrypt with. frames[0] is therefore the session packet.
        transmitter.send_session_key();

        std::vector<std::vector<uint8_t>> sent;
        for (int i = 0; i < kFecK; ++i) {
            std::vector<uint8_t> payload(256, static_cast<uint8_t>(i + 1));
            sent.push_back(payload);
            CHECK(transmitter.send_packet(payload.data(), payload.size(), 0));
        }

        // frames[0] is the session packet; the data fragments follow. Drop four
        // of them, which the 4 parity fragments should cover exactly.
        CHECK(transmitter.frames.size() >= static_cast<size_t>(kFecN) + 1);
        Deliver(receiver, transmitter.frames, {2, 3, 4, 5});

        CHECK_EQ(received.size(), sent.size());
        for (size_t i = 0; i < received.size() && i < sent.size(); ++i) {
            CHECK(received[i] == sent[i]);
        }

        const openipc::gs::LinkSnapshot snapshot = receiver.TakeSnapshot();
        CHECK(snapshot.packets_fec_recovered > 0);
        CHECK_EQ(snapshot.num_antennas, 2);
        // rssi -50 / -60 with snr 30 / 22: the better path wins both.
        CHECK_EQ(snapshot.best_rssi, -50);
        CHECK_EQ(snapshot.best_snr, 30);
    }

    // --- 3. A foreign link_id is ignored ----------------------------------
    {
        std::vector<std::vector<uint8_t>> received;
        WfbReceiver receiver(radio);
        receiver.AddStream(openipc::gs::kPortVideo, [&](const uint8_t* data, size_t size) {
            received.emplace_back(data, data + size);
        });

        std::vector<tags_item_t> tags;
        // A different vehicle transmitting on the same RF channel.
        CapturingTransmitter transmitter(kFecK, kFecN, drone_key, 0,
                                         ChannelId(kLinkId + 1, openipc::gs::kPortVideo), tags);
        transmitter.send_session_key();
        std::vector<uint8_t> payload(64, 0xab);
        for (int i = 0; i < kFecK; ++i) {
            CHECK(transmitter.send_packet(payload.data(), payload.size(), 0));
        }
        Deliver(receiver, transmitter.frames);
        CHECK_EQ(received.size(), static_cast<size_t>(0));
    }

    // --- 4. Streams are demultiplexed by radio port -----------------------
    {
        std::vector<std::vector<uint8_t>> video;
        std::vector<std::vector<uint8_t>> telemetry;
        WfbReceiver receiver(radio);
        receiver.AddStream(openipc::gs::kPortVideo, [&](const uint8_t* data, size_t size) {
            video.emplace_back(data, data + size);
        });
        receiver.AddStream(openipc::gs::kPortMavlinkRx, [&](const uint8_t* data, size_t size) {
            telemetry.emplace_back(data, data + size);
        });

        std::vector<tags_item_t> tags;
        CapturingTransmitter video_tx(kFecK, kFecN, drone_key, 0, video_channel, tags);
        CapturingTransmitter mavlink_tx(kFecK, kFecN, drone_key, 0,
                                        ChannelId(kLinkId, openipc::gs::kPortMavlinkRx), tags);
        video_tx.send_session_key();
        mavlink_tx.send_session_key();

        std::vector<uint8_t> video_payload(100, 0x11);
        std::vector<uint8_t> mavlink_payload(40, 0x22);
        for (int i = 0; i < kFecK; ++i) {
            CHECK(video_tx.send_packet(video_payload.data(), video_payload.size(), 0));
            CHECK(mavlink_tx.send_packet(mavlink_payload.data(), mavlink_payload.size(), 0));
        }

        Deliver(receiver, video_tx.frames);
        Deliver(receiver, mavlink_tx.frames);

        CHECK_EQ(video.size(), static_cast<size_t>(kFecK));
        CHECK_EQ(telemetry.size(), static_cast<size_t>(kFecK));
        if (!video.empty()) {
            CHECK(video[0] == video_payload);
        }
        if (!telemetry.empty()) {
            CHECK(telemetry[0] == mavlink_payload);
        }
    }

    // --- 5. The wrong gs.key yields no payload ----------------------------
    //
    // The single most common field failure: a ground station whose key does not
    // match the air unit. It must fail closed, not pass plaintext through.
    {
        const std::string other_gs = TempPath("other_gs.key");
        const std::string other_drone = TempPath("other_drone.key");
        CHECK(openipc::gs::GenerateKeyPair(other_gs, other_drone, &error));

        RadioConfig mismatched = radio;
        mismatched.key_path = other_gs;

        std::vector<std::vector<uint8_t>> received;
        WfbReceiver receiver(mismatched);
        receiver.AddStream(openipc::gs::kPortVideo, [&](const uint8_t* data, size_t size) {
            received.emplace_back(data, data + size);
        });

        std::vector<tags_item_t> tags;
        CapturingTransmitter transmitter(kFecK, kFecN, drone_key, 0, video_channel, tags);
        // wfb_tx announces the session key on a timer; without it the receiver
        // has nothing to decrypt with. frames[0] is therefore the session packet.
        transmitter.send_session_key();
        std::vector<uint8_t> payload(64, 0x5a);
        for (int i = 0; i < kFecK; ++i) {
            CHECK(transmitter.send_packet(payload.data(), payload.size(), 0));
        }
        Deliver(receiver, transmitter.frames);

        CHECK_EQ(received.size(), static_cast<size_t>(0));
        ::unlink(other_gs.c_str());
        ::unlink(other_drone.c_str());
    }

    ::unlink(gs_key.c_str());
    ::unlink(drone_key.c_str());
    return openipc::test::Summary("wfb_interop");
}
