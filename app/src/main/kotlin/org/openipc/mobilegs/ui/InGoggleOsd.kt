// SPDX-License-Identifier: GPL-3.0-only
package org.openipc.mobilegs.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.material3.Text
import org.openipc.gslib.LinkStats

/**
 * The in-goggle metrics panel, ported from `osd.json` in the sbc-groundstations
 * pixelpilot package.
 *
 * Upstream is a list of positioned widgets drawn by PixelPilot_rk: a
 * translucent box at the top right, per-antenna signal icons, RF temperature,
 * video format, throughput, codec and DVR state. The widget set and the
 * thresholds are upstream's; only the drawing is ours, because the phone has no
 * access to the PNG assets the SBC ships.
 */

/**
 * Signal strength as upstream bands it. From osd.json's IconSelectorWidget for
 * `wfbcli.rx.ant_stats.rssi_avg`: signal1 is the strongest bar, and each band
 * is eight dB wide below -40.
 */
private val RSSI_BANDS = listOf(
    -40 to 6, // signal1, best
    -48 to 5,
    -56 to 4,
    -64 to 3,
    -72 to 2,
    -80 to 1,
)

private const val MAX_BARS = 6

internal fun barsForRssi(rssi: Int): Int {
    for ((floor, bars) in RSSI_BANDS) {
        if (rssi >= floor) return bars
    }
    return 0
}

@Composable
fun InGoggleOsd(
    stats: LinkStats,
    codec: String,
    fps: Int,
    recording: Boolean,
    modifier: Modifier = Modifier,
    /**
     * False in APFPV, where there is no wfb layer to report on. Every radio
     * row below is fed from the wfb receiver's snapshot, and APFPV never
     * creates one - so drawing them there would sit a permanent NO SIGNAL and
     * a -105 dBm over video that is arriving perfectly well.
     */
    radioLink: Boolean = true,
) {
    // "Metrics background": upstream is a 270x130 box at the top right, black
    // at 40% alpha.
    Box(
        modifier
            .width(270.dp)
            .height(130.dp),
    ) {
        Canvas(Modifier.size(270.dp, 130.dp)) {
            drawRect(color = Color(0f, 0f, 0f, 0.4f), size = size)
        }

        Column(Modifier.padding(8.dp)) {

            if (radioLink && !stats.isLive) {
                // "No signal icon".
                Text(
                    text = "NO SIGNAL",
                    color = Color(0xFFFF5252),
                    fontFamily = FontFamily.Monospace,
                    fontSize = 15.sp,
                )
            }

            if (radioLink) {
                // Per-antenna RSSI, one icon each, as upstream lays out up to six.
                Row(verticalAlignment = Alignment.CenterVertically) {
                    if (stats.antennas == 0) {
                        SignalBars(bars = 0)
                    } else {
                        repeat(minOf(stats.antennas, MAX_BARS)) {
                            SignalBars(bars = barsForRssi(stats.bestRssi))
                            Spacer(Modifier.width(4.dp))
                        }
                    }
                    Spacer(Modifier.width(6.dp))
                    Text(
                        text = "${stats.bestRssi}dBm ${stats.bestSnr}dB",
                        color = Color.White,
                        fontFamily = FontFamily.Monospace,
                        fontSize = 11.sp,
                    )
                }
            } else {
                Text(
                    text = "APFPV",
                    color = Color(0xFF90A4AE),
                    fontFamily = FontFamily.Monospace,
                    fontSize = 11.sp,
                )
            }

            Spacer(Modifier.height(3.dp))

            // "Video FPS and resolution" and "Video codec".
            Text(
                text = "$fps fps   $codec",
                color = Color.White,
                fontFamily = FontFamily.Monospace,
                fontSize = 11.sp,
            )

            // "Video link throughput". The stats interval is 100 ms, so the
            // byte count scales by ten to reach a per-second figure.
            val kbps = stats.bytesAll * 10 * 8 / 1000
            Text(
                text = "$kbps kbit/s",
                color = Color.White,
                fontFamily = FontFamily.Monospace,
                fontSize = 11.sp,
            )

            // FEC behaviour: not a named widget upstream, but it is what the
            // SBC's wfbcli facts feed, and it is the number that tells a pilot
            // the link is degrading before the picture does.
            Text(
                text = "fec ${stats.packetsRecovered}  lost ${stats.packetsLost}" +
                    if (stats.fecK > 0) "  ${stats.fecK}/${stats.fecN}" else "",
                color = when {
                    stats.packetsLost > 0 -> Color(0xFFFF5252)
                    stats.packetsRecovered > 0 -> Color(0xFFFFC107)
                    else -> Color(0xFF9E9E9E)
                },
                fontFamily = FontFamily.Monospace,
                fontSize = 10.sp,
            )

            // "DVR status".
            if (recording) {
                Text(
                    text = "● REC",
                    color = Color(0xFFFF5252),
                    fontFamily = FontFamily.Monospace,
                    fontSize = 11.sp,
                )
            }

            // "SignalWarning".
            if (stats.isLive && stats.bestRssi <= -80) {
                Text(
                    text = "WEAK SIGNAL",
                    color = Color(0xFFFFC107),
                    fontFamily = FontFamily.Monospace,
                    fontSize = 11.sp,
                )
            }
        }
    }
}

/**
 * The signal icons upstream ships as signal1..signal6 PNGs, drawn as bars so
 * the app carries no copied assets.
 */
@Composable
private fun SignalBars(bars: Int) {
    Canvas(Modifier.size(width = 22.dp, height = 14.dp)) {
        val slot = size.width / MAX_BARS
        for (index in 0 until MAX_BARS) {
            val height = size.height * (index + 1) / MAX_BARS
            drawRect(
                color = if (index < bars) Color(0xFF4CAF50) else Color(0x33FFFFFF),
                topLeft = Offset(index * slot, size.height - height),
                size = Size(slot * 0.7f, height),
            )
        }
    }
}
