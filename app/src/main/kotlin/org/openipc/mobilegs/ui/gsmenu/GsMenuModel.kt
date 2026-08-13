// SPDX-License-Identifier: GPL-3.0-only
package org.openipc.mobilegs.ui.gsmenu

/**
 * The SBC ground station's in-goggle menu, ported.
 *
 * This is a direct transcription of `gsmenu.sh` from the sbc-groundstations
 * pixelpilot package - the menu PixelPilot_rk draws over the video on a
 * goggles-attached SBC. Section names, item names, ordering and value lists all
 * follow upstream, so a pilot who knows the SBC menu finds the same things in
 * the same places here.
 *
 * Upstream addresses every item as `<get|set> <target> <section> <item>`, and
 * that path is kept verbatim as [GsMenuItem.path] - it is the identity of the
 * setting, and what a future SSH bridge to the air unit will send.
 */

/** How a value is chosen, mirroring what gsmenu.sh emits for each item. */
sealed interface GsMenuValue {
    /** `emit_values "a\nb\nc"` - a fixed list. */
    data class Choice(val options: List<String>) : GsMenuValue

    /** `emit_values "1 100"` - an inclusive numeric range. */
    data class Range(val min: Int, val max: Int, val step: Int = 1) : GsMenuValue

    /** Free text, e.g. an SSID or a host. */
    data object Text : GsMenuValue

    /** A number typed rather than picked: ports, link ids. */
    data class Number(val min: Int = 0, val max: Int = Int.MAX_VALUE) : GsMenuValue

    /** A toggle. Upstream spells these as two-value lists. */
    data object Toggle : GsMenuValue

    /** Something that happens rather than something that is set. */
    data object Action : GsMenuValue

    /** Options discovered at runtime, e.g. the channel or network list. */
    data class Dynamic(val source: String) : GsMenuValue
}

data class GsMenuItem(
    /** e.g. "gs wfbng gs_channel", exactly as gsmenu.sh addresses it. */
    val path: String,
    val label: String,
    val value: GsMenuValue,
    /** Shown under the item; upstream relies on the wiki, we can afford a line. */
    val help: String = "",
    /**
     * False for anything that needs hardware or a link the app does not have
     * yet. Shown but not editable, so the menu stays a faithful map of the SBC
     * rather than quietly dropping entries.
     */
    val supported: Boolean = true,
)

data class GsMenuSection(val name: String, val title: String, val items: List<GsMenuItem>)

data class GsMenuTarget(val name: String, val title: String, val sections: List<GsMenuSection>)

/**
 * The menu tree. `air` is addressed over SSH to the camera by upstream; the
 * items are listed so the shape matches, and become live once the app grows
 * that bridge.
 */
object GsMenu {

