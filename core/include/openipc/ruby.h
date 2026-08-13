// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

/**
 * A receiver for RubyFPV's over-the-air video, as a spike.
 *
 * This is an independent implementation of the wire format, written to
 * interoperate rather than derived from upstream's source. RubyFPV is published
 * under a custom licence carrying a field-of-use restriction ("Military use is
 * not permitted"), which GPL-3 section 7 does not permit as an additional term
 * - so upstream's code cannot be vendored into this GPL-3.0-only project the
 * way wfb-ng's was. Nothing here is copied from it; the layout constants below
 * describe a protocol, and were verified against upstream's headers by
 * measurement rather than transcription.
 *
 * The consequence to keep in mind: there is no interop test here. The wfb-ng
 * port could be driven by upstream's own transmitter, which is what made it
 * trustworthy. This can only be checked against a real capture.
 *
 * Scope of the spike: receive and reassemble video. Ruby's ground station is a
 * full participant in the link - it pairs, answers clock pings, acknowledges,
 * and requests retransmission of missing packets - and none of that is here.
 * A link driven by this alone would work only as well as its FEC allows.
 */
namespace openipc::ruby {

// --- wire layout ------------------------------------------------------------

/** Bytes in the common radio packet header that fronts every Ruby packet. */
inline constexpr size_t kPacketHeaderSize = 24;

/** Bytes in the video segment header that follows it on video packets. */
inline constexpr size_t kVideoSegmentHeaderSize = 36;

/**
 * Bytes in the small header that fronts each FEC element. It is inside the
 * FEC-protected region rather than in front of it, so it survives recovery -
 * which is what lets a reconstructed element still say how long its video is.
 */
inline constexpr size_t kVideoImportantHeaderSize = 3;

/** Upstream's ceiling on block geometry. */
inline constexpr int kMaxDataPacketsInBlock = 16;
inline constexpr int kMaxEcPacketsInBlock = 16;

/** Component, in the low three bits of packet_flags. */
enum Component : uint8_t {
    kComponentLocalControl = 0,
    kComponentVideo = 1,
    kComponentTelemetry = 2,
    kComponentCommands = 3,
    kComponentRc = 4,
    kComponentRuby = 5,
    kComponentAudio = 6,
};

inline constexpr uint8_t kComponentMask = 0x07;
inline constexpr uint8_t kFlagHeadersOnlyCrc = 1u << 3;
inline constexpr uint8_t kFlagRetransmitted = 1u << 4;
inline constexpr uint8_t kFlagHasEncryption = 1u << 6;
inline constexpr uint8_t kFlagHighPriority = 1u << 7;

inline constexpr uint8_t kPacketTypeVideoData = 22;

/** Video codec, in the high nibble of uVideoStreamIndexAndType. */
enum VideoType : uint8_t {
    kVideoTypeNone = 0,
    kVideoTypeH264 = 1,
    kVideoTypeH265 = 2,
    kVideoTypeRtpH264 = 3,
    kVideoTypeRtpH265 = 4,
};

/** The common header, parsed. */
struct PacketHeader {
    uint32_t crc = 0;
    uint8_t flags = 0;
    uint8_t type = 0;
    uint32_t stream_packet_idx = 0;
    uint16_t flags_extended = 0;
    uint16_t total_length = 0;
    uint16_t radio_link_packet_index = 0;
    uint32_t vehicle_id_src = 0;
    uint32_t vehicle_id_dest = 0;

    uint8_t component() const { return flags & kComponentMask; }
    bool encrypted() const { return (flags & kFlagHasEncryption) != 0; }
    uint8_t stream_id() const { return static_cast<uint8_t>(stream_packet_idx >> 28); }
    uint32_t stream_index() const { return stream_packet_idx & 0x0FFFFFFFu; }
};

/** The video segment header, parsed. Only the fields reassembly needs. */
struct VideoSegmentHeader {
    uint8_t stream_index_and_type = 0;
    uint32_t block_index = 0;
    uint8_t packet_index = 0;
    uint16_t packet_size = 0;
    uint8_t data_packets = 0;
    uint8_t ec_packets = 0;
    uint16_t frame_index = 0;

    uint8_t video_type() const { return static_cast<uint8_t>(stream_index_and_type >> 4); }
};

/** Parse helpers, exposed so tests can assert the layout directly. */
bool ParsePacketHeader(const uint8_t* data, size_t size, PacketHeader* out);
bool ParseVideoSegmentHeader(const uint8_t* data, size_t size, VideoSegmentHeader* out);

/** Ruby's packet checksum: standard CRC-32, over everything after the field. */
uint32_t Crc32(const uint8_t* data, size_t size);

// --- receiver ---------------------------------------------------------------

struct RubyStats {
    uint64_t packets = 0;
    uint64_t crc_errors = 0;
    uint64_t encrypted_dropped = 0;
    uint64_t video_packets = 0;
    uint64_t blocks_completed = 0;   // delivered with every data packet present
    uint64_t blocks_recovered = 0;   // delivered only because FEC filled a gap
    uint64_t blocks_lost = 0;        // too few packets, dropped
    uint64_t video_bytes = 0;
};

/**
 * Reassembles Ruby video blocks into a byte stream.
 *
 * Blocks are held until they are complete, recoverable, or overtaken. Ruby
 * numbers blocks monotonically, so a block older than the newest by more than
 * the depth below can no longer be filled by anything still in flight - with
 * retransmissions unimplemented, that is the point to give up on it.
 */
class RubyReceiver {
public:
    using VideoCallback = std::function<void(const uint8_t* data, size_t size)>;

    explicit RubyReceiver(int reorder_depth = 6);
    ~RubyReceiver();

    RubyReceiver(const RubyReceiver&) = delete;
    RubyReceiver& operator=(const RubyReceiver&) = delete;

    void set_video_callback(VideoCallback cb) { video_cb_ = std::move(cb); }

    /** Feed one radio packet payload, i.e. what sits after the 802.11 header. */
    void ProcessPacket(const uint8_t* data, size_t size);

    /** Release every block still held, in order. Used at end of stream. */
    void Flush();

    const RubyStats& stats() const { return stats_; }
    uint8_t detected_video_type() const { return detected_video_type_; }

private:
    struct Block {
        uint32_t index = 0;
        uint8_t data_packets = 0;
        uint8_t ec_packets = 0;
        uint16_t packet_size = 0;
        int received_data = 0;
        int received_ec = 0;
        std::vector<std::vector<uint8_t>> elements;  // data_packets + ec_packets
        std::vector<bool> present;
    };

    void EmitBlock(Block& block);

    /**
     * Deliver from the front of the map only.
     *
     * A block that is already complete still has to wait behind an older one
     * that is not, or the byte stream reaches the decoder out of order and the
     * picture breaks in a way that looks like corruption rather than a bug.
     * That is the whole reason delivery is a queue and not an event.
     */
    void TryDeliver();

    VideoCallback video_cb_;
    std::map<uint32_t, Block> blocks_;
    int reorder_depth_;
    bool have_newest_ = false;
    uint32_t newest_block_ = 0;
    uint8_t detected_video_type_ = kVideoTypeNone;
    RubyStats stats_;
};

}  // namespace openipc::ruby
