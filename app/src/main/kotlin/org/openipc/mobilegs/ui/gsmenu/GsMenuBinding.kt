// SPDX-License-Identifier: GPL-3.0-only
package org.openipc.mobilegs.ui.gsmenu

import org.openipc.gslib.Bandwidth
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
