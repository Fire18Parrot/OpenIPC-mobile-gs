// SPDX-License-Identifier: GPL-3.0-only
package org.openipc.mobilegs.ui

import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
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
        val context = LocalContext.current
        // If the last run died, show the report rather than making the user go
        // looking for it.
        var showDiagnostics by remember {
            mutableStateOf(CrashReporter.lastCrash(context) != null)
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
                SettingsScreen(
                    settings = settings,
                    settingsRepository = settingsRepository,
                    service = service,
                    keyFile = keyFile,
                    onDismiss = { showSettings = false },
                    onOpenDiagnostics = {
                        showSettings = false
                        showDiagnostics = true
                    },
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

    Box(Modifier.fillMaxSize()) {
        // A SurfaceView, not a TextureView: it hands MediaCodec a buffer queue
        // the compositor consumes directly, which is both lower latency and
        // lower power than routing frames through the view hierarchy.
        AndroidView(
            modifier = Modifier.fillMaxSize(),
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

        if (settings.osdEnabled) {
            osd?.let { OsdOverlay(it, Modifier.fillMaxSize()) }
        }

        LinkHud(
            stats = stats,
            telemetry = telemetry,
            status = status,
            running = running,
            recording = service.isRecording,
            onOpenSettings = onOpenSettings,
            onToggle = { if (running) onStop() else onStart() },
            onToggleRecording = { service.toggleRecording(settings.codec) },
            modifier = Modifier.align(Alignment.TopStart).padding(12.dp),
        )
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
 * The link readout. This is the equivalent of what wfb-cli shows on an SBC:
 * signal, FEC behaviour and whether the session is actually up.
 */
@Composable
private fun LinkHud(
    stats: LinkStats,
    telemetry: Telemetry,
    status: String,
    running: Boolean,
    recording: Boolean,
    onOpenSettings: () -> Unit,
    onToggle: () -> Unit,
    onToggleRecording: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Card(modifier) {
        Column(Modifier.padding(10.dp)) {
            val linkColour = when {
                !stats.sessionEstablished -> Color(0xFFFF5252)
                stats.packetsLost > 0 -> Color(0xFFFFC107)
                else -> Color(0xFF4CAF50)
            }
            Text(
                text = if (stats.sessionEstablished) "LINK UP" else "NO LINK",
                color = linkColour,
                fontFamily = FontFamily.Monospace,
            )
            Text(
                text = "RSSI ${stats.bestRssi} dBm   SNR ${stats.bestSnr} dB   ant ${stats.antennas}",
                color = Color.White,
                fontFamily = FontFamily.Monospace,
                fontSize = 12.sp,
            )
            Text(
                text = "pkt ${stats.packetsAll}  lost ${stats.packetsLost}  fec ${stats.packetsRecovered}",
                color = Color.White,
                fontFamily = FontFamily.Monospace,
                fontSize = 12.sp,
            )
            if (stats.fecK > 0) {
                Text(
                    text = "FEC ${stats.fecK}/${stats.fecN}",
                    color = Color.White,
                    fontFamily = FontFamily.Monospace,
                    fontSize = 12.sp,
                )
            }
            if (telemetry.batteryVolts > 0f) {
                Text(
                    text = "BAT %.1fV  ALT %.0fm  SPD %.0fm/s  SAT %d".format(
                        telemetry.batteryVolts,
                        telemetry.relativeAltitudeM,
                        telemetry.groundSpeedMs,
                        telemetry.satellites,
                    ),
                    color = Color.White,
                    fontFamily = FontFamily.Monospace,
                    fontSize = 12.sp,
                )
            }
            Text(status, color = Color(0xFFB0BEC5), fontSize = 11.sp)

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                TextButton(onClick = onToggle) { Text(if (running) "Stop" else "Start") }
                TextButton(onClick = onToggleRecording) {
                    Text(if (recording) "Stop REC" else "Record")
                }
                TextButton(onClick = onOpenSettings) { Text("Settings") }
            }
        }
    }
}

/**
 * The settings, standing in for the web UI an SBC ground station serves on port
 * 5000 - reachable here without a second device.
 */
@Composable
private fun SettingsScreen(
    settings: Settings,
    settingsRepository: SettingsRepository,
    service: GroundStationService?,
    keyFile: File,
    onDismiss: () -> Unit,
    onOpenDiagnostics: () -> Unit,
) {
    val scope = rememberCoroutineScope()
    fun edit(transform: (Settings) -> Settings) {
        scope.launch {
            settingsRepository.update(transform)
            service?.applySettings(transform(settings))
        }
    }

    Box(Modifier.fillMaxSize().background(Color(0xF007090B))) {
        Column(
            Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Text("Ground station settings", color = Color.White)

            SectionTitle("Link")
            EnumRow(
                label = "Source",
                current = settings.source.name,
                options = SourceKind.entries.map { it.name },
            ) { index -> edit { it.copy(source = SourceKind.entries[index]) } }

            if (settings.source == SourceKind.DEVOURER) {
                NumberRow("Channel", settings.channel) { value ->
                    edit { it.copy(channel = value) }
                    service?.setChannel(value, settings.bandwidth.mhz)
                }
                EnumRow(
                    label = "Bandwidth",
                    current = "${settings.bandwidth.mhz} MHz",
                    options = Bandwidth.entries.map { "${it.mhz} MHz" },
                ) { index -> edit { it.copy(bandwidth = Bandwidth.entries[index]) } }
                NumberRow("Link ID", settings.linkId) { value -> edit { it.copy(linkId = value) } }

                val keyState = if (keyFile.exists()) {
                    NativeGroundStation.validateKey(keyFile.absolutePath) ?: "gs.key loaded"
                } else {
                    "no gs.key - import one from your air unit"
                }
                Text(keyState, color = Color(0xFFB0BEC5), fontSize = 12.sp)
            } else {
                NumberRow("Video UDP port", settings.udpVideoPort) { value ->
                    edit { it.copy(udpVideoPort = value) }
                }
            }

            EnumRow(
                label = "Codec",
                current = settings.codec.name,
                options = VideoCodec.entries.map { it.name },
            ) { index -> edit { it.copy(codec = VideoCodec.entries[index]) } }

            SectionTitle("MAVLink")
            Text(
                "Attach a ground control station to the telemetry stream. " +
                    "UDP_OUT sends to a fixed address, UDP_SERVER and TCP_SERVER wait " +
                    "for the GCS to connect.",
                color = Color(0xFF90A4AE),
                fontSize = 11.sp,
            )
            SwitchRow("Enabled", settings.mavlinkEnabled) { value ->
                edit { it.copy(mavlinkEnabled = value) }
            }
            EnumRow(
                label = "Transport",
                current = settings.mavlinkKind.name,
                options = MavlinkEndpointKind.entries.map { it.name },
            ) { index -> edit { it.copy(mavlinkKind = MavlinkEndpointKind.entries[index]) } }
            TextRow("Host", settings.mavlinkHost) { value -> edit { it.copy(mavlinkHost = value) } }
            NumberRow("Port", settings.mavlinkPort) { value -> edit { it.copy(mavlinkPort = value) } }
            SwitchRow("Allow uplink from GCS", settings.mavlinkUplink) { value ->
                edit { it.copy(mavlinkUplink = value) }
            }

            SwitchRow("Second endpoint", settings.secondEndpointEnabled) { value ->
                edit { it.copy(secondEndpointEnabled = value) }
            }
            if (settings.secondEndpointEnabled) {
                EnumRow(
                    label = "Second transport",
                    current = settings.secondEndpointKind.name,
                    options = MavlinkEndpointKind.entries.map { it.name },
                ) { index ->
                    edit { it.copy(secondEndpointKind = MavlinkEndpointKind.entries[index]) }
                }
                NumberRow("Second port", settings.secondEndpointPort) { value ->
                    edit { it.copy(secondEndpointPort = value) }
                }
            }

            SectionTitle("Adaptive link")
            SwitchRow("Enabled", settings.alinkEnabled) { value ->
                edit { it.copy(alinkEnabled = value) }
            }
            if (settings.alinkEnabled) {
                TextRow("Air unit address", settings.alinkHost) { value ->
                    edit { it.copy(alinkHost = value) }
                }
                NumberRow("Port", settings.alinkPort) { value -> edit { it.copy(alinkPort = value) } }
                SwitchRow("Request keyframes on loss", settings.alinkAllowIdr) { value ->
                    edit { it.copy(alinkAllowIdr = value) }
                }
                SwitchRow("Apply noise penalty", settings.alinkAllowPenalty) { value ->
                    edit { it.copy(alinkAllowPenalty = value) }
                }
                SwitchRow("Allow FEC increase", settings.alinkAllowFecIncrease) { value ->
                    edit { it.copy(alinkAllowFecIncrease = value) }
                }
            }

            SectionTitle("Display and recording")
            SwitchRow("Show OSD", settings.osdEnabled) { value -> edit { it.copy(osdEnabled = value) } }
            SwitchRow("Record on start", settings.recordVideo) { value ->
                edit { it.copy(recordVideo = value) }
            }

            SectionTitle("Diagnostics")
            Text(
                "The app's own log, readable on the phone - no computer needed.",
                color = Color(0xFF90A4AE),
                fontSize = 11.sp,
            )
            TextButton(onClick = onOpenDiagnostics) { Text("Open diagnostics") }

            Button(onClick = onDismiss, modifier = Modifier.fillMaxWidth()) { Text("Close") }
        }
    }
}

@Composable
private fun SectionTitle(text: String) {
    Text(text, color = Color(0xFFFF7A00), modifier = Modifier.padding(top = 8.dp))
}

@Composable
private fun SwitchRow(label: String, value: Boolean, onChange: (Boolean) -> Unit) {
    Row(
        Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, color = Color.White)
        Switch(checked = value, onCheckedChange = onChange)
    }
}

@Composable
private fun TextRow(label: String, value: String, onChange: (String) -> Unit) {
    OutlinedTextField(
        value = value,
        onValueChange = onChange,
        label = { Text(label) },
        singleLine = true,
        modifier = Modifier.fillMaxWidth(),
    )
}

@Composable
private fun NumberRow(label: String, value: Int, onChange: (Int) -> Unit) {
    var text by remember(value) { mutableStateOf(value.toString()) }
    OutlinedTextField(
        value = text,
        onValueChange = { entered ->
            text = entered.filter { it.isDigit() }
            text.toIntOrNull()?.let(onChange)
        },
        label = { Text(label) },
        singleLine = true,
        modifier = Modifier.fillMaxWidth(),
    )
}

@Composable
private fun EnumRow(
    label: String,
    current: String,
    options: List<String>,
    onSelect: (Int) -> Unit,
) {
    var expanded by remember { mutableStateOf(false) }
    Row(
        Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, color = Color.White)
        Box {
            TextButton(onClick = { expanded = true }) { Text(current) }
            DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
                options.forEachIndexed { index, option ->
                    DropdownMenuItem(
                        text = { Text(option) },
                        onClick = {
                            expanded = false
                            onSelect(index)
                        },
                    )
                }
            }
        }
    }
}
