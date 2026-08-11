// SPDX-License-Identifier: GPL-3.0-only
//
// MSP DisplayPort decoding, covering both MSP v1 and v2 framing and the
// subcommands msposd actually sends.

#include <cstdint>
#include <string>
#include <vector>

#include "openipc/msp_osd.h"
#include "test_util.h"

namespace {

using openipc::gs::MspOsdDecoder;
using openipc::gs::OsdFrame;

constexpr uint16_t kMspDisplayPort = 182;

std::vector<uint8_t> BuildV1(uint8_t command, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame = {'$', 'M', '>'};
    frame.push_back(static_cast<uint8_t>(payload.size()));
    frame.push_back(command);
    uint8_t checksum = static_cast<uint8_t>(payload.size()) ^ command;
    for (uint8_t byte : payload) {
        frame.push_back(byte);
        checksum ^= byte;
    }
    frame.push_back(checksum);
    return frame;
}

uint8_t Crc8DvbS2(uint8_t crc, uint8_t byte) {
    crc ^= byte;
    for (int i = 0; i < 8; ++i) {
        crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0xd5) : static_cast<uint8_t>(crc << 1);
    }
    return crc;
}

std::vector<uint8_t> BuildV2(uint16_t function, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame = {'$', 'X', '>'};
    std::vector<uint8_t> body = {
        0x00,
        static_cast<uint8_t>(function & 0xff),
        static_cast<uint8_t>(function >> 8),
        static_cast<uint8_t>(payload.size() & 0xff),
        static_cast<uint8_t>(payload.size() >> 8),
    };
    body.insert(body.end(), payload.begin(), payload.end());
    uint8_t crc = 0;
    for (uint8_t byte : body) {
        crc = Crc8DvbS2(crc, byte);
    }
    frame.insert(frame.end(), body.begin(), body.end());
    frame.push_back(crc);
    return frame;
}

// WRITE_STRING payload: subcommand, row, col, attribute, characters.
std::vector<uint8_t> WriteString(uint8_t row, uint8_t col, uint8_t attribute,
                                 const std::string& text) {
    std::vector<uint8_t> payload = {3, row, col, attribute};
    payload.insert(payload.end(), text.begin(), text.end());
    return payload;
}

std::string ReadRow(const OsdFrame& frame, int row, int col, int length) {
    std::string text;
    for (int i = 0; i < length; ++i) {
        text.push_back(static_cast<char>(frame.at(row, col + i).glyph & 0xff));
    }
    return text;
}

}  // namespace

