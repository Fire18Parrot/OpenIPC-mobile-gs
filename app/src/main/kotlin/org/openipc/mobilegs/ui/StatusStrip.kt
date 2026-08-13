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
import androidx.compose.foundation.layout.size
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.delay
import org.openipc.gslib.LinkStats
import org.openipc.gslib.SourceKind
import org.openipc.gslib.Telemetry
import org.openipc.mobilegs.settings.Settings

/** The strip's palette, kept together so the states read consistently. */
private object Strip {
    val text = Color(0xFFF2F4F6)
    val dim = Color(0xFFB4BCC4)
    val good = Color(0xFF4CD964)
    val warn = Color(0xFFFF9F0A)
    val bad = Color(0xFFFF453A)
    val idle = Color(0x33FFFFFF)
}

/**
 * The strip along the bottom right, where the goggles keep their numbers.
 *
 * Everything here is drawn rather than spelled out where a drawing reads
 * faster: a battery you can judge at a glance without parsing a percentage,
 * bars that show margin rather than a dBm figure a pilot has to convert in
 * their head, and a throughput trace that shows whether the link is climbing
 * or collapsing - which the instantaneous number cannot.
 *
 * Each element is opt-in, because what is worth screen space on a bench is not
 * what is worth it in the air.
 */
@Composable
fun StatusStrip(
    stats: LinkStats,
    telemetry: Telemetry,
    settings: Settings,
    fps: Int,
    flightSeconds: Long,
    recording: Boolean,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current

    // Throughput history for the trace. Kept here rather than in the service
    // because nothing else wants it, and it is discarded with the view.
    val history = remember { mutableStateListOf<Float>() }
    LaunchedEffect(stats) {
        history.add(stats.bytesAll * 10 * 8 / 1_000_000f)
        if (history.size > HISTORY) history.removeAt(0)
    }

    Row(
        modifier.padding(horizontal = 16.dp, vertical = 10.dp),
        horizontalArrangement = Arrangement.spacedBy(14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (recording && settings.stripRecording) {
            Element { RecordingDot() }
        }

        if (settings.stripFlightTime) {
            Element {
                ClockGlyph(Strip.dim)
                Label(formatDuration(flightSeconds), Strip.dim)
            }
        }

        if (settings.stripAltitude) {
            Element {
                ArrowGlyph(vertical = true, color = Strip.dim)
                Label("%.0fm".format(telemetry.relativeAltitudeM), Strip.text)
            }
        }

        if (settings.stripSpeed) {
            Element {
                ArrowGlyph(vertical = false, color = Strip.dim)
                Label("%.0fm/s".format(telemetry.groundSpeedMs), Strip.text)
            }
        }

        if (settings.stripCraftBattery && telemetry.batteryVolts > 0f) {
            val remaining = telemetry.batteryRemainingPct
            val colour = when {
                remaining < 0f -> Strip.text
                remaining <= 20f -> Strip.bad
                remaining <= 40f -> Strip.warn
                else -> Strip.good
            }
            Element {
                BatteryGlyph(
                    fraction = if (remaining >= 0f) remaining / 100f else null,
                    colour = colour,
                )
                Label(
                    if (remaining >= 0f) {
                        "%.0f%%  %.1fV".format(remaining, telemetry.batteryVolts)
                    } else {
                        "%.1fV".format(telemetry.batteryVolts)
                    },
                    colour,
                )
            }
        }

        // Signal and packet accounting are wfb concepts. APFPV rides the air
        // unit's own network and never fills them in, so drawing them there
        // would mean a permanently empty gauge next to working video.
        val radioLink = settings.source != SourceKind.UDP

        if (settings.stripSignal && radioLink) {
            // Coloured off the bar count rather than a second dBm threshold, so
            // the colour can never disagree with the bars beside it.
            val bars = barsFor(stats)
            val colour = when {
                bars >= 4 -> Strip.good
                bars >= 2 -> Strip.warn
                else -> Strip.bad
            }
            Element {
                SignalGlyph(bars = bars, colour = colour)
                Label("${stats.bestRssi}dBm", colour)
            }
        }

        if (settings.stripLinkBar && radioLink) {
            Element { LinkQualityBar(stats) }
        }

        if (settings.stripBitrate) {
            val mbps = stats.bytesAll * 10 * 8 / 1_000_000.0
            Element {
                Sparkline(history)
                Label(
                    if (mbps >= 1.0) {
                        "%.1fMbps".format(mbps)
                    } else {
                        "${stats.bytesAll * 10 * 8 / 1000}kbps"
                    },
                    Strip.text,
                )
            }
        }

        if (settings.stripFps) {
            Element { Label("${fps}fps", Strip.dim) }
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
            Element {
                BatteryGlyph(
                    fraction = level / 100f,
                    colour = if (level <= 20) Strip.warn else Strip.dim,
                )
                Label("$level%", Strip.dim)
            }
        }
    }
}

/**
 * One reading: its drawn indicator and its number, kept tight together so the
 * pair reads as a unit and the gap between elements stays the wider one.
 */
@Composable
private fun Element(content: @Composable () -> Unit) {
    Row(
        horizontalArrangement = Arrangement.spacedBy(5.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        content()
    }
}

@Composable
private fun Label(text: String, color: Color) {
    Text(
        text = text,
        color = color,
        fontFamily = FontFamily.Monospace,
        fontSize = 13.sp,
    )
}

/**
 * A battery cell drawn to scale. A null fraction means the craft told us a
 * voltage but not a state of charge, so the cell is drawn empty rather than
 * guessed at from volts per cell - which varies by chemistry and sags under
 * throttle, and would be a confident-looking lie.
 */
@Composable
private fun BatteryGlyph(fraction: Float?, colour: Color) {
    Canvas(Modifier.size(width = 26.dp, height = 13.dp)) {
        val stroke = 1.4.dp.toPx()
        val nub = size.width * 0.09f
        val bodyWidth = size.width - nub

        drawRoundRect(
            color = colour,
            topLeft = Offset(stroke / 2, stroke / 2),
            size = Size(bodyWidth - stroke, size.height - stroke),
            cornerRadius = CornerRadius(2.dp.toPx()),
            style = Stroke(width = stroke),
        )

        // The terminal at the positive end.
        drawRoundRect(
            color = colour,
            topLeft = Offset(bodyWidth, size.height * 0.3f),
            size = Size(nub, size.height * 0.4f),
            cornerRadius = CornerRadius(1.dp.toPx()),
        )

        if (fraction != null) {
            val inset = stroke * 2
            val usable = bodyWidth - inset * 2
            drawRoundRect(
                color = colour,
                topLeft = Offset(inset, inset),
                size = Size(usable * fraction.coerceIn(0f, 1f), size.height - inset * 2),
                cornerRadius = CornerRadius(1.dp.toPx()),
            )
        }
    }
}

/** Six ascending bars - upstream's scale, so a full glyph means the same thing. */
@Composable
private fun SignalGlyph(bars: Int, colour: Color) {
    Canvas(Modifier.size(width = 22.dp, height = 14.dp)) {
        val count = 6
        val gap = size.width * 0.07f
        val barWidth = (size.width - gap * (count - 1)) / count
        for (i in 0 until count) {
            val tall = size.height * (0.26f + 0.148f * i)
            drawRoundRect(
                color = if (i < bars) colour else Strip.idle,
                topLeft = Offset(i * (barWidth + gap), size.height - tall),
                size = Size(barWidth, tall),
                cornerRadius = CornerRadius(0.8.dp.toPx()),
            )
        }
    }
}

/**
 * How the last interval's packets went: clean, recovered by FEC, or lost.
 *
 * The instantaneous counters are hard to read while flying, but the proportions
 * are not - a widening amber band means the link is working harder to hold the
 * picture together, which is the warning that arrives before the red one.
 */
@Composable
private fun LinkQualityBar(stats: LinkStats) {
    val total = (stats.packetsAll + stats.packetsLost).coerceAtLeast(1)
    val lost = stats.packetsLost.toFloat() / total
    val recovered = stats.packetsRecovered.toFloat() / total

    Canvas(Modifier.size(width = 40.dp, height = 6.dp)) {
        val radius = CornerRadius(1.5.dp.toPx())
        drawRoundRect(color = Strip.idle, cornerRadius = radius)

        var x = 0f
        fun band(portion: Float, colour: Color) {
            val w = size.width * portion.coerceIn(0f, 1f)
            if (w <= 0f) return
            drawRoundRect(
                color = colour,
                topLeft = Offset(x, 0f),
                size = Size(w, size.height),
                cornerRadius = radius,
            )
            x += w
        }

        band(1f - lost - recovered, Strip.good)
        band(recovered, Strip.warn)
        band(lost, Strip.bad)
    }
}

/**
 * Throughput over the last few seconds. Scaled to its own peak rather than a
 * fixed ceiling, so the shape stays readable whether the link is running at
 * 2 Mbps or 20 - what matters here is the trend, not the absolute height.
 */
@Composable
private fun Sparkline(history: List<Float>) {
    Canvas(Modifier.size(width = 46.dp, height = 14.dp)) {
        if (history.size < 2) return@Canvas
        val peak = (history.maxOrNull() ?: 0f).coerceAtLeast(0.1f)
        val step = size.width / (history.size - 1)

        val path = Path()
        history.forEachIndexed { i, value ->
            val x = i * step
            val y = size.height - (value / peak) * size.height
            if (i == 0) path.moveTo(x, y) else path.lineTo(x, y)
        }
        drawPath(
            path = path,
            color = Strip.good,
            style = Stroke(width = 1.4.dp.toPx(), cap = StrokeCap.Round),
        )
    }
}

/** The recording indicator, pulsed so it reads as live rather than painted on. */
@Composable
private fun RecordingDot() {
    val transition = rememberInfiniteTransition(label = "rec")
    val alpha by transition.animateFloat(
        initialValue = 1f,
        targetValue = 0.25f,
        animationSpec = infiniteRepeatable(
            animation = tween(900, easing = LinearEasing),
            repeatMode = RepeatMode.Reverse,
        ),
        label = "rec-alpha",
    )
    Canvas(Modifier.size(9.dp)) {
        drawCircle(color = Strip.bad.copy(alpha = alpha))
    }
}

@Composable
private fun ClockGlyph(color: Color) {
    Canvas(Modifier.size(12.dp)) {
        val stroke = 1.2.dp.toPx()
        drawCircle(
            color = color,
            radius = size.minDimension / 2 - stroke / 2,
            style = Stroke(width = stroke),
        )
        hand(color, stroke, dx = 0f, dy = -size.height * 0.26f)
        hand(color, stroke, dx = size.width * 0.2f, dy = 0f)
    }
}

private fun DrawScope.hand(color: Color, stroke: Float, dx: Float, dy: Float) {
    val centre = Offset(size.width / 2, size.height / 2)
    drawLine(
        color = color,
        start = centre,
        end = Offset(centre.x + dx, centre.y + dy),
        strokeWidth = stroke,
        cap = StrokeCap.Round,
    )
}

/** Altitude and speed, marked by direction rather than a letter. */
@Composable
private fun ArrowGlyph(vertical: Boolean, color: Color) {
    Canvas(Modifier.size(11.dp)) {
        val stroke = 1.3.dp.toPx()
        val inset = stroke
        val head = size.minDimension * 0.32f

        if (vertical) {
            val top = Offset(size.width / 2, inset)
            drawLine(color, top, Offset(size.width / 2, size.height - inset), stroke, StrokeCap.Round)
            drawLine(color, top, Offset(size.width / 2 - head, inset + head), stroke, StrokeCap.Round)
            drawLine(color, top, Offset(size.width / 2 + head, inset + head), stroke, StrokeCap.Round)
        } else {
            val tip = Offset(size.width - inset, size.height / 2)
            drawLine(color, Offset(inset, size.height / 2), tip, stroke, StrokeCap.Round)
            drawLine(color, tip, Offset(size.width - inset - head, size.height / 2 - head), stroke, StrokeCap.Round)
            drawLine(color, tip, Offset(size.width - inset - head, size.height / 2 + head), stroke, StrokeCap.Round)
        }
    }
}

/**
 * Bars, from the same `osd.json` band table the metrics box uses - shared
 * rather than restated, so the two indicators cannot disagree about the signal.
 */
private fun barsFor(stats: LinkStats): Int =
    if (!stats.sessionEstablished) 0 else barsForRssi(stats.bestRssi)

private fun phoneBattery(context: Context): Int {
    val manager = context.getSystemService(Context.BATTERY_SERVICE) as? BatteryManager
    return manager?.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY) ?: 0
}

private fun formatDuration(seconds: Long): String =
    "%02d:%02d".format(seconds / 60, seconds % 60)

private const val HISTORY = 64

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
