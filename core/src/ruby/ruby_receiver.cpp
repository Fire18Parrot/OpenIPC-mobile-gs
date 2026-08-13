// SPDX-License-Identifier: GPL-3.0-only
#include "openipc/ruby.h"

#include <cstring>

extern "C" {
#include "zfex.h"
}

namespace openipc::ruby {
namespace {

// Little-endian readers. Ruby's headers are packed structs written by an
// ARM/x86 host, so the wire is little-endian; reading field by field rather
// than casting a struct over the buffer keeps this correct on any host and
// avoids an unaligned access.
uint16_t Read16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t Read32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Field offsets, measured against upstream's own headers rather than counted
// by hand. The test asserts these against the sizes the protocol requires.
constexpr size_t kOffCrc = 0;
constexpr size_t kOffFlags = 4;
constexpr size_t kOffType = 5;
constexpr size_t kOffStreamIdx = 6;
constexpr size_t kOffFlagsExt = 10;
constexpr size_t kOffTotalLength = 12;
constexpr size_t kOffRadioLinkIdx = 14;
constexpr size_t kOffVehicleSrc = 16;
constexpr size_t kOffVehicleDest = 20;

constexpr size_t kOffVsStreamAndType = 0;
constexpr size_t kOffVsBlockIndex = 17;
constexpr size_t kOffVsPacketIndex = 21;
constexpr size_t kOffVsPacketSize = 22;
constexpr size_t kOffVsDataPackets = 24;
constexpr size_t kOffVsEcPackets = 25;
constexpr size_t kOffVsFrameIndex = 26;

const uint32_t* Crc32Table() {
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        // The standard reflected CRC-32 polynomial, which is what Ruby uses.
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        built = true;
    }
    return table;
}

}  // namespace

