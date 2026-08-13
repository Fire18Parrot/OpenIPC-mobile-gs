// SPDX-License-Identifier: GPL-3.0-only
package org.openipc.mobilegs.ui.gsmenu

import org.openipc.gslib.Bandwidth
import org.openipc.gslib.MavlinkEndpointKind
import org.openipc.gslib.SourceKind
import org.openipc.gslib.VideoCodec
import org.openipc.mobilegs.settings.Settings

/**
 * Connects the ported menu paths to the app's stored settings.
 *
 * The menu tree is upstream's and must not bend to fit our storage, so the
 * binding is a lookup table rather than a field on each item. A path with no
 * binding reads as null and is shown but not editable - which is how the menu
 * stays an honest map of the SBC even where the phone cannot follow.
 */
object GsMenuBinding {

    /** The current value of [path], or null when nothing backs it here. */
    fun read(path: String, settings: Settings): String? = when (path) {
        "gs wfbng gs_channel" -> settings.channel.toString()
        "gs wfbng bandwidth" -> settings.bandwidth.mhz.toString()
        "gs wfbng adaptivelink" -> settings.alinkEnabled.onOff()
        "gs system rx_mode" -> if (settings.source == SourceKind.UDP) "apfpv" else "wfb"
        "gs system rx_codec" -> when (settings.codec) {
            VideoCodec.H264 -> "h264"
            VideoCodec.H265 -> "h265"
            VideoCodec.AUTO -> "auto"
        }
        "gs system rec_enabled" -> settings.recordVideo.onOff()
        // Only raw capture exists: the stream carries no reliable timestamps
        // and has gaps wherever FEC could not recover, so re-encoding it is
        // not something the phone can honestly offer yet.
        "gs system dvr_mode" -> "raw"
        "gs apfpv status" -> if (settings.source == SourceKind.UDP) "active" else "inactive"

        "gs app link_id" -> settings.linkId.toString()
        "gs app udp_port" -> settings.udpVideoPort.toString()
        "gs app osd" -> settings.osdEnabled.onOff()
        "gs app top_osd" -> settings.topOsdEnabled.onOff()
        "gs app video_fit" -> settings.videoFit
        "gs app no_signal" -> settings.noSignalStyle
        "gs app strip_flight_time" -> settings.stripFlightTime.onOff()
        "gs app strip_craft_battery" -> settings.stripCraftBattery.onOff()
        "gs app strip_signal" -> settings.stripSignal.onOff()
        "gs app strip_link_bar" -> settings.stripLinkBar.onOff()
        "gs app strip_bitrate" -> settings.stripBitrate.onOff()
        "gs app strip_altitude" -> settings.stripAltitude.onOff()
        "gs app strip_speed" -> settings.stripSpeed.onOff()
        "gs app strip_fps" -> settings.stripFps.onOff()
        "gs app strip_phone_battery" -> settings.stripPhoneBattery.onOff()
        "gs app strip_recording" -> settings.stripRecording.onOff()
        "gs app mavlink_enabled" -> settings.mavlinkEnabled.onOff()
        "gs app mavlink_kind" -> settings.mavlinkKind.wire()
        "gs app mavlink_host" -> settings.mavlinkHost
        "gs app mavlink_port" -> settings.mavlinkPort.toString()
        "gs app mavlink_uplink" -> settings.mavlinkUplink.onOff()
        "gs app second_endpoint" -> settings.secondEndpointEnabled.onOff()
        "gs app second_kind" -> settings.secondEndpointKind.wire()
        "gs app second_port" -> settings.secondEndpointPort.toString()
        "gs app alink_host" -> settings.alinkHost
        "gs app alink_port" -> settings.alinkPort.toString()
        "gs app alink_idr" -> settings.alinkAllowIdr.onOff()
        "gs app alink_penalty" -> settings.alinkAllowPenalty.onOff()
        "gs app alink_fec" -> settings.alinkAllowFecIncrease.onOff()

        else -> null
    }

