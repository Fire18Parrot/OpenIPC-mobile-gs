// SPDX-License-Identifier: GPL-3.0-only
//
// Spike test for the RubyFPV receiver.
//
// What this proves: that the parser agrees with the wire layout measured from
// upstream's headers, and that block reassembly and FEC recovery behave as the
// format requires under loss.
//
// What it does NOT prove: that the layout reading is right. The wfb-ng port
// could be driven by upstream's own transmitter, so its interop test compared
// us against them. RubyFPV's licence carries a field-of-use restriction that
// GPL-3 cannot absorb, so upstream's code cannot be linked here and the
// transmitter side below is ours too. A capture from a real air unit is the
// only thing that will settle it.

#include "openipc/ruby.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

extern "C" {
#include "zfex.h"
}

using namespace openipc::ruby;

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& what) {
    if (!condition) {
        std::printf("  FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

void Write16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void Write32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>(v >> 24);
}

/** One Ruby video packet, built the way an air unit builds it. */
std::vector<uint8_t> BuildVideoPacket(uint32_t block_index, uint8_t packet_index,
                                      uint8_t data_packets, uint8_t ec_packets,
                                      uint16_t packet_size, uint8_t video_type,
                                      const std::vector<uint8_t>& element) {
    std::vector<uint8_t> packet(kPacketHeaderSize + kVideoSegmentHeaderSize + packet_size, 0);

    uint8_t* vs = packet.data() + kPacketHeaderSize;
    vs[0] = static_cast<uint8_t>(video_type << 4);       // stream 0, type in high nibble
    Write32(vs + 17, block_index);
    vs[21] = packet_index;
    Write16(vs + 22, packet_size);
    vs[24] = data_packets;
    vs[25] = ec_packets;
    Write16(vs + 26, static_cast<uint16_t>(block_index));  // frame index, unused here

    std::memcpy(vs + kVideoSegmentHeaderSize, element.data(), packet_size);

    packet[4] = kComponentVideo;                          // packet_flags
    packet[5] = kPacketTypeVideoData;                     // packet_type
    Write32(packet.data() + 6, 0);                        // stream 0, index 0
    Write16(packet.data() + 10, 0);                       // flags_extended
    Write16(packet.data() + 12, static_cast<uint16_t>(packet.size()));
    Write16(packet.data() + 14, 0);                       // radio_link_packet_index
    Write32(packet.data() + 16, 0x1234);                  // vehicle_id_src
    Write32(packet.data() + 20, 0);                       // vehicle_id_dest

    const uint32_t crc = Crc32(packet.data() + 4, packet.size() - 4);
    Write32(packet.data(), crc);
    return packet;
}

/**
 * Build a whole block: chop payload into k elements, each fronted by its
 * length, then produce the parity elements with the same Rizzo FEC Ruby uses.
 */
std::vector<std::vector<uint8_t>> BuildBlock(const std::vector<uint8_t>& payload, int k, int ec,
                                             uint16_t packet_size) {
    const size_t usable = packet_size - kVideoImportantHeaderSize;
    std::vector<std::vector<uint8_t>> elements(static_cast<size_t>(k + ec),
                                               std::vector<uint8_t>(packet_size, 0));

    size_t offset = 0;
    for (int i = 0; i < k; ++i) {
        const size_t take = std::min(usable, payload.size() - std::min(offset, payload.size()));
        Write16(elements[i].data(), static_cast<uint16_t>(take));
        elements[i][2] = 0;  // important flags
        if (take > 0) {
            std::memcpy(elements[i].data() + kVideoImportantHeaderSize, payload.data() + offset,
                        take);
            offset += take;
        }
    }

    fec_t* fec = nullptr;
    const zfex_status_code_t rc = fec_new(static_cast<uint16_t>(k), static_cast<uint16_t>(k + ec),
                                          &fec);
    assert(rc == ZFEX_SC_OK && fec != nullptr);
    (void)rc;

    std::vector<const uint8_t*> src(static_cast<size_t>(k));
    for (int i = 0; i < k; ++i) {
        src[i] = elements[i].data();
    }
    std::vector<uint8_t*> parity(static_cast<size_t>(ec));
    for (int i = 0; i < ec; ++i) {
        parity[i] = elements[static_cast<size_t>(k + i)].data();
    }
    fec_encode_simd(fec, src.data(), parity.data(), packet_size);
    fec_free(fec);
    return elements;
}

std::vector<uint8_t> MakePayload(size_t bytes, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> out(bytes);
    for (auto& b : out) {
        b = static_cast<uint8_t>(rng() & 0xFF);
    }
    return out;
}

// --- the checks -------------------------------------------------------------

void TestLayout() {
    std::printf("layout\n");
    // The sizes the protocol is built around. If any of these drift, every
    // offset below them is wrong and nothing else in this file means anything.
    Check(kPacketHeaderSize == 24, "packet header is 24 bytes");
    Check(kVideoSegmentHeaderSize == 36, "video segment header is 36 bytes");
    Check(kVideoImportantHeaderSize == 3, "important header is 3 bytes");

    // A header round-trip through the parser, field by field.
    std::vector<uint8_t> raw(kPacketHeaderSize, 0);
    Write32(raw.data() + 0, 0xAABBCCDD);
    raw[4] = kComponentVideo | kFlagHighPriority;
    raw[5] = kPacketTypeVideoData;
    Write32(raw.data() + 6, (static_cast<uint32_t>(3) << 28) | 0x0123456);
    Write16(raw.data() + 10, 0x0400);
    Write16(raw.data() + 12, 24);
    Write16(raw.data() + 14, 0x1111);
    Write32(raw.data() + 16, 0xDEADBEEF);
    Write32(raw.data() + 20, 0xFEEDFACE);

    PacketHeader h;
    Check(ParsePacketHeader(raw.data(), raw.size(), &h), "header parses");
    Check(h.crc == 0xAABBCCDD, "crc field");
    Check(h.component() == kComponentVideo, "component from low three bits");
    Check(h.type == kPacketTypeVideoData, "packet type");
    Check(h.stream_id() == 3, "stream id from high four bits");
    Check(h.stream_index() == 0x0123456, "stream index from low 28 bits");
    Check(h.total_length == 24, "total length");
    Check(h.vehicle_id_src == 0xDEADBEEF, "vehicle id src");
    Check(h.vehicle_id_dest == 0xFEEDFACE, "vehicle id dest");
    Check(!h.encrypted(), "encryption bit clear");

    raw[4] |= kFlagHasEncryption;
    Check(ParsePacketHeader(raw.data(), raw.size(), &h) && h.encrypted(), "encryption bit set");

    Check(!ParsePacketHeader(raw.data(), kPacketHeaderSize - 1, &h), "short header rejected");
}

void TestCrc() {
    std::printf("crc\n");
    // Standard CRC-32 against its documented check value.
    const char* check = "123456789";
    const uint32_t crc = Crc32(reinterpret_cast<const uint8_t*>(check), 9);
    Check(crc == 0xCBF43926, "CRC-32 check value is 0xCBF43926");
}

void TestCleanBlock() {
    std::printf("clean block\n");
    const int k = 8, ec = 4;
    const uint16_t packet_size = 128;
    const auto payload = MakePayload((packet_size - kVideoImportantHeaderSize) * k, 1);
    const auto elements = BuildBlock(payload, k, ec, packet_size);

    RubyReceiver rx;
    std::vector<uint8_t> received;
    rx.set_video_callback([&](const uint8_t* d, size_t n) {
        received.insert(received.end(), d, d + n);
    });

    for (int i = 0; i < k; ++i) {
        const auto packet = BuildVideoPacket(0, static_cast<uint8_t>(i), k, ec, packet_size,
                                             kVideoTypeH265, elements[i]);
        rx.ProcessPacket(packet.data(), packet.size());
    }

    Check(rx.stats().crc_errors == 0, "no crc errors");
    Check(rx.stats().video_packets == static_cast<uint64_t>(k), "all video packets counted");
    Check(rx.stats().blocks_completed == 1, "block completed without FEC");
    Check(rx.stats().blocks_recovered == 0, "no recovery needed");
    Check(received == payload, "payload reassembles byte for byte");
    Check(rx.detected_video_type() == kVideoTypeH265, "codec read from the high nibble");
}

void TestRecoversFromLoss() {
    std::printf("loss within FEC\n");
    const int k = 8, ec = 4;
    const uint16_t packet_size = 256;
    const auto payload = MakePayload((packet_size - kVideoImportantHeaderSize) * k, 2);
    const auto elements = BuildBlock(payload, k, ec, packet_size);

    // Drop as many data packets as there is parity to replace them - the worst
    // case FEC is meant to survive.
    for (int drop_count = 1; drop_count <= ec; ++drop_count) {
        RubyReceiver rx;
        std::vector<uint8_t> received;
        rx.set_video_callback([&](const uint8_t* d, size_t n) {
            received.insert(received.end(), d, d + n);
        });

        for (int i = 0; i < k + ec; ++i) {
            if (i < drop_count) {
                continue;  // lose the first drop_count data packets
            }
            const auto packet = BuildVideoPacket(7, static_cast<uint8_t>(i), k, ec, packet_size,
                                                 kVideoTypeH264, elements[i]);
            rx.ProcessPacket(packet.data(), packet.size());
        }
        rx.Flush();

        Check(received == payload,
              "payload recovered after losing " + std::to_string(drop_count) + " data packets");
        Check(rx.stats().blocks_recovered == 1,
              "counted as recovered, " + std::to_string(drop_count) + " lost");
        Check(rx.stats().blocks_lost == 0,
              "nothing given up on, " + std::to_string(drop_count) + " lost");
    }
}

void TestBeyondFec() {
    std::printf("loss beyond FEC\n");
    const int k = 8, ec = 2;
    const uint16_t packet_size = 128;
    const auto payload = MakePayload((packet_size - kVideoImportantHeaderSize) * k, 3);
    const auto elements = BuildBlock(payload, k, ec, packet_size);

    RubyReceiver rx;
    size_t emitted = 0;
    rx.set_video_callback([&](const uint8_t*, size_t n) { emitted += n; });

    // Lose three data packets with only two parity: unrecoverable by design.
    for (int i = 3; i < k + ec; ++i) {
        const auto packet = BuildVideoPacket(1, static_cast<uint8_t>(i), k, ec, packet_size,
                                             kVideoTypeH265, elements[i]);
        rx.ProcessPacket(packet.data(), packet.size());
    }
    rx.Flush();

    Check(rx.stats().blocks_lost == 1, "block given up on rather than emitted as garbage");
    Check(emitted == 0, "nothing delivered from an unrecoverable block");
}

void TestRejectsCorruption() {
    std::printf("corruption and encryption\n");
    const int k = 4, ec = 2;
    const uint16_t packet_size = 64;
    const auto payload = MakePayload((packet_size - kVideoImportantHeaderSize) * k, 4);
    const auto elements = BuildBlock(payload, k, ec, packet_size);

    RubyReceiver rx;
    auto packet = BuildVideoPacket(0, 0, k, ec, packet_size, kVideoTypeH265, elements[0]);

    // A flipped bit anywhere after the checksum must be caught.
    packet[40] ^= 0x20;
    rx.ProcessPacket(packet.data(), packet.size());
    Check(rx.stats().crc_errors == 1, "corrupted packet rejected by CRC");
    Check(rx.stats().video_packets == 0, "corrupted packet not counted as video");

    // A truncated packet must not be read past its end.
    auto truncated = BuildVideoPacket(0, 1, k, ec, packet_size, kVideoTypeH265, elements[1]);
    truncated.resize(truncated.size() / 2);
    rx.ProcessPacket(truncated.data(), truncated.size());
    Check(rx.stats().crc_errors == 2, "truncated packet rejected");

    // An encrypted packet is dropped deliberately, not parsed as plaintext.
    auto encrypted = BuildVideoPacket(0, 2, k, ec, packet_size, kVideoTypeH265, elements[2]);
    encrypted[4] |= kFlagHasEncryption;
    Write32(encrypted.data(), Crc32(encrypted.data() + 4, encrypted.size() - 4));
    rx.ProcessPacket(encrypted.data(), encrypted.size());
    Check(rx.stats().encrypted_dropped == 1, "encrypted packet counted and dropped");
}

void TestOutOfOrderAndRandomLoss() {
    std::printf("reordering and random loss\n");
    const int k = 8, ec = 4;
    const uint16_t packet_size = 512;
    const int blocks = 40;

    std::mt19937 rng(99);
    RubyReceiver rx;
    std::vector<uint8_t> received;
    rx.set_video_callback([&](const uint8_t* d, size_t n) {
        received.insert(received.end(), d, d + n);
    });

    std::vector<uint8_t> expected;
    for (int b = 0; b < blocks; ++b) {
        const auto payload =
            MakePayload((packet_size - kVideoImportantHeaderSize) * k, 1000 + b);
        expected.insert(expected.end(), payload.begin(), payload.end());
        const auto elements = BuildBlock(payload, k, ec, packet_size);

        // Shuffle within the block and lose up to ec packets - i.e. exactly
        // what a marginal link does, minus the retransmissions Ruby would use.
        std::vector<int> order(static_cast<size_t>(k + ec));
        for (int i = 0; i < k + ec; ++i) {
            order[static_cast<size_t>(i)] = i;
        }
        std::shuffle(order.begin(), order.end(), rng);
        const int drop = static_cast<int>(rng() % (ec + 1));

        for (int i = drop; i < k + ec; ++i) {
            const int idx = order[static_cast<size_t>(i)];
            const auto packet = BuildVideoPacket(static_cast<uint32_t>(b),
                                                 static_cast<uint8_t>(idx), k, ec, packet_size,
                                                 kVideoTypeH265, elements[static_cast<size_t>(idx)]);
            rx.ProcessPacket(packet.data(), packet.size());
        }
    }
    rx.Flush();

    Check(received == expected, "every block reassembles in order under shuffle and loss");
    Check(rx.stats().blocks_lost == 0, "nothing lost while inside the FEC budget");
    Check(rx.stats().blocks_completed + rx.stats().blocks_recovered ==
              static_cast<uint64_t>(blocks),
          "every block accounted for");
}

}  // namespace

int main() {
    std::printf("RubyFPV receiver spike\n\n");
    TestLayout();
    TestCrc();
    TestCleanBlock();
    TestRecoversFromLoss();
    TestBeyondFec();
    TestRejectsCorruption();
    TestOutOfOrderAndRandomLoss();

    std::printf("\n%s\n", g_failures == 0 ? "all checks passed" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
