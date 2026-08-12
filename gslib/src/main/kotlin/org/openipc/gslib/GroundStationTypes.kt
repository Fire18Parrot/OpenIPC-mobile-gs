// SPDX-License-Identifier: GPL-3.0-only
package org.openipc.gslib

import java.nio.ByteBuffer

/** Where the ground station gets its packets. */
enum class SourceKind(val nativeValue: Int) {
    /** An RTL8812AU-class adapter on USB, running the full wfb-ng stack. */
    DEVOURER(0),

    /** APFPV: RTP over UDP from an air unit acting as an access point. */
    UDP(1),
}

enum class Bandwidth(val mhz: Int) {
    BW_5(5),
    BW_10(10),
    BW_20(20),
    BW_40(40),
    BW_80(80),
}

enum class VideoCodec(val nativeValue: Int) {
    /** Detected from the first packet. */
    AUTO(0),
    H264(1),
    H265(2),
}

enum class MavlinkEndpointKind(val nativeValue: Int) {
    /** Send to a fixed host:port, QGroundControl's usual arrangement. */
    UDP_OUT(0),

    /** Bind a port and learn the peer from its first packet. */
    UDP_SERVER(1),

    /** Listen for ground control stations that prefer TCP. */
    TCP_SERVER(2),
}

/**
 * One place an external ground control station can attach to the telemetry
 * stream. Several may be active at once, so the built-in OSD, QGroundControl
 * and a third-party GCS can all see the aircraft simultaneously.
 */
data class MavlinkEndpoint(
    val kind: MavlinkEndpointKind = MavlinkEndpointKind.UDP_OUT,
    val host: String = "127.0.0.1",
    val port: Int = 14550,
    /** Whether MAVLink arriving here is forwarded up to the aircraft. */
    val allowUplink: Boolean = true,
)

/** Radio settings, mirroring the SBC ground station's wifibroadcast.cfg. */
data class RadioConfig(
    val channel: Int = 161,
    val bandwidth: Bandwidth = Bandwidth.BW_20,
    val linkId: Int = 7669206,
    /** Absolute path to gs.key. */
    val keyPath: String = "",
)

/** Adaptive-link tuning, mirroring alink_gs.conf. */
data class AlinkConfig(
    val enabled: Boolean = false,
    val host: String = "10.5.0.10",
    val port: Int = 9999,
    val intervalMs: Int = 100,
    val snrWeight: Float = 0.5f,
    val rssiWeight: Float = 0.5f,
    val allowIdr: Boolean = true,
    val allowPenalty: Boolean = false,
    val allowFecIncrease: Boolean = false,
)

data class GroundStationConfig(
    val source: SourceKind = SourceKind.DEVOURER,
    val radio: RadioConfig = RadioConfig(),
    val udpBindAddress: String = "0.0.0.0",
    val udpVideoPort: Int = 5600,
    /**
     * Optional mirror of the decoded video to a local UDP port, matching the
     * SBC's `gs_video` peer of 127.0.0.1:5600 so other tools keep working.
     */
    val videoMirrorHost: String = "",
    val videoMirrorPort: Int = 0,
)

/** A snapshot of link quality, published once per stats interval. */
data class LinkStats(
    val bestRssi: Int = -105,
    val bestSnr: Int = 0,
    val packetsAll: Int = 0,
    val packetsLost: Int = 0,
    val packetsRecovered: Int = 0,
    val packetsBad: Int = 0,
    val packetsDecryptError: Int = 0,
    val fecK: Int = -1,
    val fecN: Int = -1,
    val antennas: Int = 0,
    val sessionEstablished: Boolean = false,
    val bytesAll: Int = 0,
    /** The codec actually seen on the wire, which may differ from the setting. */
    val detectedCodec: VideoCodec = VideoCodec.AUTO,
) {
    /** True when packets are arriving and decrypting - i.e. the link is up. */
    val isLive: Boolean get() = sessionEstablished && packetsAll > 0
}

/** Flight telemetry decoded from MAVLink, for the OSD. */
data class Telemetry(
    val armed: Boolean = false,
    val gpsFixType: Int = 0,
    val satellites: Int = 0,
    val rollRad: Float = 0f,
    val pitchRad: Float = 0f,
    val yawRad: Float = 0f,
    val latitude: Float = 0f,
    val longitude: Float = 0f,
    val altitudeM: Float = 0f,
    val relativeAltitudeM: Float = 0f,
    val groundSpeedMs: Float = 0f,
    val airSpeedMs: Float = 0f,
    val climbMs: Float = 0f,
    val headingDeg: Float = 0f,
    val throttlePct: Float = 0f,
    val batteryVolts: Float = 0f,
    val batteryAmps: Float = 0f,
    val batteryRemainingPct: Float = -1f,
)

/** One decoded OSD screen from the air unit's msposd. */
data class OsdScreen(
    val rows: Int = 0,
    val cols: Int = 0,
    /** Row-major glyph indices into the font atlas. */
    val glyphs: IntArray = IntArray(0),
    val attributes: IntArray = IntArray(0),
) {
    fun glyphAt(row: Int, col: Int): Int = glyphs[row * cols + col]

    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is OsdScreen) return false
        return rows == other.rows && cols == other.cols &&
            glyphs.contentEquals(other.glyphs) && attributes.contentEquals(other.attributes)
    }

    override fun hashCode(): Int {
        var result = rows
        result = 31 * result + cols
        result = 31 * result + glyphs.contentHashCode()
        result = 31 * result + attributes.contentHashCode()
        return result
    }
}

/**
 * Callbacks from the native pipeline. All of them arrive on native worker
 * threads, never the main thread.
 */
interface GroundStationListener {
    /**
     * One Annex-B NAL unit. The buffer is a direct view of native memory and is
     * only valid until this call returns - copy it into a codec input buffer
     * rather than retaining it.
     */
    fun onVideoNal(buffer: ByteBuffer, size: Int)

    fun onOsdScreen(screen: OsdScreen)

    fun onStats(stats: LinkStats, telemetry: Telemetry)

    fun onStatus(message: String)
}