    /** Apply [value] to [settings], or null when the path is not editable here. */
    fun write(path: String, value: String, settings: Settings): Settings? = when (path) {
        "gs wfbng gs_channel" ->
            value.toIntOrNull()?.let { settings.copy(channel = it) }

        "gs wfbng bandwidth" ->
            Bandwidth.entries.firstOrNull { it.mhz.toString() == value }
                ?.let { settings.copy(bandwidth = it) }

        "gs wfbng adaptivelink" -> settings.copy(alinkEnabled = value.isOn())

        "gs system rx_mode" -> settings.copy(
            source = if (value == "apfpv") SourceKind.UDP else SourceKind.DEVOURER,
        )

        "gs system rx_codec" -> settings.copy(
            codec = when (value) {
                "h264" -> VideoCodec.H264
                "h265" -> VideoCodec.H265
                else -> VideoCodec.AUTO
            },
        )

        "gs system rec_enabled" -> settings.copy(recordVideo = value.isOn())

        "gs app link_id" -> value.toIntOrNull()?.let { settings.copy(linkId = it) }
        "gs app udp_port" -> value.toIntOrNull()?.let { settings.copy(udpVideoPort = it) }
        "gs app osd" -> settings.copy(osdEnabled = value.isOn())
        "gs app top_osd" -> settings.copy(topOsdEnabled = value.isOn())
        "gs app video_fit" -> settings.copy(videoFit = value)
        "gs app no_signal" -> settings.copy(noSignalStyle = value)
        "gs app strip_flight_time" -> settings.copy(stripFlightTime = value.isOn())
        "gs app strip_craft_battery" -> settings.copy(stripCraftBattery = value.isOn())
        "gs app strip_signal" -> settings.copy(stripSignal = value.isOn())
        "gs app strip_link_bar" -> settings.copy(stripLinkBar = value.isOn())
        "gs app strip_bitrate" -> settings.copy(stripBitrate = value.isOn())
        "gs app strip_altitude" -> settings.copy(stripAltitude = value.isOn())
        "gs app strip_speed" -> settings.copy(stripSpeed = value.isOn())
        "gs app strip_fps" -> settings.copy(stripFps = value.isOn())
        "gs app strip_phone_battery" -> settings.copy(stripPhoneBattery = value.isOn())
        "gs app strip_recording" -> settings.copy(stripRecording = value.isOn())
        "gs app mavlink_enabled" -> settings.copy(mavlinkEnabled = value.isOn())
        "gs app mavlink_kind" -> kindOf(value)?.let { settings.copy(mavlinkKind = it) }
        "gs app mavlink_host" -> settings.copy(mavlinkHost = value)
        "gs app mavlink_port" -> value.toIntOrNull()?.let { settings.copy(mavlinkPort = it) }
        "gs app mavlink_uplink" -> settings.copy(mavlinkUplink = value.isOn())
        "gs app second_endpoint" -> settings.copy(secondEndpointEnabled = value.isOn())
        "gs app second_kind" -> kindOf(value)?.let { settings.copy(secondEndpointKind = it) }
        "gs app second_port" -> value.toIntOrNull()?.let { settings.copy(secondEndpointPort = it) }
        "gs app alink_host" -> settings.copy(alinkHost = value)
        "gs app alink_port" -> value.toIntOrNull()?.let { settings.copy(alinkPort = it) }
        "gs app alink_idr" -> settings.copy(alinkAllowIdr = value.isOn())
        "gs app alink_penalty" -> settings.copy(alinkAllowPenalty = value.isOn())
        "gs app alink_fec" -> settings.copy(alinkAllowFecIncrease = value.isOn())

        else -> null
    }

    /** Endpoint kinds are shown by their wire name, not the enum's. */
    private fun MavlinkEndpointKind.wire(): String = when (this) {
        MavlinkEndpointKind.UDP_OUT -> "udp_out"
        MavlinkEndpointKind.UDP_SERVER -> "udp_server"
        MavlinkEndpointKind.TCP_SERVER -> "tcp_server"
    }

    private fun kindOf(value: String): MavlinkEndpointKind? = when (value) {
        "udp_out" -> MavlinkEndpointKind.UDP_OUT
        "udp_server" -> MavlinkEndpointKind.UDP_SERVER
        "tcp_server" -> MavlinkEndpointKind.TCP_SERVER
        else -> null
    }

    fun isEditable(item: GsMenuItem, settings: Settings): Boolean =
        item.supported && write(item.path, read(item.path, settings) ?: "", settings) != null

    /**
     * The values the user can cycle through. Dynamic lists are resolved here
     * because only the app knows what the phone can actually do.
     */
    fun options(item: GsMenuItem): List<String> = when (val value = item.value) {
        is GsMenuValue.Choice -> value.options
        is GsMenuValue.Range -> buildList {
            var current = value.min
            while (current <= value.max) {
                add(current.toString())
                current += value.step
            }
        }
        GsMenuValue.Toggle -> listOf("off", "on")
        is GsMenuValue.Number -> emptyList()
        is GsMenuValue.Dynamic -> when (value.source) {
            // 2.4 GHz plus the 5 GHz channels OpenIPC air units actually use.
            "wifi_channels" -> WIFI_CHANNELS
            else -> emptyList()
        }
        else -> emptyList()
    }

    private val WIFI_CHANNELS: List<String> =
        ((1..14) + listOf(
            36, 40, 44, 48, 52, 56, 60, 64,
            100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
            149, 153, 157, 161, 165,
        )).map { it.toString() }

    private fun Boolean.onOff() = if (this) "on" else "off"

    private fun String.isOn() = this == "on" || this == "true" || this == "1"
}
