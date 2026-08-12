// SPDX-License-Identifier: GPL-3.0-only
package org.openipc.mobilegs.ui

import android.os.SystemClock
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import java.io.File
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import org.openipc.gslib.Bandwidth
import org.openipc.gslib.LinkStats
import org.openipc.gslib.MavlinkEndpointKind
import org.openipc.gslib.NativeGroundStation
import org.openipc.gslib.SourceKind
import org.openipc.gslib.Telemetry
import org.openipc.gslib.VideoCodec
import org.openipc.mobilegs.diag.CrashReporter
import org.openipc.mobilegs.ui.gsmenu.GsMenuScreen
import org.openipc.mobilegs.service.GroundStationService
import org.openipc.mobilegs.settings.Settings
import org.openipc.mobilegs.settings.SettingsRepository

private val DarkScheme = darkColorScheme(
    primary = Color(0xFFFF7A00),
    background = Color(0xFF07090B),
    surface = Color(0xFF12171C),
)

@Composable
fun GroundStationApp(
    serviceFlow: StateFlow<GroundStationService?>,
    settingsRepository: SettingsRepository,
    onStartRequested: (Settings) -> Unit,
    onStopRequested: () -> Unit,
    keyFile: File,
) {
    MaterialTheme(colorScheme = DarkScheme) {
        val service by serviceFlow.collectAsState()
        val settings by settingsRepository.settings.collectAsState(initial = Settings())
        var showSettings by remember { mutableStateOf(false) }
        val scope = rememberCoroutineScope()
        val context = LocalContext.current
        // If the last run died, show the report rather than making the user go
        // looking for it.
        var showDiagnostics by remember {
            mutableStateOf(CrashReporter.lastCrash(context) != null)
        }

        // The menu's Status panel needs live link state. Collected here rather
        // than at the call site because a composable may not be invoked from
        // inside a safe-call chain, and the service is nullable until bound.
        var menuStats by remember { mutableStateOf(LinkStats()) }
        var menuFps by remember { mutableStateOf(0) }
        LaunchedEffect(service) {
            val active = service ?: return@LaunchedEffect
            launch { active.linkStats.collect { menuStats = it } }
            launch { active.videoFps.collect { menuFps = it } }
        }

        Box(Modifier.fillMaxSize().background(Color.Black)) {
            FlightScreen(
                service = service,
                settings = settings,
                onOpenSettings = { showSettings = true },
                onStart = { onStartRequested(settings) },
                onStop = onStopRequested,
            )

            if (showSettings) {
                GsMenuScreen(
                    settings = settings,
                    stats = menuStats,
                    videoFps = menuFps,
                    keyStatus = keyStatus(keyFile),
                    onChange = { updated ->
                        scope.launch { settingsRepository.update { updated } }
                        service?.applySettings(updated)
                    },
                    onOpenDiagnostics = {
                        showSettings = false
                        showDiagnostics = true
                    },
                    onDismiss = { showSettings = false },
                )
            }

            if (showDiagnostics) {
                DiagnosticsScreen(onDismiss = { showDiagnostics = false })
            }
        }
    }
}

/**
 * The flight view: video underneath, OSD and link state on top. Nothing here
 * blocks the video path - every value shown is a snapshot published by the
 * native stats thread.
 */
