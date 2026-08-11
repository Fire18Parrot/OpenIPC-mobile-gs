// SPDX-License-Identifier: GPL-3.0-only
//
// RTP depacketiser tests: the shapes an OpenIPC air unit actually emits, plus
// the loss cases that decide whether the decoder gets a clean bitstream or
// visible corruption.

#include <cstdint>
#include <vector>

#include "openipc/rtp.h"
#include "test_util.h"

namespace {

using openipc::gs::NalUnit;
using openipc::gs::RtpDepacketizer;
using openipc::gs::VideoCodec;

std::vector<uint8_t> MakeRtp(uint16_t sequence, uint32_t timestamp, bool marker,
                             const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> packet = {
        0x80,
        static_cast<uint8_t>(marker ? 0x80 | 96 : 96),
        static_cast<uint8_t>(sequence >> 8),
        static_cast<uint8_t>(sequence & 0xff),
        static_cast<uint8_t>(timestamp >> 24),
        static_cast<uint8_t>(timestamp >> 16),
        static_cast<uint8_t>(timestamp >> 8),
        static_cast<uint8_t>(timestamp),
        0xde, 0xad, 0xbe, 0xef,  // SSRC
    };
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

struct Collector {
    std::vector<std::vector<uint8_t>> nals;

    void operator()(const NalUnit& nal) { nals.emplace_back(nal.data, nal.data + nal.size); }
};

bool HasStartCode(const std::vector<uint8_t>& nal) {
    return nal.size() >= 4 && nal[0] == 0 && nal[1] == 0 && nal[2] == 0 && nal[3] == 1;
}

}  // namespace

int main() {
    // --- H.264 single NAL unit -------------------------------------------
    {
        Collector collector;
        RtpDepacketizer depacketizer(VideoCodec::kH264);
        depacketizer.set_callback([&](const NalUnit& nal) { collector(nal); });

        // nal_type 5 = IDR slice.
        const std::vector<uint8_t> payload = {0x65, 0x11, 0x22, 0x33};
        const std::vector<uint8_t> packet = MakeRtp(1, 9000, true, payload);
        CHECK(depacketizer.Push(packet.data(), packet.size()));

        CHECK_EQ(collector.nals.size(), static_cast<size_t>(1));
        CHECK(HasStartCode(collector.nals[0]));
        CHECK_EQ(collector.nals[0].size(), payload.size() + 4);
        CHECK_EQ(static_cast<int>(collector.nals[0][4]), 0x65);
    }

    // --- H.264 FU-A reassembly -------------------------------------------
    {
        Collector collector;
        RtpDepacketizer depacketizer(VideoCodec::kH264);
        depacketizer.set_callback([&](const NalUnit& nal) { collector(nal); });

        // FU indicator 0x7c (F/NRI from 0x65), FU header start bit + type 5.
        const std::vector<uint8_t> first = {0x7c, 0x85, 0xaa, 0xbb};
        const std::vector<uint8_t> middle = {0x7c, 0x05, 0xcc};
        const std::vector<uint8_t> last = {0x7c, 0x45, 0xdd};

        auto p1 = MakeRtp(10, 9000, false, first);
        auto p2 = MakeRtp(11, 9000, false, middle);
        auto p3 = MakeRtp(12, 9000, true, last);
        depacketizer.Push(p1.data(), p1.size());
        CHECK_EQ(collector.nals.size(), static_cast<size_t>(0));
        depacketizer.Push(p2.data(), p2.size());
        depacketizer.Push(p3.data(), p3.size());

        CHECK_EQ(collector.nals.size(), static_cast<size_t>(1));
        const std::vector<uint8_t> expected = {0, 0, 0, 1, 0x65, 0xaa, 0xbb, 0xcc, 0xdd};
        CHECK(collector.nals[0] == expected);
    }

    // --- A gap mid-fragment discards the NAL rather than splicing a hole --
    {
        Collector collector;
        RtpDepacketizer depacketizer(VideoCodec::kH264);
        depacketizer.set_callback([&](const NalUnit& nal) { collector(nal); });

        auto p1 = MakeRtp(20, 9000, false, {0x7c, 0x85, 0xaa});
        auto p3 = MakeRtp(22, 9000, true, {0x7c, 0x45, 0xdd});  // sequence 21 lost
        depacketizer.Push(p1.data(), p1.size());
        depacketizer.Push(p3.data(), p3.size());

        CHECK_EQ(collector.nals.size(), static_cast<size_t>(0));
        CHECK(depacketizer.broken_fragments() > 0);
    }

    // --- H.264 STAP-A splits into separate NALs ---------------------------
    {
        Collector collector;
        RtpDepacketizer depacketizer(VideoCodec::kH264);
        depacketizer.set_callback([&](const NalUnit& nal) { collector(nal); });

        // Type 24, then [size][nal] pairs: SPS (0x67) and PPS (0x68).
        const std::vector<uint8_t> payload = {0x18, 0x00, 0x02, 0x67, 0x42,
                                              0x00, 0x02, 0x68, 0xce};
        auto packet = MakeRtp(30, 9000, true, payload);
        depacketizer.Push(packet.data(), packet.size());

        CHECK_EQ(collector.nals.size(), static_cast<size_t>(2));
        if (collector.nals.size() == 2) {
            CHECK_EQ(static_cast<int>(collector.nals[0][4]), 0x67);
            CHECK_EQ(static_cast<int>(collector.nals[1][4]), 0x68);
        }
    }

    // --- H.265 single NAL and FU reassembly -------------------------------
    {
        Collector collector;
        RtpDepacketizer depacketizer(VideoCodec::kH265);
        depacketizer.set_callback([&](const NalUnit& nal) { collector(nal); });

        // Type 19 (IDR_W_RADL) -> first byte (19 << 1) = 0x26, tid 1.
        const std::vector<uint8_t> single = {0x26, 0x01, 0x11, 0x22};
        auto packet = MakeRtp(40, 9000, true, single);
        depacketizer.Push(packet.data(), packet.size());
        CHECK_EQ(collector.nals.size(), static_cast<size_t>(1));
        CHECK_EQ(static_cast<int>(collector.nals[0][4]), 0x26);

        collector.nals.clear();
        // FU: payload header type 49 -> (49 << 1) = 0x62.
        auto f1 = MakeRtp(41, 9000, false, {0x62, 0x01, 0x93, 0xaa});  // start, type 19
        auto f2 = MakeRtp(42, 9000, true, {0x62, 0x01, 0x53, 0xbb});   // end, type 19
        depacketizer.Push(f1.data(), f1.size());
        depacketizer.Push(f2.data(), f2.size());

        CHECK_EQ(collector.nals.size(), static_cast<size_t>(1));
        const std::vector<uint8_t> expected = {0, 0, 0, 1, 0x26, 0x01, 0xaa, 0xbb};
        CHECK(collector.nals[0] == expected);
    }

    // --- H.265 aggregation packet ----------------------------------------
    {
        Collector collector;
        RtpDepacketizer depacketizer(VideoCodec::kH265);
        depacketizer.set_callback([&](const NalUnit& nal) { collector(nal); });

        // Type 48 -> 0x60, then [size][nal] pairs.
        const std::vector<uint8_t> payload = {0x60, 0x01, 0x00, 0x03, 0x40, 0x01, 0xaa,
                                              0x00, 0x03, 0x42, 0x01, 0xbb};
        auto packet = MakeRtp(50, 9000, true, payload);
        depacketizer.Push(packet.data(), packet.size());
        CHECK_EQ(collector.nals.size(), static_cast<size_t>(2));
    }

    // --- Codec autodetection ---------------------------------------------
    {
        const std::vector<uint8_t> h264 = MakeRtp(1, 0, true, {0x65, 0x11, 0x22});
        CHECK(openipc::gs::DetectCodec(h264.data(), h264.size()) == VideoCodec::kH264);

        const std::vector<uint8_t> h265 = MakeRtp(1, 0, true, {0x26, 0x01, 0x11});
        CHECK(openipc::gs::DetectCodec(h265.data(), h265.size()) == VideoCodec::kH265);
    }

    // --- Malformed input is rejected without crashing ---------------------
    {
        RtpDepacketizer depacketizer(VideoCodec::kH264);
        const std::vector<uint8_t> too_short = {0x80, 0x60};
        CHECK(!depacketizer.Push(too_short.data(), too_short.size()));

        // RTP version 1 is not valid.
        std::vector<uint8_t> bad_version = MakeRtp(1, 0, true, {0x65, 0x01});
        bad_version[0] = 0x40;
        CHECK(!depacketizer.Push(bad_version.data(), bad_version.size()));

        // A header-only packet has no payload to depacketise.
        const std::vector<uint8_t> empty = MakeRtp(1, 0, true, {});
        CHECK(!depacketizer.Push(empty.data(), empty.size()));
    }

    // --- CSRC and extension headers are skipped ---------------------------
    {
        Collector collector;
        RtpDepacketizer depacketizer(VideoCodec::kH264);
        depacketizer.set_callback([&](const NalUnit& nal) { collector(nal); });

        std::vector<uint8_t> packet = {
            0x91,                    // version 2, extension bit, 1 CSRC
            96, 0x00, 0x01,          // payload type, sequence
            0x00, 0x00, 0x00, 0x01,  // timestamp
            0xde, 0xad, 0xbe, 0xef,  // SSRC
            0x11, 0x22, 0x33, 0x44,  // CSRC[0]
            0xbe, 0xde, 0x00, 0x01,  // extension header, 1 word
            0x00, 0x00, 0x00, 0x00,  // extension word
            0x65, 0xaa,              // payload
        };
        CHECK(depacketizer.Push(packet.data(), packet.size()));
        CHECK_EQ(collector.nals.size(), static_cast<size_t>(1));
        if (!collector.nals.empty()) {
            CHECK_EQ(collector.nals[0].size(), static_cast<size_t>(6));
            CHECK_EQ(static_cast<int>(collector.nals[0][4]), 0x65);
        }
    }

    return openipc::test::Summary("rtp");
}
