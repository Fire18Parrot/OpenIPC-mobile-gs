// SPDX-License-Identifier: GPL-3.0-only
package org.openipc.mobilegs.ui.gsmenu

import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.BarChart
import androidx.compose.material.icons.outlined.MoreHoriz
import androidx.compose.material.icons.outlined.PhotoLibrary
import androidx.compose.material.icons.outlined.Settings
import androidx.compose.material.icons.outlined.SwapHoriz
import androidx.compose.ui.graphics.vector.ImageVector

/**
 * How the ported menu is presented in the goggles layout.
 *
 * The tree in [GsMenu] is upstream's and does not move. This is a second view
 * of it, grouped the way a goggles menu groups things: a short icon rail rather
 * than a target row and a section column. The grouping turned out to be almost
 * free, because the five rail entries line up with sections we already have -
 * Transmission is `gs wfbng`, Settings is `gs system`, and the rest is either
 * live state or an action.
 *
 * Rows are addressed by upstream path, so this file says nothing about what a
 * setting *is*; change the tree and this follows.
 */
data class GsRail(
    val id: String,
    /** Label under the icon. */
    val title: String,
    /** Heading over the content panel; upstream's own word where it has one. */
    val panelTitle: String,
    val icon: ImageVector,
    /** Items drawn from the ported tree, in the order they should appear. */
    val paths: List<String> = emptyList(),
)

object GsRails {

    const val STATUS = "status"
    const val ALBUM = "album"
    const val TRANSMISSION = "transmission"
    const val SETTINGS = "settings"
    const val MORE = "more"

    val entries: List<GsRail> = listOf(
        // Live link state. Nothing here is a setting, so the panel is built
        // from the current stats rather than from the tree.
        GsRail(STATUS, "Status", "Status", Icons.Outlined.BarChart),

        // The DVR, which the SBC exposes through its separate web UI.
        GsRail(ALBUM, "Album", "Album", Icons.Outlined.PhotoLibrary),

        GsRail(
            TRANSMISSION, "Transmission", "Transmission", Icons.Outlined.SwapHoriz,
            listOf(
                "gs wfbng gs_channel",
                "gs wfbng bandwidth",
                "gs wfbng txpower",
                "gs wfbng adaptivelink",
                "gs system rx_mode",
                "air wfbng power",
                "air wfbng air_channel",
            ),
        ),

        // "Camera" is what the goggles call this panel, and it is where the
        // video and recording settings live.
        GsRail(
            SETTINGS, "Settings", "Camera", Icons.Outlined.Settings,
            listOf(
                "gs system rx_codec",
                "gs system video_scale",
                "gs system rec_enabled",
                "gs system dvr_mode",
                "gs system dvr_max_size",
                "gs system dvr_osd",
                "gs system dvr_reenc_codec",
                "gs system dvr_reenc_resolution",
                "gs system dvr_reenc_fps",
                "gs system dvr_reenc_bitrate",
                "gs system connector",
                "gs system resolution",
                "gs system gs_rendering",
                "gs system gs_live_colortrans",
                "gs system audio",
                "gs system audio_device",
                "gs system audio_volume",
                // The corner strip and the screen behind the video. These have
                // no gsmenu.sh equivalent - an SBC drives a fixed goggles panel
                // and cannot burn it in the way a phone's OLED burns - but they
                // are display settings, so this is where they belong.
                "gs app osd",
                "gs app no_signal",
                "gs app strip_flight_time",
                "gs app strip_craft_battery",
                "gs app strip_signal",
                "gs app strip_bitrate",
                "gs app strip_fps",
                "gs app strip_phone_battery",
            ),
        ),

        // Everything that is neither a live reading nor a video setting: the
        // remaining ported sections plus the two things a phone needs and an
        // SBC does not.
        GsRail(
            MORE, "More", "More", Icons.Outlined.MoreHoriz,
            listOf(
                "gs apfpv status",
                "gs apfpv ssid",
                "gs apfpv password",
                "gs apfpv wlx",
                "gs wifi wlan",
                "gs wifi ssid",
                "gs wifi networks",
                "gs wifi hotspot",
                // App-only, with no gsmenu.sh equivalent: the SBC keeps these
                // in wifibroadcast.cfg and alink_gs.conf, which a phone has no
                // way to edit.
                "gs app link_id",
                "gs app udp_port",
                "gs app mavlink_enabled",
                "gs app mavlink_kind",
                "gs app mavlink_host",
                "gs app mavlink_port",
                "gs app mavlink_uplink",
                "gs app second_endpoint",
                "gs app second_kind",
                "gs app second_port",
                "gs app alink_host",
                "gs app alink_port",
                "gs app alink_idr",
                "gs app alink_penalty",
                "gs app alink_fec",
            ),
        ),
    )

    fun byId(id: String): GsRail = entries.firstOrNull { it.id == id } ?: entries.first()
}
