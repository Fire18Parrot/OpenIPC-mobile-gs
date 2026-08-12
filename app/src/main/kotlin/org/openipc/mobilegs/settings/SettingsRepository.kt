// SPDX-License-Identifier: GPL-3.0-only
package org.openipc.mobilegs.settings

import android.content.Context
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import org.openipc.gslib.AlinkConfig
import org.openipc.gslib.Bandwidth
import org.openipc.gslib.GroundStationConfig
import org.openipc.gslib.MavlinkEndpoint
import org.openipc.gslib.MavlinkEndpointKind
import org.openipc.gslib.RadioConfig
import org.openipc.gslib.SourceKind
import org.openipc.gslib.VideoCodec

private val Context.dataStore by preferencesDataStore(name = "ground_station")

/**
 * Everything the SBC ground station keeps in wifibroadcast.cfg, alink_gs.conf
 * and its web UI on port 5000, held as app settings instead.
 */
data class Settings(
    val source: SourceKind = SourceKind.DEVOURER,
    val channel: Int = 161,
    val bandwidth: Bandwidth = Bandwidth.BW_20,
    val linkId: Int = 7669206,
    val codec: VideoCodec = VideoCodec.AUTO,
    val udpVideoPort: Int = 5600,

    val mavlinkEnabled: Boolean = true,
    val mavlinkKind: MavlinkEndpointKind = MavlinkEndpointKind.UDP_OUT,
    val mavlinkHost: String = "127.0.0.1",
    val mavlinkPort: Int = 14550,
    val mavlinkUplink: Boolean = true,

    /** A second endpoint, so a GCS can attach without displacing the first. */
    val secondEndpointEnabled: Boolean = false,
    val secondEndpointKind: MavlinkEndpointKind = MavlinkEndpointKind.TCP_SERVER,
    val secondEndpointPort: Int = 5760,

    val alinkEnabled: Boolean = false,
    val alinkHost: String = "10.5.0.10",
    val alinkPort: Int = 9999,
    val alinkAllowIdr: Boolean = true,
    val alinkAllowPenalty: Boolean = false,
    val alinkAllowFecIncrease: Boolean = false,

    val recordVideo: Boolean = false,
    val osdEnabled: Boolean = true,

    // The corner strip, bottom right, where the goggles put their numbers.
    // Each element is opt-in: what a pilot wants to see differs. Signal and
    // frame rate default off because the ported osd.json metrics box already
    // draws both, with bars, in the opposite corner.
    val stripBitrate: Boolean = true,
    val stripSignal: Boolean = false,
    val stripFps: Boolean = false,
    val stripCraftBattery: Boolean = true,
    val stripPhoneBattery: Boolean = true,
    val stripFlightTime: Boolean = true,

    /**
     * What to show where there is no picture. OLED phones burn in, and a
     * ground station spends a lot of its life parked on a static screen.
     */
    val noSignalStyle: String = "drift",
) {
    fun toGroundStationConfig(keyPath: String) = GroundStationConfig(
        source = source,
        radio = RadioConfig(
            channel = channel,
            bandwidth = bandwidth,
            linkId = linkId,
            keyPath = keyPath,
        ),
        udpVideoPort = udpVideoPort,
    )

    fun toAlinkConfig() = AlinkConfig(
        enabled = alinkEnabled,
        host = alinkHost,
        port = alinkPort,
        allowIdr = alinkAllowIdr,
        allowPenalty = alinkAllowPenalty,
        allowFecIncrease = alinkAllowFecIncrease,
    )

    fun toMavlinkEndpoints(): List<MavlinkEndpoint> = buildList {
        if (mavlinkEnabled) {
            add(MavlinkEndpoint(mavlinkKind, mavlinkHost, mavlinkPort, mavlinkUplink))
        }
        if (secondEndpointEnabled) {
            add(MavlinkEndpoint(secondEndpointKind, "0.0.0.0", secondEndpointPort, true))
        }
    }
}

