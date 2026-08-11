// SPDX-License-Identifier: GPL-3.0-only

#include "openipc/msp_osd.h"

#include <algorithm>
#include <cstring>

namespace openipc::gs {
namespace {

constexpr uint16_t kMspDisplayPort = 182;

// CRC-8/DVB-S2, used by MSP v2.
uint8_t Crc8DvbS2(uint8_t crc, uint8_t byte) {
    crc ^= byte;
    for (int i = 0; i < 8; ++i) {
        crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0xd5) : static_cast<uint8_t>(crc << 1);
    }
    return crc;
}

}  // namespace

MspOsdDecoder::MspOsdDecoder() {
    payload_.reserve(512);
    SetCanvas(kOsdRowsHd, kOsdColsHd);
}

void MspOsdDecoder::SetCanvas(int rows, int cols) {
    if (rows <= 0 || cols <= 0) {
        return;
    }
    if (frame_.rows == rows && frame_.cols == cols) {
        return;
    }
    frame_.rows = rows;
    frame_.cols = cols;
    frame_.cells.assign(static_cast<size_t>(rows) * cols, OsdCell{});
}

void MspOsdDecoder::Clear() {
    std::fill(frame_.cells.begin(), frame_.cells.end(), OsdCell{});
}

void MspOsdDecoder::Push(const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        HandleByte(data[i]);
    }
}

void MspOsdDecoder::HandleByte(uint8_t byte) {
    switch (state_) {
        case State::kIdle:
            if (byte == '$') {
                state_ = State::kHeaderM;
            }
            break;

        case State::kHeaderM:
            if (byte == 'M') {
                v2_ = false;
                state_ = State::kHeaderArrow;
            } else if (byte == 'X') {
                v2_ = true;
                state_ = State::kHeaderArrow;
            } else if (byte == '$') {
                // Stay put: a second '$' restarts the header.
            } else {
                state_ = State::kIdle;
            }
            break;

        case State::kHeaderArrow:
            // Direction byte: '<' to the flight controller, '>' from it, '!' on
            // error. We only care about frames coming from the aircraft but
            // accept any so a bidirectional capture still decodes.
            if (byte == '<' || byte == '>' || byte == '!') {
                payload_.clear();
                checksum_ = 0;
                state_ = v2_ ? State::kV2Flags : State::kV1Size;
            } else {
                state_ = State::kIdle;
            }
            break;

        // --- MSP v1: size, command, payload, XOR checksum ---
        case State::kV1Size:
            expected_size_ = byte;
            checksum_ = byte;
            state_ = State::kV1Command;
            break;

        case State::kV1Command:
            function_ = byte;
            checksum_ ^= byte;
            state_ = expected_size_ > 0 ? State::kV1Payload : State::kV1Checksum;
            break;

        case State::kV1Payload:
            payload_.push_back(byte);
            checksum_ ^= byte;
            if (payload_.size() >= expected_size_) {
                state_ = State::kV1Checksum;
            }
            break;

        case State::kV1Checksum:
            if (checksum_ == byte) {
                DispatchMessage();
            } else {
                ++checksum_errors_;
            }
            state_ = State::kIdle;
            break;

        // --- MSP v2: flags, function (LE16), size (LE16), payload, CRC8 ---
        case State::kV2Flags:
            checksum_ = Crc8DvbS2(0, byte);
            state_ = State::kV2FunctionLo;
            break;

        case State::kV2FunctionLo:
            function_ = byte;
            checksum_ = Crc8DvbS2(checksum_, byte);
            state_ = State::kV2FunctionHi;
            break;

        case State::kV2FunctionHi:
            function_ |= static_cast<uint16_t>(byte) << 8;
            checksum_ = Crc8DvbS2(checksum_, byte);
            state_ = State::kV2SizeLo;
            break;

        case State::kV2SizeLo:
            expected_size_ = byte;
            checksum_ = Crc8DvbS2(checksum_, byte);
            state_ = State::kV2SizeHi;
            break;

        case State::kV2SizeHi:
            expected_size_ |= static_cast<uint16_t>(byte) << 8;
            checksum_ = Crc8DvbS2(checksum_, byte);
            if (expected_size_ > 1024) {
                // Implausible for DisplayPort; treat as desync.
                state_ = State::kIdle;
            } else {
                state_ = expected_size_ > 0 ? State::kV2Payload : State::kV2Checksum;
            }
            break;

        case State::kV2Payload:
            payload_.push_back(byte);
            checksum_ = Crc8DvbS2(checksum_, byte);
            if (payload_.size() >= expected_size_) {
                state_ = State::kV2Checksum;
            }
            break;

        case State::kV2Checksum:
            if (checksum_ == byte) {
                DispatchMessage();
            } else {
                ++checksum_errors_;
            }
            state_ = State::kIdle;
            break;
    }
}

void MspOsdDecoder::DispatchMessage() {
    ++messages_;
    if (function_ == kMspDisplayPort) {
        HandleDisplayPort(payload_.data(), payload_.size());
    }
}

void MspOsdDecoder::HandleDisplayPort(const uint8_t* payload, size_t size) {
    if (size < 1) {
        return;
    }
    const auto cmd = static_cast<DisplayPortCmd>(payload[0]);

    switch (cmd) {
        case DisplayPortCmd::kClear:
            Clear();
            break;

        case DisplayPortCmd::kWriteString: {
            // row, col, attribute, then the characters.
            if (size < 4) {
                return;
            }
            const int row = payload[1];
            const int col = payload[2];
            const uint8_t attribute = payload[3];
            if (row < 0 || row >= frame_.rows || col < 0 || col >= frame_.cols) {
                return;
            }
            // msposd selects a font page through the low bits of the attribute
            // byte, which is how it addresses glyphs past 255.
            const uint16_t page = static_cast<uint16_t>(attribute & 0x03) << 8;
            for (size_t i = 4; i < size; ++i) {
                const int target_col = col + static_cast<int>(i - 4);
                if (target_col >= frame_.cols) {
                    break;
                }
                if (payload[i] == 0) {
                    break;  // NUL terminates the run
                }
                OsdCell& cell = frame_.at(row, target_col);
                cell.glyph = static_cast<uint16_t>(payload[i]) | page;
                cell.attributes = attribute;
            }
            break;
        }

        case DisplayPortCmd::kDrawScreen:
            ++frames_;
            if (callback_) {
                callback_(frame_);
            }
            break;

        case DisplayPortCmd::kOptions: {
            // payload: [cmd][font type][canvas rows/cols variant]. msposd sends
            // the variant so the ground station can size its canvas.
            if (size >= 3) {
                const uint8_t variant = payload[2];
                if (variant == 0) {
                    SetCanvas(kOsdRowsSd, kOsdColsSd);
                } else {
                    SetCanvas(kOsdRowsHd, kOsdColsHd);
                }
            }
            break;
        }

        case DisplayPortCmd::kRelease:
            Clear();
            if (callback_) {
                callback_(frame_);
            }
            break;

        case DisplayPortCmd::kHeartbeat:
        case DisplayPortCmd::kSys:
        default:
            break;
    }
}

}  // namespace openipc::gs
