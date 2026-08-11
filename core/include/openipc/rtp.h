// SPDX-License-Identifier: GPL-3.0-only
//
// RTP depacketiser for the video stream.
//
// OpenIPC air units send H.264 (RFC 6184) or H.265 (RFC 7798) over RTP. On an
// SBC ground station that RTP lands on UDP 5600 and is fed to a Rockchip MPP
// decoder; here it comes straight out of the wfb aggregator (or off a UDP
// socket in APFPV mode) and is turned into Annex-B NAL units for MediaCodec.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace openipc::gs {

enum class VideoCodec {
    kUnknown,
    kH264,
    kH265,
};

// Emitted once per complete access unit boundary is not attempted here: we emit
// per NAL unit, prefixed with the 4-byte Annex-B start code, which is what
// MediaCodec wants and what keeps latency at one packet.
struct NalUnit {
    const uint8_t* data = nullptr;  // points into the depacketiser's buffer
    size_t size = 0;
    uint32_t rtp_timestamp = 0;
    bool marker = false;  // RTP marker bit: last packet of an access unit
};

using NalCallback = std::function<void(const NalUnit&)>;

class RtpDepacketizer {
public:
    explicit RtpDepacketizer(VideoCodec codec = VideoCodec::kUnknown);

    void set_codec(VideoCodec codec) { codec_ = codec; }
    VideoCodec codec() const { return codec_; }

    void set_callback(NalCallback callback) { callback_ = std::move(callback); }

    // Feed one RTP packet. Returns false if the packet was malformed or dropped.
    bool Push(const uint8_t* data, size_t size);

    // Counters for the OSD / diagnostics.
    uint64_t packets() const { return packets_; }
    uint64_t nals() const { return nals_; }
    uint64_t dropped() const { return dropped_; }
    // Fragments discarded because a piece of the fragmented NAL went missing.
    uint64_t broken_fragments() const { return broken_fragments_; }

private:
    void EmitNal(const uint8_t* payload, size_t size, uint32_t timestamp, bool marker);
    bool HandleH264(const uint8_t* payload, size_t size, uint32_t timestamp, bool marker);
    bool HandleH265(const uint8_t* payload, size_t size, uint32_t timestamp, bool marker);
    void ResetFragment();

    VideoCodec codec_;
    NalCallback callback_;

    // Reassembly buffer for FU-A (H.264) / FU (H.265) fragments.
    std::vector<uint8_t> fragment_;
    bool in_fragment_ = false;
    uint16_t expected_seq_ = 0;

    // Scratch buffer holding start code + NAL, reused to avoid per-packet
    // allocation on the video path.
    std::vector<uint8_t> out_;

    uint64_t packets_ = 0;
    uint64_t nals_ = 0;
    uint64_t dropped_ = 0;
    uint64_t broken_fragments_ = 0;
};

// Guess the codec from the first NAL seen, so the user does not have to declare
// it when the air unit's setting is unknown.
VideoCodec DetectCodec(const uint8_t* rtp_packet, size_t size);

}  // namespace openipc::gs
