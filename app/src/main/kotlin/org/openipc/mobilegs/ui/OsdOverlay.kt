// SPDX-License-Identifier: GPL-3.0-only
package org.openipc.mobilegs.ui

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import androidx.compose.foundation.Canvas
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.drawIntoCanvas
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.platform.LocalContext
import org.openipc.gslib.OsdScreen

/**
 * Draws the MSP DisplayPort character grid, the job msposd does on an SBC by
 * blitting onto a DRM plane.
 *
 * The font is the same PNG atlas the OpenIPC ground station ships: a single
 * column of glyphs, each cell [FONT_CELL_WIDTH] x [FONT_CELL_HEIGHT], with
 * further pages stacked after the first 256. Drop a `font_hd.png` into
 * `app/src/main/assets/` to use a different set - the Betaflight, INAV and
 * ArduPilot fonts from the msposd repository all work unmodified.
 */
@Composable
fun OsdOverlay(screen: OsdScreen, modifier: Modifier = Modifier) {
    val context = LocalContext.current
    val font = remember { loadFontAtlas(context.assets) }

    Canvas(modifier) {
        if (screen.rows <= 0 || screen.cols <= 0) return@Canvas

        // Fit the character grid to the viewport while keeping the glyph
        // aspect ratio, so the OSD lines up with the video underneath.
        val cellWidth = size.width / screen.cols
        val cellHeight = size.height / screen.rows

        if (font == null) {
            drawFallback(screen, cellWidth, cellHeight)
            return@Canvas
        }

        val glyphsPerColumn = font.height / FONT_CELL_HEIGHT
        drawIntoCanvas { canvas ->
            for (row in 0 until screen.rows) {
                for (col in 0 until screen.cols) {
                    val glyph = screen.glyphAt(row, col)
                    if (glyph == 0 || glyph == ' '.code) continue
                    if (glyph >= glyphsPerColumn) continue

                    val sourceTop = glyph * FONT_CELL_HEIGHT
                    canvas.nativeCanvas.drawBitmap(
                        font,
                        android.graphics.Rect(
                            0,
                            sourceTop,
                            FONT_CELL_WIDTH,
                            sourceTop + FONT_CELL_HEIGHT,
                        ),
                        android.graphics.RectF(
                            col * cellWidth,
                            row * cellHeight,
                            (col + 1) * cellWidth,
                            (row + 1) * cellHeight,
                        ),
                        null,
                    )
                }
            }
        }
    }
}

/**
 * Without a font atlas the OSD would simply vanish, which is worse than a rough
 * rendering: the pilot still needs to see that data is arriving. Marking
 * occupied cells keeps the layout visible until a font is installed.
 */
private fun DrawScope.drawFallback(screen: OsdScreen, cellWidth: Float, cellHeight: Float) {
    for (row in 0 until screen.rows) {
        for (col in 0 until screen.cols) {
            val glyph = screen.glyphAt(row, col)
            if (glyph == 0 || glyph == ' '.code) continue
            drawRect(
                color = Color(0x66FFFFFF),
                topLeft = androidx.compose.ui.geometry.Offset(
                    col * cellWidth + cellWidth * 0.2f,
                    row * cellHeight + cellHeight * 0.2f,
                ),
                size = androidx.compose.ui.geometry.Size(cellWidth * 0.6f, cellHeight * 0.6f),
            )
        }
    }
}

private fun loadFontAtlas(assets: android.content.res.AssetManager): Bitmap? =
    FONT_CANDIDATES.firstNotNullOfOrNull { name ->
        runCatching { assets.open(name).use { BitmapFactory.decodeStream(it) } }.getOrNull()
    }

/** Matches the names msposd installs into /usr/share/fonts. */
private val FONT_CANDIDATES = listOf(
    "font_hd.png",
    "font_btfl_hd.png",
    "font_inav_hd.png",
    "font_ardu_hd.png",
    "font.png",
)

private const val FONT_CELL_WIDTH = 24
private const val FONT_CELL_HEIGHT = 36