class SettingsRepository(private val context: Context) {

    val settings: Flow<Settings> = context.dataStore.data.map { prefs ->
        Settings(
            source = SourceKind.entries.getOrElse(prefs[KEY_SOURCE] ?: 0) { SourceKind.DEVOURER },
            channel = prefs[KEY_CHANNEL] ?: 161,
            bandwidth = Bandwidth.entries.firstOrNull { it.mhz == prefs[KEY_BANDWIDTH] }
                ?: Bandwidth.BW_20,
            linkId = prefs[KEY_LINK_ID] ?: 7669206,
            codec = VideoCodec.entries.getOrElse(prefs[KEY_CODEC] ?: 0) { VideoCodec.AUTO },
            udpVideoPort = prefs[KEY_UDP_PORT] ?: 5600,
            mavlinkEnabled = prefs[KEY_MAVLINK_ENABLED] ?: true,
            mavlinkKind = MavlinkEndpointKind.entries
                .getOrElse(prefs[KEY_MAVLINK_KIND] ?: 0) { MavlinkEndpointKind.UDP_OUT },
            mavlinkHost = prefs[KEY_MAVLINK_HOST] ?: "127.0.0.1",
            mavlinkPort = prefs[KEY_MAVLINK_PORT] ?: 14550,
            mavlinkUplink = prefs[KEY_MAVLINK_UPLINK] ?: true,
            secondEndpointEnabled = prefs[KEY_SECOND_ENABLED] ?: false,
            secondEndpointKind = MavlinkEndpointKind.entries
                .getOrElse(prefs[KEY_SECOND_KIND] ?: 2) { MavlinkEndpointKind.TCP_SERVER },
            secondEndpointPort = prefs[KEY_SECOND_PORT] ?: 5760,
            alinkEnabled = prefs[KEY_ALINK_ENABLED] ?: false,
            alinkHost = prefs[KEY_ALINK_HOST] ?: "10.5.0.10",
            alinkPort = prefs[KEY_ALINK_PORT] ?: 9999,
            alinkAllowIdr = prefs[KEY_ALINK_IDR] ?: true,
            alinkAllowPenalty = prefs[KEY_ALINK_PENALTY] ?: false,
            alinkAllowFecIncrease = prefs[KEY_ALINK_FEC] ?: false,
            recordVideo = prefs[KEY_RECORD] ?: false,
            osdEnabled = prefs[KEY_OSD] ?: true,
            stripBitrate = prefs[KEY_STRIP_BITRATE] ?: true,
            stripSignal = prefs[KEY_STRIP_SIGNAL] ?: false,
            stripFps = prefs[KEY_STRIP_FPS] ?: false,
            stripCraftBattery = prefs[KEY_STRIP_CRAFT_BATTERY] ?: true,
            stripPhoneBattery = prefs[KEY_STRIP_PHONE_BATTERY] ?: true,
            stripFlightTime = prefs[KEY_STRIP_FLIGHT_TIME] ?: true,
            noSignalStyle = prefs[KEY_NO_SIGNAL] ?: "drift",
        )
    }

