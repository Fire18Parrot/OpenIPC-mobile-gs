// SPDX-License-Identifier: GPL-3.0-only
package org.openipc.mobilegs.ui.gsmenu

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Close
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.DpOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import java.io.File
import org.openipc.gslib.LinkStats
import org.openipc.mobilegs.dvr.VideoRecorder
import org.openipc.mobilegs.settings.Settings

/**
 * The ground-station menu in the goggles layout: an icon rail on the left, a
 * titled panel of rows on the right, floating over the video.
 *
 * The content is the ported `gsmenu` tree unchanged - see [GsMenu] - with
 * [GsRails] deciding only how it is grouped on screen. Values open a list
 * rather than cycling, because reaching channel 36 from 161 by repeated taps is
 * no way to run a menu.
 */
private object Goggles {
    val panel = Color(0xC73A3E44)
    val panelDeep = Color(0x8C181B1E)
    val hairline = Color(0x47FFFFFF)
    val hairlineSoft = Color(0x1FFFFFFF)
    val scrim = Color(0xC705070A)
    val text = Color(0xFFF2F4F6)
    val textDim = Color(0xFFC9CFD6)
    val textFaint = Color(0xFF8D959D)
    val focus = Color(0xFFFFD400)
    val on = Color(0xFF4CD964)
    val warn = Color(0xFFFF9F0A)
    val bad = Color(0xFFFF453A)
}

@Composable
fun GsMenuScreen(
    settings: Settings,
    stats: LinkStats,
    videoFps: Int,
    keyStatus: String,
    onChange: (Settings) -> Unit,
    onOpenDiagnostics: () -> Unit,
    onDismiss: () -> Unit,
) {
    val context = LocalContext.current
    var railId by remember { mutableStateOf(GsRails.TRANSMISSION) }
    var importResult by remember { mutableStateOf<String?>(null) }

    // Without a gs.key the wfb path cannot decrypt anything, and the SBC's
    // answer - drop the file on the SD card, or scp it - is not open to a
    // phone. So the menu has to be able to take one.
    val importKey = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument(),
    ) { uri: Uri? ->
        if (uri == null) return@rememberLauncherForActivityResult
        importResult = runCatching {
            val target = File(context.filesDir, "gs.key")
            context.contentResolver.openInputStream(uri).use { input ->
                requireNotNull(input) { "could not read the file" }
                target.outputStream().use { output -> input.copyTo(output) }
            }
            val size = target.length()
            // wfb-ng's layout is exactly 64 bytes: the ground station's secret
            // key followed by the air unit's public key.
            if (size != 64L) {
                target.delete()
                "not a gs.key: expected 64 bytes, got $size"
            } else {
                "gs.key imported"
            }
        }.getOrElse { "import failed: ${it.message}" }
    }

    val rail = GsRails.byId(railId)

    Box(Modifier.fillMaxSize().background(Goggles.scrim)) {
        Row(
            Modifier
                .align(Alignment.CenterStart)
                .padding(start = 24.dp, end = 24.dp)
                .fillMaxHeight()
                .padding(vertical = 28.dp),
            horizontalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            RailColumn(
                current = rail.id,
                onSelect = { railId = it },
                onDismiss = onDismiss,
            )

            Panel(rail.panelTitle) {
                when (rail.id) {
                    GsRails.STATUS -> statusRows(stats, videoFps, keyStatus)
                    GsRails.ALBUM -> albumRows(context)
                    GsRails.MORE -> moreRows(
                        rail = rail,
                        settings = settings,
                        keyStatus = keyStatus,
                        importResult = importResult,
                        onImportKey = { importKey.launch(arrayOf("*/*")) },
                        onOpenDiagnostics = onOpenDiagnostics,
                        onChange = onChange,
                    )
                    else -> settingRows(rail, settings, onChange)
                }
            }
        }
    }
}

// --- chrome ----------------------------------------------------------------

