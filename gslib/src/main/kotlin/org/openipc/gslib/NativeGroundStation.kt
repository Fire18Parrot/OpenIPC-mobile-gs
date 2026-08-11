// SPDX-License-Identifier: GPL-3.0-only
package org.openipc.gslib

import java.nio.ByteBuffer

/**
 * The ground station: wfb-ng, devourer, adaptive-link, msposd and the MAVLink
 * router, running in this process.
 *
 * On the OpenIPC SBC ground station these are separate daemons wired together
 * with UDP sockets and shell scripts. Here one object owns the pipeline, which
 * removes the loopback hops and lets the whole thing start and stop with the
 * app's foreground service.
 */
class NativeGroundStation : AutoCloseable {

    private var handle: Long = nativeCreate()
    private var listener: GroundStationListener? = null

    @Volatile
    var isRunning: Boolean = false
        private set

    /**
     * Start receiving.
     *
     * @param usbFd file descriptor from `UsbDeviceConnection.getFileDescriptor()`.
     *   Required for [SourceKind.DEVOURER] and ignored otherwise. The connection
     *   must stay open for as long as the ground station runs.
     * @return true on success; on failure see [lastError].
     */
    fun start(
        config: GroundStationConfig,
        usbFd: Int,
        listener: GroundStationListener,
    ): Boolean {
        check(handle != 0L) { "ground station already closed" }
        this.listener = listener
        val started = nativeStart(
            handle,
            config.source.nativeValue,
            config.radio.channel,
            config.radio.bandwidth.mhz,
            config.radio.linkId,
            config.radio.keyPath,
            config.udpBindAddress,
            config.udpVideoPort,
            config.videoMirrorHost,
            config.videoMirrorPort,
            usbFd,
            Bridge(listener),
        )
        isRunning = started
        return started
    }

    fun stop() {
        if (handle != 0L) {
            nativeStop(handle)
        }
        isRunning = false
    }

    val lastError: String
        get() = if (handle != 0L) nativeLastError(handle) else ""

    /** Retune without dropping the session. */
    fun setChannel(channel: Int, bandwidth: Bandwidth): Boolean =
        handle != 0L && nativeSetChannel(handle, channel, bandwidth.mhz)

    fun setVideoCodec(codec: VideoCodec) {
        if (handle != 0L) nativeSetVideoCodec(handle, codec.nativeValue)
    }

    /** Replace the set of endpoints an external GCS can attach to. */
    fun setMavlinkEndpoints(endpoints: List<MavlinkEndpoint>) {
        if (handle == 0L) return
        nativeSetMavlinkEndpoints(
            handle,
            IntArray(endpoints.size) { endpoints[it].kind.nativeValue },
            Array(endpoints.size) { endpoints[it].host },
            IntArray(endpoints.size) { endpoints[it].port },
            BooleanArray(endpoints.size) { endpoints[it].allowUplink },
        )
    }

    fun setAlink(config: AlinkConfig) {
        if (handle == 0L) return
        nativeSetAlink(
            handle,
            config.enabled,
            config.host,
            config.port,
            config.intervalMs,
            config.snrWeight,
            config.rssiWeight,
            config.allowIdr,
            config.allowPenalty,
            config.allowFecIncrease,
        )
    }

    override fun close() {
        if (handle != 0L) {
            nativeDestroy(handle)
            handle = 0L
        }
        isRunning = false
        listener = null
    }