@Composable
private fun FlightScreen(
    service: GroundStationService?,
    settings: Settings,
    onOpenSettings: () -> Unit,
    onStart: () -> Unit,
    onStop: () -> Unit,
) {
    if (service == null) {
        EmptyState(onOpenSettings)
        return
    }
    val stats by service.linkStats.collectAsState()
    val telemetry by service.telemetry.collectAsState()
    val osd by service.osd.collectAsState()
    val status by service.status.collectAsState()
    val running by service.running.collectAsState()
    val fps by service.videoFps.collectAsState()
    val videoSize by service.videoSize.collectAsState()
    val linkUpSince by service.linkUpSinceMs.collectAsState()

    // The flight clock, ticked here rather than in the service: it is a
    // presentation concern, and one recomposition a second is nothing.
    var flightSeconds by remember { mutableStateOf(0L) }
    LaunchedEffect(linkUpSince) {
        if (linkUpSince == 0L) {
            flightSeconds = 0L
            return@LaunchedEffect
        }
        while (true) {
            flightSeconds = (SystemClock.elapsedRealtime() - linkUpSince) / 1000
            delay(1000)
        }
    }

    // Whether to cover the screen with the no-signal fill. Held off for a
    // moment after the link drops, because packetsAll is a per-interval count
    // and a lossy link can read zero for one interval while the picture is
    // still perfectly good - flashing a background over live video would be
    // worse than the gap it papers over.
    var noPicture by remember { mutableStateOf(true) }
    LaunchedEffect(stats.isLive) {
        if (stats.isLive) {
            noPicture = false
        } else {
            delay(1500)
            noPicture = true
        }
    }

    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        // A SurfaceView, not a TextureView: it hands MediaCodec a buffer queue
        // the compositor consumes directly, which is both lower latency and
        // lower power than routing frames through the view hierarchy.
        //
        // Sized to the stream's own aspect rather than the screen's. Filling a
        // 20:9 phone with a 16:9 picture would stretch it; this gives the video
        // every pixel it can honestly use and leaves the rest black.
        val aspect = if (videoSize.second > 0) {
            videoSize.first.toFloat() / videoSize.second.toFloat()
        } else {
            16f / 9f
        }
        AndroidView(
            modifier = Modifier.fillMaxWidth().aspectRatio(aspect),
            factory = { context ->
                SurfaceView(context).apply {
                    holder.addCallback(object : SurfaceHolder.Callback {
                        override fun surfaceCreated(holder: SurfaceHolder) {
                            service.attachSurface(holder.surface, settings.codec)
                        }

                        override fun surfaceChanged(
                            holder: SurfaceHolder,
                            format: Int,
                            width: Int,
                            height: Int,
                        ) = Unit

                        override fun surfaceDestroyed(holder: SurfaceHolder) {
                            service.attachSurface(null, settings.codec)
                        }
                    })
                }
            },
        )

        // Over the SurfaceView rather than behind it: an idle SurfaceView is an
        // opaque black hole punched through the window, so anything drawn under
        // it is invisible. A ground station spends most of its life on this
        // screen, and an OLED panel keeps whatever sits still on it - hence the
        // moving default. Removed the instant video returns.
        if (noPicture) {
            NoSignalBackground(settings.noSignalStyle, Modifier.fillMaxSize())
        }

        if (settings.osdEnabled) {
            osd?.let { OsdOverlay(it, Modifier.fillMaxSize()) }
        }

        // Upstream places the metrics box at the top right (x = -270 in
        // osd.json), so it goes there.
        InGoggleOsd(
            stats = stats,
            // Show what is on the wire, not what was asked for: "auto" tells
            // a pilot nothing, and the difference is what a black screen means.
            codec = stats.detectedCodec.takeIf { it != VideoCodec.AUTO }
                ?.name?.lowercase() ?: "no video",
            fps = fps,
            recording = service.isRecording,
            modifier = Modifier.align(Alignment.TopEnd).padding(8.dp),
        )

        // Bottom right, where the goggles put their numbers. Which of them
        // appear is set per element in the menu, under Camera.
        StatusStrip(
            stats = stats,
            telemetry = telemetry,
            settings = settings,
            fps = fps,
            flightSeconds = flightSeconds,
            modifier = Modifier.align(Alignment.BottomEnd),
        )

        // The SBC opens its menu with a goggle button; a phone needs something
        // to touch, so these sit out of the way at the bottom.
        Row(
            modifier = Modifier.align(Alignment.BottomStart).padding(10.dp),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            TextButton(onClick = { if (running) onStop() else onStart() }) {
                Text(if (running) "Stop" else "Start")
            }
            TextButton(onClick = { service.toggleRecording(settings.codec) }) {
                Text(if (service.isRecording) "Stop REC" else "Record")
            }
            TextButton(onClick = onOpenSettings) { Text("Menu") }
            Text(
                text = status,
                color = Color(0xFF90A4AE),
                fontFamily = FontFamily.Monospace,
                fontSize = 10.sp,
            )
        }
    }
}

@Composable
private fun EmptyState(onOpenSettings: () -> Unit) {
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Text("Starting the ground station...", color = Color.White)
            TextButton(onClick = onOpenSettings) { Text("Settings") }
        }
    }
}

/**
 * gs.key state, shown in the menu footer. The SBC's in-goggle menu has no key
 * entry - keys get there over SSH or on the SD card - but on a phone this is
 * the only place to see whether the ground station can decrypt at all.
 */
private fun keyStatus(keyFile: File): String = when {
    !keyFile.exists() -> "gs.key missing - copy one from your air unit"
    else -> NativeGroundStation.validateKey(keyFile.absolutePath)?.let { "gs.key invalid: $it" }
        ?: "gs.key loaded"
}
