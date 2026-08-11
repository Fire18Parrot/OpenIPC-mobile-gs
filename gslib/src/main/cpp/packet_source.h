// SPDX-License-Identifier: GPL-3.0-only
//
// Where frames come from.
//
// The SBC ground station has exactly one answer to this - a kernel driver
// presenting a monitor-mode interface - but a phone has two worth supporting:
// an RTL8812AU-class adapter driven by devourer over USB, and APFPV, where the
// air unit is an ordinary access point and the video arrives as RTP on a UDP
// socket with no adapter at all.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "openipc/config.h"
#include "openipc/wfb_receiver.h"

namespace openipc::gs {

class PacketSource {
public:
    // A raw 802.11 frame including its FCS, with receive metadata.
    using FrameCallback = std::function<void(const uint8_t*, size_t, const FrameMeta&)>;
    // A complete UDP payload, for sources that do not involve wfb at all.
    using DatagramCallback = std::function<void(const uint8_t*, size_t)>;

    virtual ~PacketSource() = default;

    virtual bool Start(std::string* error) = 0;
    virtual void Stop() = 0;

    // Only meaningful for radio sources.
    virtual bool SetChannel(int channel, Bandwidth bandwidth) {
        (void)channel;
        (void)bandwidth;
        return false;
    }

    // Inject a frame upward to the air unit. Radio sources transmit; a UDP
    // source has no uplink and returns false.
    virtual bool Transmit(const uint8_t* frame, size_t size) {
        (void)frame;
        (void)size;
        return false;
    }

    void set_frame_callback(FrameCallback callback) { on_frame_ = std::move(callback); }
    void set_datagram_callback(DatagramCallback callback) { on_datagram_ = std::move(callback); }

protected:
    FrameCallback on_frame_;
    DatagramCallback on_datagram_;
};

// RTL8812AU / 8811AU / 8821AU / 8812EU / 8822EU over libusb, no root and no
// kernel module. Android hands us an already-open file descriptor because only
// the system may open a USB device; libusb is told not to enumerate and to
// adopt that descriptor instead.
std::unique_ptr<PacketSource> MakeDevourerSource(const RadioConfig& radio, int usb_fd);

// APFPV, or any air unit streaming RTP to a UDP port.
std::unique_ptr<PacketSource> MakeUdpSource(const UdpSourceConfig& config);

}  // namespace openipc::gs