int main() {
    // --- MSP v1: write a string, then draw ---------------------------------
    {
        MspOsdDecoder decoder;
        int frames = 0;
        OsdFrame captured;
        decoder.set_callback([&](const OsdFrame& frame) {
            ++frames;
            captured = frame;
        });

        const auto write = BuildV1(kMspDisplayPort, WriteString(2, 5, 0, "BATT"));
        decoder.Push(write.data(), write.size());
        CHECK_EQ(frames, 0);  // nothing is shown until DRAW_SCREEN

        const auto draw = BuildV1(kMspDisplayPort, {4});
        decoder.Push(draw.data(), draw.size());

        CHECK_EQ(frames, 1);
        CHECK_STREQ(ReadRow(captured, 2, 5, 4), "BATT");
        CHECK_EQ(decoder.checksum_errors(), static_cast<uint64_t>(0));
    }

    // --- MSP v2 framing decodes the same ----------------------------------
    {
        MspOsdDecoder decoder;
        OsdFrame captured;
        decoder.set_callback([&](const OsdFrame& frame) { captured = frame; });

        const auto write = BuildV2(kMspDisplayPort, WriteString(0, 0, 0, "HELLO"));
        const auto draw = BuildV2(kMspDisplayPort, {4});
        decoder.Push(write.data(), write.size());
        decoder.Push(draw.data(), draw.size());

        CHECK_STREQ(ReadRow(captured, 0, 0, 5), "HELLO");
    }

    // --- CLEAR wipes the canvas -------------------------------------------
    {
        MspOsdDecoder decoder;
        OsdFrame captured;
        decoder.set_callback([&](const OsdFrame& frame) { captured = frame; });

        const auto write = BuildV1(kMspDisplayPort, WriteString(1, 1, 0, "XX"));
        const auto clear = BuildV1(kMspDisplayPort, {2});
        const auto draw = BuildV1(kMspDisplayPort, {4});
        decoder.Push(write.data(), write.size());
        decoder.Push(clear.data(), clear.size());
        decoder.Push(draw.data(), draw.size());

        CHECK_EQ(static_cast<int>(captured.at(1, 1).glyph), 0);
    }

    // --- A bad checksum is counted and dropped ----------------------------
    {
        MspOsdDecoder decoder;
        int frames = 0;
        decoder.set_callback([&](const OsdFrame&) { ++frames; });

        auto draw = BuildV1(kMspDisplayPort, {4});
        draw.back() ^= 0xff;  // corrupt the checksum
        decoder.Push(draw.data(), draw.size());

        CHECK_EQ(frames, 0);
        CHECK_EQ(decoder.checksum_errors(), static_cast<uint64_t>(1));
    }

    // --- The parser resynchronises after garbage --------------------------
    {
        MspOsdDecoder decoder;
        OsdFrame captured;
        int frames = 0;
        decoder.set_callback([&](const OsdFrame& frame) {
            ++frames;
            captured = frame;
        });

        const std::vector<uint8_t> noise = {0x00, 0xff, 0x24, 0x13, 0x37, '$', '$'};
        decoder.Push(noise.data(), noise.size());

        const auto write = BuildV1(kMspDisplayPort, WriteString(3, 0, 0, "OK"));
        const auto draw = BuildV1(kMspDisplayPort, {4});
        decoder.Push(write.data(), write.size());
        decoder.Push(draw.data(), draw.size());

        CHECK_EQ(frames, 1);
        CHECK_STREQ(ReadRow(captured, 3, 0, 2), "OK");
    }

    // --- Byte-at-a-time delivery works, as it must for a serial stream -----
    {
        MspOsdDecoder decoder;
        int frames = 0;
        decoder.set_callback([&](const OsdFrame&) { ++frames; });

        const auto draw = BuildV1(kMspDisplayPort, {4});
        for (uint8_t byte : draw) {
            decoder.Push(&byte, 1);
        }
        CHECK_EQ(frames, 1);
    }

    // --- Writes outside the canvas are clipped, not crashes ---------------
    {
        MspOsdDecoder decoder;
        decoder.SetCanvas(openipc::gs::kOsdRowsHd, openipc::gs::kOsdColsHd);
        OsdFrame captured;
        decoder.set_callback([&](const OsdFrame& frame) { captured = frame; });

        // A row past the bottom is ignored entirely.
        const auto off_canvas = BuildV1(kMspDisplayPort, WriteString(200, 0, 0, "NOPE"));
        // A string running off the right edge is truncated at the boundary.
        const auto overhang = BuildV1(
            kMspDisplayPort, WriteString(0, openipc::gs::kOsdColsHd - 2, 0, "ABCDEF"));
        const auto draw = BuildV1(kMspDisplayPort, {4});
        decoder.Push(off_canvas.data(), off_canvas.size());
        decoder.Push(overhang.data(), overhang.size());
        decoder.Push(draw.data(), draw.size());

        CHECK_STREQ(ReadRow(captured, 0, openipc::gs::kOsdColsHd - 2, 2), "AB");
    }

    // --- The attribute byte selects a font page ---------------------------
    {
        MspOsdDecoder decoder;
        OsdFrame captured;
        decoder.set_callback([&](const OsdFrame& frame) { captured = frame; });

        const auto write = BuildV1(kMspDisplayPort, WriteString(0, 0, 0x01, "A"));
        const auto draw = BuildV1(kMspDisplayPort, {4});
        decoder.Push(write.data(), write.size());
        decoder.Push(draw.data(), draw.size());

        CHECK_EQ(static_cast<int>(captured.at(0, 0).glyph), 0x100 | 'A');
    }

    // --- OPTIONS resizes the canvas ---------------------------------------
    {
        MspOsdDecoder decoder;
        const auto options_sd = BuildV1(kMspDisplayPort, {5, 0, 0});
        decoder.Push(options_sd.data(), options_sd.size());
        CHECK_EQ(decoder.frame().rows, openipc::gs::kOsdRowsSd);
        CHECK_EQ(decoder.frame().cols, openipc::gs::kOsdColsSd);

        const auto options_hd = BuildV1(kMspDisplayPort, {5, 0, 1});
        decoder.Push(options_hd.data(), options_hd.size());
        CHECK_EQ(decoder.frame().rows, openipc::gs::kOsdRowsHd);
    }

    // --- Unrelated MSP commands are ignored --------------------------------
    {
        MspOsdDecoder decoder;
        int frames = 0;
        decoder.set_callback([&](const OsdFrame&) { ++frames; });
        const auto other = BuildV1(101 /* MSP_STATUS */, {1, 2, 3});
        decoder.Push(other.data(), other.size());
        CHECK_EQ(frames, 0);
        CHECK_EQ(decoder.messages(), static_cast<uint64_t>(1));
    }

    return openipc::test::Summary("msp");
}
