// SPDX-License-Identifier: GPL-3.0-only
package org.openipc.mobilegs.ui

import android.content.Context
import android.os.BatteryManager
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.material3.Text
import kotlinx.coroutines.delay
import org.openipc.gslib.LinkStats
import org.openipc.gslib.Telemetry
import org.openipc.mobilegs.settings.Settings

/**
 * The strip along the bottom edge, where the goggles put their own numbers.
 *
 * A 16:9 picture on a modern phone leaves letterbox either side, and that space
 * is free: putting the link and battery figures there keeps them off the video
 * entirely, which is the one place a pilot cannot afford clutter. Each element
 * is opt-in, because what matters differs between a bench test and a flight.
 */
@Composable
fun StatusStrip(
    stats: LinkStats,
    telemetry: Telemetry,
    settings: Settings,
    fps: Int,
    flightSeconds: Long,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current

    Row(
        modifier.padding(horizontal = 14.dp, vertical = 8.dp),
        horizontalArrangement = Arrangement.spacedBy(16.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (settings.stripFlightTime) {
            Element(formatDuration(flightSeconds), Color(0xFFC9CFD6))
        }

        if (settings.stripCraftBattery && telemetry.batteryVolts > 0f) {
            val remaining = telemetry.batteryRemainingPct
            val text = if (remaining >= 0f) {
                "%.1fV  %.0f%%".format(telemetry.batteryVolts, remaining)
            } else {
                "%.1fV".format(telemetry.batteryVolts)
            }
            Element(
                text,
                when {
                    remaining in 0f..20f -> Color(0xFFFF453A)
                    remaining in 0f..40f -> Color(0xFFFF9F0A)
                    else -> Color(0xFFF2F4F6)
                },
            )
        }

        if (settings.stripSignal) {
            Element(
                "${stats.bestRssi}dBm ${stats.bestSnr}dB",
                when {
                    !stats.sessionEstablished -> Color(0xFFFF453A)
                    stats.bestRssi >= -60 -> Color(0xFF4CD964)
                    stats.bestRssi >= -80 -> Color(0xFFFF9F0A)
                    else -> Color(0xFFFF453A)
                },
            )
        }

        if (settings.stripBitrate) {
            // The stats interval is 100 ms, so scale to a per-second figure.
            val mbps = stats.bytesAll * 10 * 8 / 1_000_000.0
            Element(
                if (mbps >= 1.0) "%.1f Mbps".format(mbps) else "${stats.bytesAll * 10 * 8 / 1000} kbps",
                Color(0xFFF2F4F6),
            )
        }

        if (settings.stripFps) {
            Element("$fps fps", Color(0xFFC9CFD6))
        }

        if (settings.stripPhoneBattery) {
            // Polled rather than watched: a battery broadcast receiver for a
            // number that moves once every few minutes is not worth the wiring.
            val level by produceState(initialValue = phoneBattery(context), context) {
                while (true) {
                    value = phoneBattery(context)
                    delay(30_000)
                }
            }
            Element(
                "▯ $level%",
                if (level <= 20) Color(0xFFFF9F0A) else Color(0xFFC9CFD6),
            )
        }
    }
}

@Composable
private fun Element(text: String, color: Color) {
    Text(
        text = text,
        color = color,
        fontFamily = FontFamily.Monospace,
        fontSize = 13.sp,
    )
}

private fun phoneBattery(context: Context): Int {
    val manager = context.getSystemService(Context.BATTERY_SERVICE) as? BatteryManager
    return manager?.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY) ?: 0
}

private fun formatDuration(seconds: Long): String =
    "%02d:%02d".format(seconds / 60, seconds % 60)

/**
 * What fills the screen when there is no picture.
 *
 * A ground station spends a lot of its life parked on a static screen, and an
 * OLED panel will keep whatever sat there. `drift` slides a soft gradient
 * around slowly so no pixel holds the same value; the plain fills are there for
 * anyone who would rather have a dark screen than movement in the corner of
 * their eye.
 */
@Composable
fun NoSignalBackground(style: String, modifier: Modifier = Modifier) {
    when (style) {
        "black" -> Canvas(modifier.fillMaxSize()) { drawRect(Color.Black) }

        "grey" -> Canvas(modifier.fillMaxSize()) { drawRect(Color(0xFF14181C)) }

        else -> {
            val transition = rememberInfiniteTransition(label = "drift")
            val phase by transition.animateFloat(
                initialValue = 0f,
                targetValue = 1f,
                animationSpec = infiniteRepeatable(
                    // Slow enough not to be a distraction, fast enough that no
                    // pixel sits at one value long enough to stain.
                    animation = tween(38_000, easing = LinearEasing),
                    repeatMode = RepeatMode.Reverse,
                ),
                label = "phase",
            )
            Canvas(modifier.fillMaxSize()) {
                drawRect(Color(0xFF05070A))
                val centre = Offset(
                    x = size.width * (0.2f + 0.6f * phase),
                    y = size.height * (0.75f - 0.5f * phase),
                )
                drawCircle(
                    brush = Brush.radialGradient(
                        colors = listOf(Color(0x2233414D), Color(0x00000000)),
                        center = centre,
                        radius = size.minDimension * 0.7f,
                    ),
                    radius = size.minDimension * 0.7f,
                    center = centre,
                )
            }
        }
    }
}