    suspend fun update(transform: (Settings) -> Settings) {
        val next = transform(settings.first())
        context.dataStore.edit { prefs ->
            prefs[KEY_SOURCE] = next.source.ordinal
            prefs[KEY_CHANNEL] = next.channel
            prefs[KEY_BANDWIDTH] = next.bandwidth.mhz
            prefs[KEY_LINK_ID] = next.linkId
            prefs[KEY_CODEC] = next.codec.ordinal
            prefs[KEY_UDP_PORT] = next.udpVideoPort
            prefs[KEY_MAVLINK_ENABLED] = next.mavlinkEnabled
            prefs[KEY_MAVLINK_KIND] = next.mavlinkKind.ordinal
            prefs[KEY_MAVLINK_HOST] = next.mavlinkHost
            prefs[KEY_MAVLINK_PORT] = next.mavlinkPort
            prefs[KEY_MAVLINK_UPLINK] = next.mavlinkUplink
            prefs[KEY_SECOND_ENABLED] = next.secondEndpointEnabled
            prefs[KEY_SECOND_KIND] = next.secondEndpointKind.ordinal
            prefs[KEY_SECOND_PORT] = next.secondEndpointPort
            prefs[KEY_ALINK_ENABLED] = next.alinkEnabled
            prefs[KEY_ALINK_HOST] = next.alinkHost
            prefs[KEY_ALINK_PORT] = next.alinkPort
            prefs[KEY_ALINK_IDR] = next.alinkAllowIdr
            prefs[KEY_ALINK_PENALTY] = next.alinkAllowPenalty
            prefs[KEY_ALINK_FEC] = next.alinkAllowFecIncrease
            prefs[KEY_RECORD] = next.recordVideo
            prefs[KEY_OSD] = next.osdEnabled
            prefs[KEY_STRIP_BITRATE] = next.stripBitrate
            prefs[KEY_STRIP_SIGNAL] = next.stripSignal
            prefs[KEY_STRIP_FPS] = next.stripFps
            prefs[KEY_STRIP_CRAFT_BATTERY] = next.stripCraftBattery
            prefs[KEY_STRIP_PHONE_BATTERY] = next.stripPhoneBattery
            prefs[KEY_STRIP_FLIGHT_TIME] = next.stripFlightTime
            prefs[KEY_NO_SIGNAL] = next.noSignalStyle
        }
    }

    private companion object {
        val KEY_SOURCE = intPreferencesKey("source")
        val KEY_CHANNEL = intPreferencesKey("channel")
        val KEY_BANDWIDTH = intPreferencesKey("bandwidth")
        val KEY_LINK_ID = intPreferencesKey("link_id")
        val KEY_CODEC = intPreferencesKey("codec")
        val KEY_UDP_PORT = intPreferencesKey("udp_port")
        val KEY_MAVLINK_ENABLED = booleanPreferencesKey("mavlink_enabled")
        val KEY_MAVLINK_KIND = intPreferencesKey("mavlink_kind")
        val KEY_MAVLINK_HOST = stringPreferencesKey("mavlink_host")
        val KEY_MAVLINK_PORT = intPreferencesKey("mavlink_port")
        val KEY_MAVLINK_UPLINK = booleanPreferencesKey("mavlink_uplink")
        val KEY_SECOND_ENABLED = booleanPreferencesKey("second_enabled")
        val KEY_SECOND_KIND = intPreferencesKey("second_kind")
        val KEY_SECOND_PORT = intPreferencesKey("second_port")
        val KEY_ALINK_ENABLED = booleanPreferencesKey("alink_enabled")
        val KEY_ALINK_HOST = stringPreferencesKey("alink_host")
        val KEY_ALINK_PORT = intPreferencesKey("alink_port")
        val KEY_ALINK_IDR = booleanPreferencesKey("alink_idr")
        val KEY_ALINK_PENALTY = booleanPreferencesKey("alink_penalty")
        val KEY_ALINK_FEC = booleanPreferencesKey("alink_fec")
        val KEY_RECORD = booleanPreferencesKey("record")
        val KEY_OSD = booleanPreferencesKey("osd")
        val KEY_STRIP_BITRATE = booleanPreferencesKey("strip_bitrate")
        val KEY_STRIP_SIGNAL = booleanPreferencesKey("strip_signal")
        val KEY_STRIP_FPS = booleanPreferencesKey("strip_fps")
        val KEY_STRIP_CRAFT_BATTERY = booleanPreferencesKey("strip_craft_battery")
        val KEY_STRIP_PHONE_BATTERY = booleanPreferencesKey("strip_phone_battery")
        val KEY_STRIP_FLIGHT_TIME = booleanPreferencesKey("strip_flight_time")
        val KEY_NO_SIGNAL = stringPreferencesKey("no_signal_style")
    }
}