    val targets: List<GsMenuTarget> = listOf(
        GsMenuTarget(
            name = "gs",
            title = "GS",
            sections = listOf(
                GsMenuSection(
                    name = "wfbng",
                    title = "WFB-NG",
                    items = listOf(
                        GsMenuItem(
                            "gs wfbng gs_channel", "Channel",
                            GsMenuValue.Dynamic("wifi_channels"),
                            "Must match the air unit's channel.",
                        ),
                        GsMenuItem(
                            "gs wfbng bandwidth", "Bandwidth",
                            GsMenuValue.Choice(listOf("20", "40")),
                        ),
                        GsMenuItem(
                            "gs wfbng txpower", "TX power",
                            GsMenuValue.Range(1, 100),
                            "Only used for the uplink.",
                        ),
                        GsMenuItem(
                            "gs wfbng adaptivelink", "Adaptive link",
                            GsMenuValue.Toggle,
                            "Feeds link quality back to the air unit.",
                        ),
                    ),
                ),
                GsMenuSection(
                    name = "system",
                    title = "System",
                    items = listOf(
                        GsMenuItem(
                            "gs system rx_mode", "RX mode",
                            GsMenuValue.Choice(listOf("wfb", "apfpv")),
                            "wfb needs an adapter; apfpv joins the air unit's own network.",
                        ),
                        GsMenuItem(
                            "gs system rx_codec", "Codec",
                            GsMenuValue.Choice(listOf("h264", "h265", "auto")),
                        ),
                        GsMenuItem(
                            "gs system resolution", "Resolution",
                            GsMenuValue.Dynamic("display_modes"),
                            "Set by the phone's display.",
                            supported = false,
                        ),
                        GsMenuItem(
                            "gs system connector", "Connector",
                            GsMenuValue.Choice(listOf("HDMI")),
                            "SBC-only; the phone renders to its own screen.",
                            supported = false,
                        ),
                        GsMenuItem(
                            "gs system video_scale", "Video scale",
                            GsMenuValue.Range(50, 100),
                        ),
                        GsMenuItem(
                            "gs system gs_rendering", "Rendering",
                            GsMenuValue.Toggle,
                            supported = false,
                        ),
                        GsMenuItem(
                            "gs system gs_live_colortrans", "Live colour transform",
                            GsMenuValue.Toggle,
                            supported = false,
                        ),
                        GsMenuItem("gs system rec_enabled", "Record on start", GsMenuValue.Toggle),
                        GsMenuItem(
                            "gs system dvr_mode", "DVR mode",
                            GsMenuValue.Choice(listOf("raw", "reencode", "both")),
                            "Only raw is implemented; the stream has no reliable timestamps.",
                        ),
                        GsMenuItem("gs system dvr_osd", "DVR OSD", GsMenuValue.Toggle, supported = false),
                        GsMenuItem(
                            "gs system dvr_max_size", "DVR max size (GB)",
                            GsMenuValue.Range(1, 40),
                        ),
                        GsMenuItem(
                            "gs system dvr_reenc_codec", "DVR re-encode codec",
                            GsMenuValue.Choice(listOf("h264", "h265")), supported = false,
                        ),
                        GsMenuItem(
                            "gs system dvr_reenc_resolution", "DVR re-encode resolution",
                            GsMenuValue.Choice(listOf("720p", "1080p")), supported = false,
                        ),
                        GsMenuItem(
                            "gs system dvr_reenc_fps", "DVR re-encode FPS",
                            GsMenuValue.Choice(listOf("30", "60")), supported = false,
                        ),
                        GsMenuItem(
                            "gs system dvr_reenc_bitrate", "DVR re-encode bitrate",
                            GsMenuValue.Choice(
                                (5000..50000 step 5000).map { it.toString() },
                            ),
                            supported = false,
                        ),
                        GsMenuItem("gs system audio", "Audio", GsMenuValue.Toggle, supported = false),
                        GsMenuItem(
                            "gs system audio_device", "Audio device",
                            GsMenuValue.Dynamic("audio_devices"), supported = false,
                        ),
                        GsMenuItem(
                            "gs system audio_volume", "Audio volume",
                            GsMenuValue.Range(0, 100), supported = false,
                        ),
                    ),
                ),
                GsMenuSection(
                    name = "apfpv",
                    title = "APFPV",
                    items = listOf(
                        GsMenuItem("gs apfpv status", "Status", GsMenuValue.Action),
                        GsMenuItem("gs apfpv ssid", "SSID", GsMenuValue.Text),
                        GsMenuItem("gs apfpv password", "Password", GsMenuValue.Text),
                        GsMenuItem(
                            "gs apfpv wlx", "Adapter",
                            GsMenuValue.Dynamic("wlan_interfaces"),
                            "The phone's own Wi-Fi is used instead.",
                            supported = false,
                        ),
                        GsMenuItem("gs apfpv reset", "Reset", GsMenuValue.Action),
                    ),
                ),
                GsMenuSection(
                    name = "app",
                    title = "App",
                    items = listOf(
                        GsMenuItem(
                            "gs app link_id", "Link ID",
                            GsMenuValue.Number(0, 16_777_215),
                            "Must match the air unit's wfb.yaml.",
                        ),
                        GsMenuItem(
                            "gs app udp_port", "APFPV video port",
                            GsMenuValue.Number(1, 65_535),
                            "Where the air unit's own network sends RTP.",
                        ),
                        GsMenuItem(
                            "gs app osd", "Show air unit OSD",
                            GsMenuValue.Toggle,
                            "The camera's own msposd overlay.",
                        ),
                        GsMenuItem(
                            "gs app top_osd", "Link metrics box",
                            GsMenuValue.Toggle,
                            "The osd.json panel at the top right.",
                        ),
                        GsMenuItem(
                            "gs app no_signal", "No-signal background",
                            GsMenuValue.Choice(listOf("drift", "black", "grey")),
                            "Drift moves a soft gradient so an OLED cannot stain.",
                        ),

                        // The corner strip. Goggles put their own numbers in the
                        // bottom right and let the pilot choose which; these are
                        // ours, and each is off-able for the same reason.
                        GsMenuItem(
                            "gs app strip_flight_time", "Strip: flight time",
                            GsMenuValue.Toggle,
                            "Time since the link came up.",
                        ),
                        GsMenuItem(
                            "gs app strip_craft_battery", "Strip: craft battery",
                            GsMenuValue.Toggle,
                            "Cell gauge, remaining percent and pack voltage.",
                        ),
                        GsMenuItem(
                            "gs app strip_signal", "Strip: signal",
                            GsMenuValue.Toggle,
                            "Five bars banded as the metrics box bands them.",
                        ),
                        GsMenuItem(
                            "gs app strip_link_bar", "Strip: link quality",
                            GsMenuValue.Toggle,
                            "Clean, FEC-recovered and lost packets as one bar.",
                        ),
                        GsMenuItem(
                            "gs app strip_bitrate", "Strip: bitrate",
                            GsMenuValue.Toggle,
                            "With a trace of the last few seconds.",
                        ),
                        GsMenuItem(
                            "gs app strip_altitude", "Strip: altitude",
                            GsMenuValue.Toggle,
                            "Height above the launch point, from MAVLink.",
                        ),
                        GsMenuItem(
                            "gs app strip_speed", "Strip: ground speed",
                            GsMenuValue.Toggle,
                        ),
                        GsMenuItem("gs app strip_fps", "Strip: frame rate", GsMenuValue.Toggle),
                        GsMenuItem(
                            "gs app strip_phone_battery", "Strip: phone battery",
                            GsMenuValue.Toggle,
                        ),
                        GsMenuItem(
                            "gs app strip_recording", "Strip: recording light",
                            GsMenuValue.Toggle,
                        ),

                        GsMenuItem(
                            "gs app mavlink_enabled", "MAVLink out",
                            GsMenuValue.Toggle,
                            "Share telemetry with a ground control station.",
                        ),
                        GsMenuItem(
                            "gs app mavlink_kind", "MAVLink transport",
                            GsMenuValue.Choice(listOf("udp_out", "udp_server", "tcp_server")),
                        ),
                        GsMenuItem("gs app mavlink_host", "MAVLink host", GsMenuValue.Text),
                        GsMenuItem(
                            "gs app mavlink_port", "MAVLink port",
                            GsMenuValue.Number(1, 65_535),
                        ),
                        GsMenuItem(
                            "gs app mavlink_uplink", "Allow uplink from GCS",
                            GsMenuValue.Toggle,
                            "Lets a GCS command the aircraft, not only watch it.",
                        ),
                        GsMenuItem(
                            "gs app second_endpoint", "Second endpoint",
                            GsMenuValue.Toggle,
                            "A second GCS can attach without displacing the first.",
                        ),
                        GsMenuItem(
                            "gs app second_kind", "Second transport",
                            GsMenuValue.Choice(listOf("udp_out", "udp_server", "tcp_server")),
                        ),
                        GsMenuItem(
                            "gs app second_port", "Second port",
                            GsMenuValue.Number(1, 65_535),
                        ),
                        GsMenuItem("gs app alink_host", "Adaptive link host", GsMenuValue.Text),
                        GsMenuItem(
                            "gs app alink_port", "Adaptive link port",
                            GsMenuValue.Number(1, 65_535),
                        ),
                        GsMenuItem(
                            "gs app alink_idr", "Request keyframes on loss",
                            GsMenuValue.Toggle,
                        ),
                        GsMenuItem("gs app alink_penalty", "Apply noise penalty", GsMenuValue.Toggle),
                        GsMenuItem("gs app alink_fec", "Allow FEC increase", GsMenuValue.Toggle),
                    ),
                ),
                GsMenuSection(
                    name = "wifi",
                    title = "WiFi",
                    items = listOf(
                        GsMenuItem(
                            "gs wifi wlan", "Interface",
                            GsMenuValue.Dynamic("wlan_interfaces"), supported = false,
                        ),
                        GsMenuItem("gs wifi ssid", "SSID", GsMenuValue.Text, supported = false),
                        GsMenuItem(
                            "gs wifi password", "Password", GsMenuValue.Text, supported = false,
                        ),
                        GsMenuItem(
                            "gs wifi networks", "Networks",
                            GsMenuValue.Dynamic("wifi_networks"),
                            "Use Android's own Wi-Fi settings.",
                            supported = false,
                        ),
                        GsMenuItem(
                            "gs wifi savednetworks", "Saved networks",
                            GsMenuValue.Dynamic("wifi_saved"), supported = false,
                        ),
                        GsMenuItem(
                            "gs wifi hotspot", "Hotspot", GsMenuValue.Toggle, supported = false,
                        ),
                    ),
                ),
            ),
        ),
        GsMenuTarget(
            name = "air",
            title = "Air",
            sections = listOf(
                GsMenuSection(
                    name = "wfbng",
                    title = "WFB-NG",
                    items = listOf(
                        GsMenuItem(
                            "air wfbng power", "TX power",
                            GsMenuValue.Choice(
                                listOf("1", "20", "25", "30", "35", "40", "45", "50", "55", "58"),
                            ),
                            "Needs an SSH connection to the air unit.",
                            supported = false,
                        ),
                        GsMenuItem(
                            "air wfbng air_channel", "Channel",
                            GsMenuValue.Dynamic("wifi_channels"),
                            "Needs an SSH connection to the air unit.",
                            supported = false,
                        ),
                    ),
                ),
            ),
        ),
    )

    fun section(target: String, section: String): GsMenuSection? =
        targets.firstOrNull { it.name == target }?.sections?.firstOrNull { it.name == section }

    /** Look an item up by its upstream path, which is its identity. */
    fun item(path: String): GsMenuItem? =
        targets.asSequence()
            .flatMap { it.sections.asSequence() }
            .flatMap { it.items.asSequence() }
            .firstOrNull { it.path == path }
}