@Composable
private fun RailColumn(current: String, onSelect: (String) -> Unit, onDismiss: () -> Unit) {
    Column(
        Modifier
            .width(112.dp)
            .fillMaxHeight()
            .background(Goggles.panel, RoundedCornerShape(5.dp))
            .border(1.dp, Goggles.hairline, RoundedCornerShape(5.dp))
            .padding(vertical = 6.dp),
    ) {
        GsRails.entries.forEach { entry ->
            val selected = entry.id == current
            Column(
                Modifier
                    .fillMaxWidth()
                    .background(if (selected) Goggles.panelDeep else Color.Transparent)
                    .clickable { onSelect(entry.id) }
                    .padding(vertical = 14.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                Icon(
                    imageVector = entry.icon,
                    contentDescription = entry.title,
                    tint = if (selected) Goggles.focus else Goggles.textDim,
                    modifier = Modifier.size(22.dp),
                )
                Text(
                    text = entry.title,
                    color = if (selected) Goggles.text else Goggles.textDim,
                    fontSize = 11.sp,
                )
            }
        }

        Spacer(Modifier.weight(1f))

        // The goggles close their menu with a hardware button; a phone needs
        // something to press, and it belongs with the other navigation.
        Column(
            Modifier
                .fillMaxWidth()
                .clickable(onClick = onDismiss)
                .padding(vertical = 14.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            Icon(
                imageVector = Icons.Outlined.Close,
                contentDescription = "Close",
                tint = Goggles.textDim,
                modifier = Modifier.size(22.dp),
            )
            Text(text = "Close", color = Goggles.textDim, fontSize = 11.sp)
        }
    }
}

@Composable
private fun Panel(title: String, content: androidx.compose.foundation.lazy.LazyListScope.() -> Unit) {
    Column(
        Modifier
            .widthIn(min = 320.dp, max = 520.dp)
            .fillMaxHeight()
            .background(Goggles.panel, RoundedCornerShape(5.dp))
            .border(1.dp, Goggles.hairline, RoundedCornerShape(5.dp)),
    ) {
        Text(
            text = title,
            color = Goggles.text,
            fontSize = 15.sp,
            fontWeight = FontWeight.Medium,
            modifier = Modifier
                .fillMaxWidth()
                .padding(vertical = 13.dp),
            textAlign = androidx.compose.ui.text.style.TextAlign.Center,
        )
        Box(Modifier.fillMaxWidth().height(1.dp).background(Goggles.hairlineSoft))
        LazyColumn(Modifier.fillMaxSize(), content = content)
    }
}

/** One row: label on the left, whatever changes it on the right. */
@Composable
private fun MenuRow(
    label: String,
    note: String = "",
    enabled: Boolean = true,
    onClick: (() -> Unit)? = null,
    trailing: @Composable () -> Unit,
) {
    Row(
        Modifier
            .fillMaxWidth()
            .defaultMinSize(minHeight = 52.dp)
            .then(
                if (onClick != null && enabled) Modifier.clickable(onClick = onClick) else Modifier,
            )
            .padding(horizontal = 16.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Column(Modifier.weight(1f)) {
            Text(
                text = label,
                color = if (enabled) Goggles.text else Goggles.textFaint,
                fontSize = 14.sp,
            )
            if (note.isNotEmpty()) {
                Text(text = note, color = Goggles.textFaint, fontSize = 10.sp)
            }
        }
        trailing()
    }
}

/** The value list. A Popup, so it is never clipped by the scrolling panel. */
@Composable
private fun ValueDropdown(
    current: String,
    options: List<String>,
    enabled: Boolean,
    onSelect: (String) -> Unit,
) {
    var expanded by remember { mutableStateOf(false) }

    Box {
        Row(
            Modifier
                .then(if (enabled) Modifier.clickable { expanded = true } else Modifier)
                .padding(vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(7.dp),
        ) {
            Text(
                text = current.ifEmpty { "-" },
                color = if (enabled) Goggles.textDim else Goggles.textFaint,
                fontSize = 13.sp,
            )
            if (enabled) {
                Text(
                    text = "▾",
                    color = if (expanded) Goggles.focus else Goggles.textDim,
                    fontSize = 11.sp,
                )
            }
        }

        DropdownMenu(
            expanded = expanded,
            onDismissRequest = { expanded = false },
            offset = DpOffset(0.dp, 4.dp),
            modifier = Modifier
                .background(Color(0xF71E2125))
                .heightIn(max = 264.dp),
        ) {
            options.forEach { option ->
                val isCurrent = option == current
                DropdownMenuItem(
                    text = {
                        Text(
                            text = option,
                            color = if (isCurrent) Goggles.focus else Goggles.text,
                            fontSize = 13.sp,
                        )
                    },
                    trailingIcon = if (isCurrent) {
                        { Text("✓", color = Goggles.focus, fontSize = 12.sp) }
                    } else {
                        null
                    },
                    onClick = {
                        expanded = false
                        onSelect(option)
                    },
                )
            }
        }
    }
}

@Composable
private fun GogglesSwitch(checked: Boolean, enabled: Boolean, onChange: (Boolean) -> Unit) {
    Switch(
        checked = checked,
        onCheckedChange = if (enabled) onChange else null,
        enabled = enabled,
        colors = SwitchDefaults.colors(
            checkedThumbColor = Color.White,
            checkedTrackColor = Goggles.on,
            uncheckedThumbColor = Color.White,
            uncheckedTrackColor = Color(0x38FFFFFF),
            uncheckedBorderColor = Color.Transparent,
        ),
    )
}

// --- panel contents --------------------------------------------------------

private fun androidx.compose.foundation.lazy.LazyListScope.settingRows(
    rail: GsRail,
    settings: Settings,
    onChange: (Settings) -> Unit,
) {
    val menuItems = rail.paths.mapNotNull { GsMenu.item(it) }
    items(menuItems) { item ->
        val current = GsMenuBinding.read(item.path, settings)
        val options = GsMenuBinding.options(item)
        val editable = item.supported &&
            current != null &&
            GsMenuBinding.write(item.path, current, settings) != null

        val note = when {
            !item.supported && item.help.isNotEmpty() -> item.help
            !item.supported -> "not available on a phone"
            else -> item.help
        }

        if (item.value is GsMenuValue.Toggle) {
            MenuRow(item.label, note, enabled = editable) {
                GogglesSwitch(
                    checked = current == "on",
                    enabled = editable,
                ) { checked ->
                    GsMenuBinding.write(item.path, if (checked) "on" else "off", settings)
                        ?.let(onChange)
                }
            }
        } else {
            MenuRow(item.label, note, enabled = editable) {
                ValueDropdown(
                    current = current ?: "-",
                    options = options,
                    enabled = editable && options.isNotEmpty(),
                ) { picked ->
                    GsMenuBinding.write(item.path, picked, settings)?.let(onChange)
                }
            }
        }
    }
}

private fun androidx.compose.foundation.lazy.LazyListScope.statusRows(
    stats: LinkStats,
    videoFps: Int,
    keyStatus: String,
) {
    val rows = listOf(
        Triple("Link", if (stats.sessionEstablished) "Connected" else "No link",
            if (stats.sessionEstablished) Goggles.on else Goggles.bad),
        Triple("Signal", "${stats.bestRssi} dBm",
            when {
                stats.bestRssi >= -60 -> Goggles.on
                stats.bestRssi >= -80 -> Goggles.warn
                else -> Goggles.bad
            }),
        Triple("SNR", "${stats.bestSnr} dB", Goggles.textDim),
        Triple("Antennas", stats.antennas.toString(), Goggles.textDim),
        Triple("FEC recovered / lost", "${stats.packetsRecovered} / ${stats.packetsLost}",
            if (stats.packetsLost > 0) Goggles.bad else Goggles.textDim),
        // The stats interval is 100 ms, so scale to a per-second figure.
        Triple("Bitrate", "${stats.bytesAll * 10 * 8 / 1000} kbit/s", Goggles.textDim),
        Triple("Video", "${stats.detectedCodec.name.lowercase()} · $videoFps fps", Goggles.textDim),
        Triple("FEC", if (stats.fecK > 0) "${stats.fecK}/${stats.fecN}" else "-", Goggles.textDim),
        Triple("gs.key", keyStatus, if (keyStatus.startsWith("gs.key loaded")) Goggles.on else Goggles.warn),
    )
    items(rows) { (label, value, tone) ->
        MenuRow(label) {
            Text(text = value, color = tone, fontSize = 13.sp)
        }
    }
}

private fun androidx.compose.foundation.lazy.LazyListScope.albumRows(
    context: android.content.Context,
) {
    val recordings = VideoRecorder().listRecordings(context)
    if (recordings.isEmpty()) {
        item {
            MenuRow("No recordings yet", "Recordings appear here after you press Record.") {}
        }
        return
    }
    items(recordings) { file ->
        MenuRow(file.name, "%.1f MB".format(file.length() / 1_000_000.0)) {
            Text("›", color = Goggles.textDim, fontSize = 15.sp)
        }
    }
}

private fun androidx.compose.foundation.lazy.LazyListScope.moreRows(
    rail: GsRail,
    settings: Settings,
    keyStatus: String,
    importResult: String?,
    onImportKey: () -> Unit,
    onOpenDiagnostics: () -> Unit,
    onChange: (Settings) -> Unit,
) {
    settingRows(rail, settings, onChange)

    // Neither of these is part of the ported tree: the SBC takes keys over SSH
    // or on the SD card, and its log is an adb session away. On a phone in a
    // field this is the only place for either.
    item {
        MenuRow("Import gs.key", importResult ?: keyStatus, onClick = onImportKey) {
            Text("›", color = Goggles.textDim, fontSize = 15.sp)
        }
    }
    item {
        MenuRow("Diagnostics", "Crash reports and the live log.", onClick = onOpenDiagnostics) {
            Text("›", color = Goggles.textDim, fontSize = 15.sp)
        }
    }
    item { Spacer(Modifier.height(12.dp)) }
}
