// SPDX-License-Identifier: GPL-3.0-only

#include "openipc/rtp.h"

#include <cstring>

namespace openipc::gs {
namespace {

constexpr size_t kRtpHeaderSize = 12;
constexpr uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};

struct RtpHeader {
    uint8_t payload_type = 0;
    uint16_t sequence = 0;
    uint32_t timestamp = 0;
    bool marker = false;
    size_t payload_offset = 0;
};

// Parse the fixed header plus CSRC list and any extension, per RFC 3550.
bool ParseRtp(const uint8_t* data, size_t size, RtpHeader* out) {
    if (size < kRtpHeaderSize) {
        return false;
    }
    const uint8_t version = (data[0] >> 6) & 0x03;
    if (version != 2) {
        return false;
    }
    const bool padding = (data[0] & 0x20) != 0;
    const bool extension = (data[0] & 0x10) != 0;
    const uint8_t csrc_count = data[0] & 0x0f;

    out->marker = (data[1] & 0x80) != 0;
    out->payload_type = data[1] & 0x7f;
    out->sequence = static_cast<uint16_t>((data[2] << 8) | data[3]);
    out->timestamp = (static_cast<uint32_t>(data[4]) << 24) | (static_cast<uint32_t>(data[5]) << 16) |
                     (static_cast<uint32_t>(data[6]) << 8) | static_cast<uint32_t>(data[7]);

    size_t offset = kRtpHeaderSize + static_cast<size_t>(csrc_count) * 4;
    if (offset > size) {
        return false;
    }
    if (extension) {
        if (offset + 4 > size) {
            return false;
        }
        const size_t ext_words = (static_cast<size_t>(data[offset + 2]) << 8) | data[offset + 3];
        offset += 4 + ext_words * 4;
        if (offset > size) {
            return false;
        }
    }
    if (padding) {
        if (size == 0) {
            return false;
        }
        const uint8_t pad = data[size - 1];
        if (pad == 0 || offset + pad > size) {
            return false;
        }
        // Caller uses size - pad; encode by shrinking through the payload span.
        // Handled by the caller via the returned offset and the adjusted size.
    }
    out->payload_offset = offset;
    return true;
}

size_t PayloadSize(const uint8_t* data, size_t size, size_t offset) {
    const bool padding = (data[0] & 0x20) != 0;
    size_t end = size;
    if (padding && size > 0) {
        const uint8_t pad = data[size - 1];
        if (pad > 0 && offset + pad <= size) {
            end = size - pad;
        }
    }
    return end > offset ? end - offset : 0;
}

}  // namespace

RtpDepacketizer::RtpDepacketizer(VideoCodec codec) : codec_(codec) {
    fragment_.reserve(64 * 1024);
    out_.reserve(64 * 1024);
}

void RtpDepacketizer::ResetFragment() {
    fragment_.clear();
    in_fragment_ = false;
}

void RtpDepacketizer::EmitNal(const uint8_t* payload, size_t size, uint32_t timestamp, bool marker) {
    if (size == 0 || !callback_) {
        return;
    }
    out_.clear();
    out_.insert(out_.end(), kStartCode, kStartCode + sizeof(kStartCode));
    out_.insert(out_.end(), payload, payload + size);

    NalUnit nal;
    nal.data = out_.data();
    nal.size = out_.size();
    nal.rtp_timestamp = timestamp;
    nal.marker = marker;
    ++nals_;
    callback_(nal);
}

bool RtpDepacketizer::HandleH264(const uint8_t* payload, size_t size, uint32_t timestamp, bool marker) {
    if (size < 1) {
        return false;
    }
    const uint8_t nal_type = payload[0] & 0x1f;

    if (nal_type >= 1 && nal_type <= 23) {
        // Single NAL unit packet.
        EmitNal(payload, size, timestamp, marker);
        return true;
    }

    if (nal_type == 24) {
        // STAP-A: one or more NALs, each preceded by a 16-bit size.
        size_t offset = 1;
        while (offset + 2 <= size) {
            const size_t nal_size = (static_cast<size_t>(payload[offset]) << 8) | payload[offset + 1];
            offset += 2;
            if (nal_size == 0 || offset + nal_size > size) {
                ++dropped_;
                return false;
            }
            EmitNal(payload + offset, nal_size, timestamp, marker);
            offset += nal_size;
        }
        return true;
    }

    if (nal_type == 28) {
        // FU-A: fragmentation unit. Byte 0 is the FU indicator, byte 1 the FU
        // header carrying start/end bits and the real NAL type.
        if (size < 2) {
            return false;
        }
        const uint8_t fu_header = payload[1];
        const bool start = (fu_header & 0x80) != 0;
        const bool end = (fu_header & 0x40) != 0;
        const uint8_t real_type = fu_header & 0x1f;

        if (start) {
            fragment_.clear();
            // Rebuild the original NAL header: F and NRI from the indicator,
            // type from the FU header.
            fragment_.push_back(static_cast<uint8_t>((payload[0] & 0xe0) | real_type));
            in_fragment_ = true;
        } else if (!in_fragment_) {
            // Missed the start fragment; the rest of this NAL is unusable.
            ++broken_fragments_;
            return false;
        }

        fragment_.insert(fragment_.end(), payload + 2, payload + size);

        if (end) {
            EmitNal(fragment_.data(), fragment_.size(), timestamp, marker);
            ResetFragment();
        }
        return true;
    }

    // STAP-B / MTAP / FU-B are not used by any OpenIPC air unit.
    ++dropped_;
    return false;
}

