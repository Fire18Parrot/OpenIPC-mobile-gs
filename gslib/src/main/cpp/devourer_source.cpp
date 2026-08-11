// SPDX-License-Identifier: GPL-3.0-only
//
// The devourer-backed packet source: an RTL8812AU-class adapter on the phone's
// USB port, driven entirely in userspace.

#include <android/log.h>
#include <libusb.h>

#include <atomic>
#include <memory>
#include <thread>

#include "devourer/src/IRtlDevice.h"
#include "devourer/src/RxPacket.h"
#include "devourer/src/SelectedChannel.h"
#include "devourer/src/UsbOpen.h"
#include "devourer/src/UsbDeviceLock.h"
#include "devourer/src/WiFiDriver.h"
#include "devourer/src/logger.h"
#include "packet_source.h"

namespace openipc::gs {
namespace {

constexpr const char* kTag = "openipc-gs";

ChannelWidth_t ToChannelWidth(Bandwidth bandwidth) {
    switch (bandwidth) {
        case Bandwidth::k5:
            return CHANNEL_WIDTH_5;
        case Bandwidth::k10:
            return CHANNEL_WIDTH_10;
        case Bandwidth::k40:
            return CHANNEL_WIDTH_40;
        case Bandwidth::k80:
            return CHANNEL_WIDTH_80;
        case Bandwidth::k20:
        default:
            return CHANNEL_WIDTH_20;
    }
}

SelectedChannel MakeChannel(int channel, Bandwidth bandwidth) {
    SelectedChannel selected;
    selected.Channel = static_cast<uint8_t>(channel);
    selected.ChannelOffset = 0;
    selected.ChannelWidth = ToChannelWidth(bandwidth);
    return selected;
}

class DevourerSource : public PacketSource {
public:
    DevourerSource(const RadioConfig& radio, int usb_fd)
        : radio_(radio), usb_fd_(usb_fd), logger_(std::make_shared<Logger>()) {}

    ~DevourerSource() override { Stop(); }

    bool Start(std::string* error) override {
        // Android never lets an app open a USB device itself: the user grants
        // permission, the framework opens it, and we adopt the descriptor.
        // LIBUSB_OPTION_NO_DEVICE_DISCOVERY tells libusb not to try scanning
        // /dev/bus/usb, which is not readable by an unprivileged app.
        if (libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY) != LIBUSB_SUCCESS) {
            *error = "libusb refused NO_DEVICE_DISCOVERY";
            return false;
        }
        int rc = libusb_init(&context_);
        if (rc != LIBUSB_SUCCESS) {
            *error = std::string("libusb_init failed: ") + libusb_error_name(rc);
            return false;
        }

        rc = libusb_wrap_sys_device(context_, static_cast<intptr_t>(usb_fd_), &handle_);
        if (rc != LIBUSB_SUCCESS || handle_ == nullptr) {
            *error = std::string("could not adopt the USB descriptor: ") + libusb_error_name(rc);
            Cleanup();
            return false;
        }

        const int interface = devourer::find_wifi_interface(handle_);
        // Resetting the device would invalidate the descriptor the framework
        // gave us, so the reset step is skipped here - unlike on a desktop,
        // where devourer owns the handle end to end.
        rc = devourer::claim_interface_then_reset(handle_, interface, logger_, /*do_reset=*/false,
                                                  usb_lock_);
        if (rc != 0) {
            *error = rc == LIBUSB_ERROR_BUSY
                         ? "the adapter is already in use by another app"
                         : std::string("could not claim the adapter: ") + libusb_error_name(rc);
            Cleanup();
            return false;
        }

        driver_ = std::make_unique<WiFiDriver>(logger_);
        device_ = driver_->CreateRtlDevice(handle_, context_, usb_lock_);
        if (!device_) {
            *error = "unsupported Wi-Fi chip - devourer did not recognise it";
            Cleanup();
            return false;
        }

        channel_ = MakeChannel(radio_.channel, radio_.bandwidth);

        try {
            // InitWrite brings the chip up for both directions, so the same
            // claimed handle can carry the adaptive-link uplink while the RX
            // loop runs. Init() alone would be receive-only.
            device_->InitWrite(channel_);
        } catch (const std::exception& e) {
            *error = std::string("adapter bring-up failed: ") + e.what();
            Cleanup();
            return false;
        }

        running_ = true;
        rx_thread_ = std::thread([this] { RxLoop(); });
        return true;
    }

    void Stop() override {
        if (!running_.exchange(false)) {
            Cleanup();
            return;
        }
        if (device_) {
            device_->StopRxLoop();
        }
        if (rx_thread_.joinable()) {
            rx_thread_.join();
        }
        Cleanup();
    }

    bool SetChannel(int channel, Bandwidth bandwidth) override {
        if (!device_) {
            return false;
        }
        channel_ = MakeChannel(channel, bandwidth);
        try {
            device_->SetMonitorChannel(channel_);
        } catch (const std::exception& e) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "channel change failed: %s", e.what());
            return false;
        }
        return true;
    }

    bool Transmit(const uint8_t* frame, size_t size) override {
        if (!device_) {
            return false;
        }
        return device_->send_packet(frame, size);
    }

private:
    void RxLoop() {
        try {
            device_->StartRxLoop([this](const Packet& packet) {
                if (!on_frame_ || packet.Data.empty()) {
                    return;
                }
                FrameMeta meta;
                // devourer reports RSSI as an unsigned value in dBm magnitude;
                // wfb-ng and the OSD both want it signed and negative.
                meta.paths = 2;
                for (int i = 0; i < 2; ++i) {
                    const int rssi = static_cast<int>(packet.RxAtrib.rssi[i]);
                    meta.rssi[i] = static_cast<int8_t>(rssi > 127 ? -128 : -rssi);
                    meta.snr[i] = packet.RxAtrib.snr[i];
                }
                meta.mcs_index = static_cast<uint8_t>(packet.RxAtrib.data_rate);
                meta.bandwidth = packet.RxAtrib.bw;
                on_frame_(packet.Data.data(), packet.Data.size(), meta);
            });
        } catch (const std::exception& e) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "rx loop stopped: %s", e.what());
        }
    }

    void Cleanup() {
        device_.reset();
        driver_.reset();
        usb_lock_.reset();
        if (handle_ != nullptr) {
            libusb_close(handle_);
            handle_ = nullptr;
        }
        if (context_ != nullptr) {
            libusb_exit(context_);
            context_ = nullptr;
        }
    }

    RadioConfig radio_;
    int usb_fd_;
    libusb_context* context_ = nullptr;
    libusb_device_handle* handle_ = nullptr;
    std::shared_ptr<devourer::UsbDeviceLock> usb_lock_;
    std::unique_ptr<WiFiDriver> driver_;
    std::unique_ptr<IRtlDevice> device_;
    SelectedChannel channel_{};
    Logger_t logger_;
    std::thread rx_thread_;
    std::atomic<bool> running_{false};
};

}  // namespace

std::unique_ptr<PacketSource> MakeDevourerSource(const RadioConfig& radio, int usb_fd) {
    return std::make_unique<DevourerSource>(radio, usb_fd);
}

}  // namespace openipc::gs
