// SPDX-License-Identifier: GPL-3.0-only
package org.openipc.mobilegs.ui.gsmenu

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import java.io.File
import org.openipc.mobilegs.settings.Settings

/**
 * The in-goggle menu, drawn the way the SBC ground station draws it: a column
 * of sections on the left, the selected section's items on the right, over a
 * dimmed video background.
 *
 * Tapping an item cycles it to its next value, which is how the SBC menu
 * behaves under a two-button goggle input. Items the phone cannot serve are
 * still listed, greyed, with the reason - dropping them would make this a
 * different menu from the one a pilot already knows.
 */
@Composable
fun GsMenuScreen(
    settings: Settings,
    keyStatus: String,
    onChange: (Settings) -> Unit,
    onOpenDiagnostics: () -> Unit,
    onDismiss: () -> Unit,
) {
    val context = LocalContext.current
    var importResult by remember { mutableStateOf<String?>(null) }

    // Without a gs.key the wfb path cannot decrypt anything, and the SBC's
    // answer - drop the file on the SD card, or scp it - is not available on a
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

    var target by remember { mutableStateOf(GsMenu.targets.first().name) }
    var section by remember { mutableStateOf(GsMenu.targets.first().sections.first().name) }

    val currentTarget = GsMenu.targets.firstOrNull { it.name == target } ?: GsMenu.targets.first()
    val currentSection = currentTarget.sections.firstOrNull { it.name == section }
        ?: currentTarget.sections.first()

    Box(Modifier.fillMaxSize().background(Color(0xE005070A))) {
        Column(Modifier.fillMaxSize().padding(14.dp)) {

            // Target row: GS / Air, as upstream splits it.
            Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                GsMenu.targets.forEach { entry ->
                    val selected = entry.name == target
                    Text(
                        text = entry.title.uppercase(),
                        color = if (selected) Color(0xFFFF7A00) else Color(0xFF78909C),
                        fontFamily = FontFamily.Monospace,
                        fontSize = 15.sp,
                        modifier = Modifier
                            .clickable {
                                target = entry.name
                                section = entry.sections.first().name
                            }
                            .padding(horizontal = 10.dp, vertical = 6.dp),
                    )
                }
                Box(Modifier.weight(1f))
                TextButton(onClick = onDismiss) { Text("Close") }
            }

            Row(Modifier.fillMaxSize()) {

                // Sections.
                Column(
                    Modifier
                        .width(150.dp)
                        .fillMaxHeight()
                        .padding(end = 10.dp),
                ) {
                    currentTarget.sections.forEach { entry ->
                        val selected = entry.name == currentSection.name
                        Text(
                            text = entry.title,
                            color = if (selected) Color.White else Color(0xFF78909C),
                            fontFamily = FontFamily.Monospace,
                            fontSize = 13.sp,
                            modifier = Modifier
                                .fillMaxWidth()
                                .background(
                                    if (selected) Color(0x33FF7A00) else Color.Transparent,
                                )
                                .clickable { section = entry.name }
                                .padding(8.dp),
                        )
                    }

                    Box(Modifier.weight(1f))
                    // Neither of these is part of the ported tree. The SBC has
                    // no equivalent - it takes keys over SSH or on the SD card,
                    // and a log is an adb session away - but on a phone in a
                    // field this is the only place for either.
                    Text(
                        text = keyStatus,
                        color = if (keyStatus.startsWith("gs.key loaded")) {
                            Color(0xFF4CAF50)
                        } else {
                            Color(0xFFFFC107)
                        },
                        fontFamily = FontFamily.Monospace,
                        fontSize = 9.sp,
                        modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
                    )
                    TextButton(onClick = { importKey.launch(arrayOf("*/*")) }) {
                        Text("Import gs.key")
                    }
                    importResult?.let { message ->
                        Text(
                            text = message,
                            color = if (message == "gs.key imported") {
                                Color(0xFF4CAF50)
                            } else {
                                Color(0xFFFF5252)
                            },
                            fontFamily = FontFamily.Monospace,
                            fontSize = 9.sp,
                            modifier = Modifier.padding(horizontal = 8.dp),
                        )
                    }
                    TextButton(onClick = onOpenDiagnostics) { Text("Diagnostics") }
                }

                // Items in the selected section.
                LazyColumn(Modifier.fillMaxSize()) {
                    items(currentSection.items) { item ->
                        GsMenuRow(
                            item = item,
                            settings = settings,
                            onChange = onChange,
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun GsMenuRow(
    item: GsMenuItem,
    settings: Settings,
    onChange: (Settings) -> Unit,
) {
    val current = GsMenuBinding.read(item.path, settings)
    val options = GsMenuBinding.options(item)
    val editable = item.supported &&
        current != null &&
        GsMenuBinding.write(item.path, current, settings) != null &&
        options.isNotEmpty()

    val labelColour = if (item.supported) Color.White else Color(0xFF546E7A)
    val valueColour = when {
        !item.supported -> Color(0xFF546E7A)
        current == null -> Color(0xFF546E7A)
        else -> Color(0xFFFF7A00)
    }

    Column(
        Modifier
            .fillMaxWidth()
            .clickable(enabled = editable) {
                val index = options.indexOf(current)
                val next = options[(index + 1).mod(options.size)]
                GsMenuBinding.write(item.path, next, settings)?.let(onChange)
            }
            .padding(horizontal = 10.dp, vertical = 7.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                text = item.label,
                color = labelColour,
                fontFamily = FontFamily.Monospace,
                fontSize = 13.sp,
                modifier = Modifier.weight(1f),
            )
            Text(
                text = current ?: "-",
                color = valueColour,
                fontFamily = FontFamily.Monospace,
                fontSize = 13.sp,
            )
        }
        val note = when {
            !item.supported && item.help.isNotEmpty() -> item.help
            !item.supported -> "not available on a phone"
            item.help.isNotEmpty() -> item.help
            else -> ""
        }
        if (note.isNotEmpty()) {
            Text(
                text = note,
                color = Color(0xFF607D8B),
                fontFamily = FontFamily.Monospace,
                fontSize = 9.sp,
            )
        }
        // The upstream path, so anything here can be matched against gsmenu.sh.
        Text(
            text = item.path,
            color = Color(0xFF37474F),
            fontFamily = FontFamily.Monospace,
            fontSize = 8.sp,
        )
    }
}
