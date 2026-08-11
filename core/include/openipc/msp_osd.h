// SPDX-License-Identifier: GPL-3.0-only
//
// MSP DisplayPort OSD decoding - the receiving half of msposd.
//
// On the SBC ground station, msposd reads the MSP stream coming off the air
// unit and blits characters from a font PNG onto a DRM plane. The app does the
// same job in two halves: this decoder maintains the character grid, and the
// Android layer draws it from a font atlas in assets/.
//
// The wire format is Betaflight/INAV MSP (v1 and v2) carrying the
// MSP_DISPLAYPORT (182) command.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace openipc::gs {

// MSP DisplayPort subcommands.
enum class DisplayPortCmd : uint8_t {
    kHeartbeat = 0,
    kRelease = 1,
    kClear = 2,
    kWriteString = 3,
    kDrawScreen = 4,
    kOptions = 5,
    kSys = 6,
};

// One cell of the OSD character grid.
struct OsdCell {
    // Character index into the font atlas. Wider than a byte because msposd
    // font pages address more than 256 glyphs.
    uint16_t glyph = 0;
    uint8_t attributes = 0;
};

// A complete OSD frame, published on every DRAW_SCREEN.
struct OsdFrame {
    int rows = 0;
    int cols = 0;
    std::vector<OsdCell> cells;  // row-major, rows * cols entries

    const OsdCell& at(int row, int col) const { return cells[static_cast<size_t>(row) * cols + col]; }
    OsdCell& at(int row, int col) { return cells[static_cast<size_t>(row) * cols + col]; }
};

using OsdFrameCallback = std::function<void(const OsdFrame&)>;

// Canvas sizes msposd uses. The air unit announces its choice through the
// OPTIONS subcommand; until then we assume HD, which is what OpenIPC ships.
inline constexpr int kOsdRowsSd = 16;
inline constexpr int kOsdColsSd = 30;
inline constexpr int kOsdRowsHd = 20;
inline constexpr int kOsdColsHd = 53;

class MspOsdDecoder {
public:
    MspOsdDecoder();

    void set_callback(OsdFrameCallback callback) { callback_ = std::move(callback); }

    // Feed bytes off the telemetry stream. The parser is resynchronising, so
    // partial and corrupt input is safe to hand over.
    void Push(const uint8_t* data, size_t size);

    // Resize the canvas. Called on OPTIONS, or by the UI when the user forces a
    // layout.
    void SetCanvas(int rows, int cols);

    const OsdFrame& frame() const { return frame_; }

    uint64_t frames() const { return frames_; }
    uint64_t messages() const { return messages_; }
    uint64_t checksum_errors() const { return checksum_errors_; }

private:
    void HandleByte(uint8_t byte);
    void DispatchMessage();
    void HandleDisplayPort(const uint8_t* payload, size_t size);
    void Clear();

    enum class State {
        kIdle,
        kHeaderM,      // saw '$'
        kHeaderArrow,  // saw '$M' or '$X'
        kV1Size,
        kV1Command,
        kV1Payload,
        kV1Checksum,
        kV2Flags,
        kV2FunctionLo,
        kV2FunctionHi,
        kV2SizeLo,
        kV2SizeHi,
        kV2Payload,
        kV2Checksum,
    };

    State state_ = State::kIdle;
    bool v2_ = false;
    uint16_t function_ = 0;
    uint16_t expected_size_ = 0;
    uint8_t checksum_ = 0;
    std::vector<uint8_t> payload_;

    OsdFrame frame_;
    OsdFrameCallback callback_;

    uint64_t frames_ = 0;
    uint64_t messages_ = 0;
    uint64_t checksum_errors_ = 0;
};

}  // namespace openipc::gs