uint32_t Crc32(const uint8_t* data, size_t size) {
    const uint32_t* table = Crc32Table();
    uint32_t crc = ~0u;
    for (size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ ~0u;
}

bool ParsePacketHeader(const uint8_t* data, size_t size, PacketHeader* out) {
    if (data == nullptr || size < kPacketHeaderSize || out == nullptr) {
        return false;
    }
    out->crc = Read32(data + kOffCrc);
    out->flags = data[kOffFlags];
    out->type = data[kOffType];
    out->stream_packet_idx = Read32(data + kOffStreamIdx);
    out->flags_extended = Read16(data + kOffFlagsExt);
    out->total_length = Read16(data + kOffTotalLength);
    out->radio_link_packet_index = Read16(data + kOffRadioLinkIdx);
    out->vehicle_id_src = Read32(data + kOffVehicleSrc);
    out->vehicle_id_dest = Read32(data + kOffVehicleDest);
    return true;
}

bool ParseVideoSegmentHeader(const uint8_t* data, size_t size, VideoSegmentHeader* out) {
    if (data == nullptr || size < kVideoSegmentHeaderSize || out == nullptr) {
        return false;
    }
    out->stream_index_and_type = data[kOffVsStreamAndType];
    out->block_index = Read32(data + kOffVsBlockIndex);
    out->packet_index = data[kOffVsPacketIndex];
    out->packet_size = Read16(data + kOffVsPacketSize);
    out->data_packets = data[kOffVsDataPackets];
    out->ec_packets = data[kOffVsEcPackets];
    out->frame_index = Read16(data + kOffVsFrameIndex);
    return true;
}

RubyReceiver::RubyReceiver(int reorder_depth) : reorder_depth_(reorder_depth) {}

RubyReceiver::~RubyReceiver() = default;

void RubyReceiver::ProcessPacket(const uint8_t* data, size_t size) {
    PacketHeader header;
    if (!ParsePacketHeader(data, size, &header)) {
        return;
    }
    ++stats_.packets;

    // total_length counts every header and the CRC, so a packet claiming more
    // than arrived is truncated and cannot be checksummed.
    if (header.total_length > size || header.total_length < kPacketHeaderSize) {
        ++stats_.crc_errors;
        return;
    }

    // The checksum starts after itself. With the headers-only bit set it covers
    // just the header, which is how Ruby avoids checksumming a video payload
    // that FEC is already protecting.
    const size_t crc_len = (header.flags & kFlagHeadersOnlyCrc)
                               ? kPacketHeaderSize - sizeof(uint32_t)
                               : header.total_length - sizeof(uint32_t);
    if (Crc32(data + sizeof(uint32_t), crc_len) != header.crc) {
        ++stats_.crc_errors;
        return;
    }

    // Encryption covers everything after packet_flags. Ruby's own README calls
    // it temporarily disabled, and no key exchange is implemented here, so an
    // encrypted packet is counted and dropped rather than parsed as garbage.
    if (header.encrypted()) {
        ++stats_.encrypted_dropped;
        return;
    }

    if (header.component() != kComponentVideo || header.type != kPacketTypeVideoData) {
        return;
    }

    const uint8_t* vs = data + kPacketHeaderSize;
    const size_t vs_size = header.total_length - kPacketHeaderSize;
    VideoSegmentHeader video;
    if (!ParseVideoSegmentHeader(vs, vs_size, &video)) {
        return;
    }
    ++stats_.video_packets;
    detected_video_type_ = video.video_type();

    const int k = video.data_packets;
    const int ec = video.ec_packets;
    if (k <= 0 || k > kMaxDataPacketsInBlock || ec < 0 || ec > kMaxEcPacketsInBlock) {
        return;
    }
    if (video.packet_index >= k + ec) {
        return;
    }
    if (video.packet_size < kVideoImportantHeaderSize) {
        return;
    }

    // Every packet in a block carries exactly packet_size bytes of FEC element,
    // data and parity alike - that uniformity is what FEC requires.
    const uint8_t* element = vs + kVideoSegmentHeaderSize;
    const size_t available = vs_size - kVideoSegmentHeaderSize;
    if (available < video.packet_size) {
        return;
    }

    Block& block = blocks_[video.block_index];
    if (block.elements.empty()) {
        block.index = video.block_index;
        block.data_packets = video.data_packets;
        block.ec_packets = video.ec_packets;
        block.packet_size = video.packet_size;
        block.elements.assign(static_cast<size_t>(k + ec), {});
        block.present.assign(static_cast<size_t>(k + ec), false);
    } else if (block.data_packets != video.data_packets ||
               block.ec_packets != video.ec_packets ||
               block.packet_size != video.packet_size) {
        // Ruby retunes its EC scheme on the fly. A block whose geometry changed
        // mid-flight cannot be decoded from a mixture, so start it over.
        blocks_.erase(video.block_index);
        return;
    }

    if (!block.present[video.packet_index]) {
        block.elements[video.packet_index].assign(element, element + video.packet_size);
        block.present[video.packet_index] = true;
        if (video.packet_index < k) {
            ++block.received_data;
        } else {
            ++block.received_ec;
        }
    }

    if (!have_newest_ || video.block_index > newest_block_) {
        have_newest_ = true;
        newest_block_ = video.block_index;
    }

    TryDeliver();
}

void RubyReceiver::TryDeliver() {
    while (!blocks_.empty()) {
        auto it = blocks_.begin();
        Block& block = it->second;
        const int k = block.data_packets;

        // Complete: every data element is here, no FEC needed. Because this is
        // the oldest block held, emitting it cannot get ahead of anything.
        if (block.received_data >= k) {
            EmitBlock(block);
            blocks_.erase(it);
            continue;
        }

        // Not complete. Give the missing data packets until the stream has
        // moved far enough past this block that they cannot still be in
        // flight - reordering is the reason to wait, and a fixed depth is the
        // cheapest way to bound that wait. Anything newer stays queued behind
        // this one however complete it is, because the decoder needs bytes in
        // order far more than it needs them early.
        const bool overtaken =
            have_newest_ && newest_block_ >= it->first &&
            (newest_block_ - it->first) >= static_cast<uint32_t>(reorder_depth_);
        if (!overtaken) {
            break;
        }

        if (block.received_data + block.received_ec >= k) {
            EmitBlock(block);
        } else {
            ++stats_.blocks_lost;
        }
        blocks_.erase(it);
    }
}

void RubyReceiver::EmitBlock(Block& block) {
    const int k = block.data_packets;
    const int n = k + block.ec_packets;
    const size_t sz = block.packet_size;

    if (block.received_data < k) {
        // Rebuild the missing data elements. zfex wants the k blocks it is
        // given laid out so that a present primary block sits at its own index
        // and every gap is filled by some parity block, with index[] recording
        // which block each slot actually holds. This is the same convention
        // wfb-ng's receiver uses, because it is the same Rizzo FEC underneath -
        // which is the one piece of Ruby that did not need reimplementing.
        fec_t* fec = nullptr;
        if (fec_new(static_cast<uint16_t>(k), static_cast<uint16_t>(n), &fec) != ZFEX_SC_OK ||
            fec == nullptr) {
            ++stats_.blocks_lost;
            return;
        }

        std::vector<const uint8_t*> in_blocks(static_cast<size_t>(k), nullptr);
        std::vector<uint8_t*> out_blocks;
        std::vector<unsigned int> index(static_cast<size_t>(k), 0);

        int spare = k;  // next parity block to press into service
        bool ok = true;
        for (int i = 0; i < k; ++i) {
            if (block.present[i]) {
                in_blocks[i] = block.elements[i].data();
                index[i] = static_cast<unsigned int>(i);
                continue;
            }
            while (spare < n && !block.present[spare]) {
                ++spare;
            }
            if (spare >= n) {
                ok = false;
                break;
            }
            block.elements[i].assign(sz, 0);
            in_blocks[i] = block.elements[spare].data();
            index[i] = static_cast<unsigned int>(spare);
            out_blocks.push_back(block.elements[i].data());
            ++spare;
        }

        if (!ok) {
            fec_free(fec);
            ++stats_.blocks_lost;
            return;
        }

        const zfex_status_code_t rc = fec_decode_simd(
            fec, in_blocks.data(), out_blocks.data(), index.data(), sz);
        fec_free(fec);
        if (rc != ZFEX_SC_OK) {
            ++stats_.blocks_lost;
            return;
        }
        ++stats_.blocks_recovered;
    } else {
        ++stats_.blocks_completed;
    }

    // Each recovered element opens with its own length, which is why that
    // header sits inside the FEC region: a reconstructed element still knows
    // how much of itself is video and how much is padding.
    for (int i = 0; i < k; ++i) {
        const std::vector<uint8_t>& element = block.elements[i];
        if (element.size() < kVideoImportantHeaderSize) {
            continue;
        }
        const uint16_t length = Read16(element.data());
        if (length == 0 || length > element.size() - kVideoImportantHeaderSize) {
            continue;
        }
        stats_.video_bytes += length;
        if (video_cb_) {
            video_cb_(element.data() + kVideoImportantHeaderSize, length);
        }
    }
}

void RubyReceiver::Flush() {
    while (!blocks_.empty()) {
        auto it = blocks_.begin();
        Block& block = it->second;
        if (block.received_data + block.received_ec >= block.data_packets) {
            EmitBlock(block);
        } else {
            ++stats_.blocks_lost;
        }
        blocks_.erase(it);
    }
}

}  // namespace openipc::ruby