    /**
     * Called from native code. Unpacks the flat arrays the bridge uses - which
     * keep the JNI signatures small - into the public data classes.
     */
    private inner class Bridge(private val target: GroundStationListener) {

        fun onVideoNal(buffer: ByteBuffer, size: Int) = target.onVideoNal(buffer, size)

        fun onOsdFrame(rows: Int, cols: Int, glyphs: IntArray, attributes: IntArray) =
            target.onOsdScreen(OsdScreen(rows, cols, glyphs, attributes))

        fun onStats(ints: IntArray, floats: FloatArray) {
            val stats = LinkStats(
                bestRssi = ints[STAT_BEST_RSSI],
                bestSnr = ints[STAT_BEST_SNR],
                packetsAll = ints[STAT_PACKETS_ALL],
                packetsLost = ints[STAT_PACKETS_LOST],
                packetsRecovered = ints[STAT_PACKETS_RECOVERED],
                packetsBad = ints[STAT_PACKETS_BAD],
                packetsDecryptError = ints[STAT_PACKETS_DECRYPT_ERR],
                fecK = ints[STAT_FEC_K],
                fecN = ints[STAT_FEC_N],
                antennas = ints[STAT_ANTENNAS],
                sessionEstablished = ints[STAT_SESSION] != 0,
                bytesAll = ints[STAT_BYTES_ALL],
            )
            val telemetry = Telemetry(
                armed = ints[STAT_ARMED] != 0,
                gpsFixType = ints[STAT_GPS_FIX],
                satellites = ints[STAT_SATELLITES],
                rollRad = floats[STAT_ROLL],
                pitchRad = floats[STAT_PITCH],
                yawRad = floats[STAT_YAW],
                latitude = floats[STAT_LATITUDE],
                longitude = floats[STAT_LONGITUDE],
                altitudeM = floats[STAT_ALTITUDE],
                relativeAltitudeM = floats[STAT_RELATIVE_ALTITUDE],
                groundSpeedMs = floats[STAT_GROUND_SPEED],
                airSpeedMs = floats[STAT_AIR_SPEED],
                climbMs = floats[STAT_CLIMB],
                headingDeg = floats[STAT_HEADING],
                throttlePct = floats[STAT_THROTTLE],
                batteryVolts = floats[STAT_BATTERY_VOLTAGE],
                batteryAmps = floats[STAT_BATTERY_CURRENT],
                batteryRemainingPct = floats[STAT_BATTERY_REMAINING],
            )
            target.onStats(stats, telemetry)
        }

        fun onStatus(message: String) = target.onStatus(message)
    }

    private external fun nativeCreate(): Long
    private external fun nativeDestroy(handle: Long)
    private external fun nativeStart(
        handle: Long,
        sourceKind: Int,
        channel: Int,
        bandwidth: Int,
        linkId: Int,
        keyPath: String,
        udpBind: String,
        udpPort: Int,
        mirrorHost: String,
        mirrorPort: Int,
        usbFd: Int,
        listener: Any,
    ): Boolean

    private external fun nativeStop(handle: Long)
    private external fun nativeLastError(handle: Long): String
    private external fun nativeSetChannel(handle: Long, channel: Int, bandwidth: Int): Boolean
    private external fun nativeSetVideoCodec(handle: Long, codec: Int)
    private external fun nativeSetMavlinkEndpoints(
        handle: Long,
        kinds: IntArray,
        hosts: Array<String>,
        ports: IntArray,
        uplinkFlags: BooleanArray,
    )

    private external fun nativeSetAlink(
        handle: Long,
        enabled: Boolean,
        host: String,
        port: Int,
        intervalMs: Int,
        snrWeight: Float,
        rssiWeight: Float,
        allowIdr: Boolean,
        allowPenalty: Boolean,
        allowFecIncrease: Boolean,
    )

    companion object {
        init {
            System.loadLibrary("openipc_gs")
        }

        // Indices into the flat stats arrays. Must match the enums in
        // jni_bridge.cpp.
        private const val STAT_BEST_RSSI = 0
        private const val STAT_BEST_SNR = 1
        private const val STAT_PACKETS_ALL = 2
        private const val STAT_PACKETS_LOST = 3
        private const val STAT_PACKETS_RECOVERED = 4
        private const val STAT_PACKETS_BAD = 5
        private const val STAT_PACKETS_DECRYPT_ERR = 6
        private const val STAT_FEC_K = 7
        private const val STAT_FEC_N = 8
        private const val STAT_ANTENNAS = 9
        private const val STAT_SESSION = 10
        private const val STAT_BYTES_ALL = 11
        private const val STAT_ARMED = 12
        private const val STAT_GPS_FIX = 13
        private const val STAT_SATELLITES = 14

        private const val STAT_ROLL = 0
        private const val STAT_PITCH = 1
        private const val STAT_YAW = 2
        private const val STAT_LATITUDE = 3
        private const val STAT_LONGITUDE = 4
        private const val STAT_ALTITUDE = 5
        private const val STAT_RELATIVE_ALTITUDE = 6
        private const val STAT_GROUND_SPEED = 7
        private const val STAT_AIR_SPEED = 8
        private const val STAT_CLIMB = 9
        private const val STAT_HEADING = 10
        private const val STAT_THROTTLE = 11
        private const val STAT_BATTERY_VOLTAGE = 12
        private const val STAT_BATTERY_CURRENT = 13
        private const val STAT_BATTERY_REMAINING = 14

        /** Key handling lives on [NativeKeys]; kept here for convenience. */
        fun generateKeyPair(gsPath: String, dronePath: String): Boolean =
            NativeKeys.generateKeyPair(gsPath, dronePath)

        fun validateKey(path: String): String? = NativeKeys.validateKey(path)
    }
}