bool RtpDepacketizer::HandleH265(const uint8_t* payload, size_t size, uint32_t timestamp, bool marker) {
    if (size < 2) {
        return false;
    }
    // H.265 has a 2-byte NAL header; the type is bits 1..6 of the first byte.
    const uint8_t nal_type = (payload[0] >> 1) & 0x3f;

    if (nal_type <= 47) {
        EmitNal(payload, size, timestamp, marker);
        return true;
    }

    if (nal_type == 48) {
        // AP: aggregation packet, same layout idea as STAP-A but with the
        // 2-byte PayloadHdr consumed first.
        size_t offset = 2;
        while (offset + 2 <= size) {
            const size_t nal_size = (static_cast<size_t>(payload[offset]) << 8) | payload[offset + 1];
            offset += 2;
            if (nal_size == 0 || offset + nal_size > size) {
                ++dropped_;
                return false;
            }
            EmitNal(payload + offset, nal_size, timestamp, marker);
            offset += nal_size;
        }
        return true;
    }

    if (nal_type == 49) {
        // FU: PayloadHdr (2 bytes) + FU header (1 byte) + payload.
        if (size < 3) {
            return false;
        }
        const uint8_t fu_header = payload[2];
        const bool start = (fu_header & 0x80) != 0;
        const bool end = (fu_header & 0x40) != 0;
        const uint8_t real_type = fu_header & 0x3f;

        if (start) {
            fragment_.clear();
            // Reconstruct the 2-byte header with the real type spliced in,
            // preserving the layer id and TID from the payload header.
            fragment_.push_back(static_cast<uint8_t>((payload[0] & 0x81) | (real_type << 1)));
            fragment_.push_back(payload[1]);
            in_fragment_ = true;
        } else if (!in_fragment_) {
            ++broken_fragments_;
            return false;
        }

        fragment_.insert(fragment_.end(), payload + 3, payload + size);

        if (end) {
            EmitNal(fragment_.data(), fragment_.size(), timestamp, marker);
            ResetFragment();
        }
        return true;
    }

    ++dropped_;
    return false;
}

bool RtpDepacketizer::Push(const uint8_t* data, size_t size) {
    ++packets_;

    RtpHeader header;
    if (!ParseRtp(data, size, &header)) {
        ++dropped_;
        return false;
    }
    const size_t payload_size = PayloadSize(data, size, header.payload_offset);
    if (payload_size == 0) {
        ++dropped_;
        return false;
    }
    const uint8_t* payload = data + header.payload_offset;

    // A gap in the RTP sequence while a fragmented NAL is in flight means the
    // NAL can never be completed - drop it rather than splicing a hole into the
    // bitstream, which the decoder would render as corruption.
    if (in_fragment_ && header.sequence != expected_seq_) {
        ++broken_fragments_;
        ResetFragment();
    }
    expected_seq_ = static_cast<uint16_t>(header.sequence + 1);

    if (codec_ == VideoCodec::kUnknown) {
        codec_ = DetectCodec(data, size);
        if (codec_ == VideoCodec::kUnknown) {
            ++dropped_;
            return false;
        }
    }

    if (codec_ == VideoCodec::kH264) {
        return HandleH264(payload, payload_size, header.timestamp, header.marker);
    }
    return HandleH265(payload, payload_size, header.timestamp, header.marker);
}

VideoCodec DetectCodec(const uint8_t* rtp_packet, size_t size) {
    RtpHeader header;
    if (!ParseRtp(rtp_packet, size, &header)) {
        return VideoCodec::kUnknown;
    }
    const size_t payload_size = PayloadSize(rtp_packet, size, header.payload_offset);
    if (payload_size < 2) {
        return VideoCodec::kUnknown;
    }
    const uint8_t* payload = rtp_packet + header.payload_offset;

    // The forbidden_zero_bit is 0 in both codecs. In H.265 the second header
    // byte's low 3 bits are the temporal id plus one, so they are never 0;
    // that plus a plausible NAL type is enough to tell the two apart on the
    // first packet, which is all we need before the user picks explicitly.
    const uint8_t h265_type = (payload[0] >> 1) & 0x3f;
    const uint8_t h265_tid = payload[1] & 0x07;
    if ((payload[0] & 0x80) == 0 && h265_tid != 0 &&
        (h265_type <= 40 || h265_type == 48 || h265_type == 49)) {
        return VideoCodec::kH265;
    }

    const uint8_t h264_type = payload[0] & 0x1f;
    if ((payload[0] & 0x80) == 0 && h264_type != 0 &&
        (h264_type <= 23 || h264_type == 24 || h264_type == 28)) {
        return VideoCodec::kH264;
    }
    return VideoCodec::kUnknown;
}

}  // namespace openipc::gs
