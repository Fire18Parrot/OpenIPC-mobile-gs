# OpenIPC Mobile GS

An Android ground station for OpenIPC digital FPV: the component set of the
[OpenIPC SBC ground station](https://github.com/OpenIPC/sbc-groundstations)
ported into a single app, so a phone plus a ~$15 RTL8812AU adapter *is* the
ground station — no single-board computer, no SD card to flash, no root, and no
second device to open a web UI on.

It is the same idea as [PixelPilot](https://github.com/OpenIPC/PixelPilot), taken
further: the SBC's adaptive-link, msposd and settings UI come along too, and the
telemetry stream is exposed on real endpoints so an external ground control
station can attach.

## What replaced what

| SBC ground-station package | In the app |
| --- | --- |
| `rtl8812au`, `rtl88x2eu`, `rtl88x2cu` kernel modules | [devourer](https://github.com/OpenIPC/devourer) in userspace over libusb — no root, no custom kernel |
| `wifibroadcast-ng` (`wfb_rx`, `wfb_tx`, `gs.sh`) | wfb-ng's own `Aggregator`, compiled in and subclassed so payload arrives by callback |
| `pixelpilot` (Rockchip MPP + DRM player) | `MediaCodec` low-latency decode onto a `SurfaceView` |
| `msposd` | MSP DisplayPort decoder plus a Compose font-atlas renderer |
| `adaptive-link` (`alink_gs`) | Ported to C++, byte-compatible with `alink_drone` |
| `wfb-server`, `dvrui` (web UI on `:5000`) | Compose settings and DVR screens, on the device itself |
| `yaml-cli`, `yq` (`wifibroadcast.cfg`, `gs.key`) | DataStore settings, plus `gs.key` import and key-pair generation |

## How it fits together

```
USB ──▶ devourer ──▶ 802.11 frames ──▶ wfb-ng Aggregator ──┬─▶ port 0x00  video    ─▶ RTP ─▶ MediaCodec ─▶ Surface
        (libusb)      (+ RSSI/SNR)     (decrypt, FEC,      │                              └─▶ DVR (.h264/.h265)
                                        reassembly)        ├─▶ port 0x10  telemetry ─▶ MAVLink router ─▶ UDP/TCP endpoints
                                                           │                        └─▶ MSP OSD ─▶ overlay
                                                           └─▶ port 0x20  tunnel    ─▶ MSP OSD

                        adaptive-link ◀── link stats            uplink ─▶ port 0xa0 (alink), 0x90 (MAVLink from a GCS)
```

Radio ports follow wfb-ng's `master.cfg`, so the app is interchangeable with an
SBC ground station on the same air unit.

Everything runs in one process. On an SBC these are separate daemons wired
together with UDP sockets; here the aggregator hands buffers straight to the
next stage, which removes the loopback hops.

## MAVLink for an external GCS

Telemetry is fanned out to as many endpoints as you like, each independently
configured, so the built-in OSD, QGroundControl and a third-party ground control
station can all be attached at once:

- **UDP out** — send to a fixed `host:port` (default `127.0.0.1:14550`).
- **UDP server** — bind a port and learn the peer from its first packet.
- **TCP server** — listen for a GCS that prefers TCP (default `5760`).

Endpoints are bidirectional by default: MAVLink a GCS sends back is routed onto
the uplink radio port, so it can command the aircraft rather than only watch it.

## APFPV

Set the source to `UDP` and the app skips the radio layer entirely, reading RTP
from the air unit's own access point. No adapter and no keys — shorter range
than wfb-ng, but it works with hardware you already have.

## Building

```bash
git clone --recurse-submodules https://github.com/Fire18Parrot/OpenIPC-mobile-gs
cd OpenIPC-mobile-gs
./gradlew assembleDebug
```

You need the Android SDK with NDK `27.0.12077973` and CMake 3.22.1. Everything
else — devourer, wfb-ng, libusb, libsodium — is built from the submodules in
`third_party/`, so there are no prebuilt binaries in the tree.

If you already cloned without `--recurse-submodules`:

```bash
git submodule update --init --recursive
```

## Testing

The protocol code is deliberately free of Android dependencies so it can be
tested on a desktop with no phone and no radio:

```bash
sudo apt-get install -y cmake ninja-build libsodium-dev libpcap-dev
cmake -S core -B build -G Ninja -DOPENIPC_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The interop suite is the important one. It compiles wfb-ng's own `Transmitter`
and drives our receiver with it, so packets pass through upstream's session-key
exchange, its ChaCha20-Poly1305 encryption and its Reed-Solomon encoder before
our code sees them. It covers a clean round trip, FEC recovery with four
fragments dropped from a `k=8/n=12` block, rejection of a foreign `link_id`,
stream demultiplexing, and — the common field failure — failing closed when
`gs.key` does not match the air unit.

## Keys

`gs.key` uses wfb-ng's layout: 32 bytes of ground-station secret key followed by
32 bytes of the air unit's public key. Copy the one from your existing ground
station, or generate a fresh pair in the app and copy the resulting `drone.key`
to the air unit. Key files are gitignored; do not commit them.

## OSD fonts

Drop any msposd font atlas (`font_hd.png`, `font_btfl_hd.png`,
`font_inav_hd.png`, `font_ardu_hd.png`) into `app/src/main/assets/`. The fonts
from the [msposd](https://github.com/OpenIPC/msposd) repository work unmodified.
Without one the OSD still renders occupied cells, so telemetry is visibly
arriving.

## Status

Test-verified here: the portable engine — wfb receive path, adaptive-link
scoring and wire format, RTP depacketising, MSP decoding, MAVLink framing and
endpoint routing. 213 checks across 5 suites.

Compile-verified by CI only: the Android layer — JNI, the devourer and libusb
integration, Compose UI, MediaCodec and the DVR. On-air behaviour needs real
hardware; reports from a field test are welcome.

## Licence

GPL-3.0, matching wfb-ng, devourer and the rest of the OpenIPC stack it is
built from.
