// SPDX-License-Identifier: GPL-3.0-only
package org.openipc.mobilegs.ui

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.openipc.mobilegs.diag.CrashReporter
import org.openipc.mobilegs.diag.DiagnosticsLog

/**
 * Reads the app's own log on the device.
 *
 * A ground station is used in a field, not at a desk, so anything that can only
 * be diagnosed over adb effectively cannot be reported at all. The crash report
 * from the previous run is shown first, because that is what the user came here
 * for after the app died.
 */
@Composable
fun DiagnosticsScreen(onDismiss: () -> Unit) {
    val context = LocalContext.current
    val lines by DiagnosticsLog.lines.collectAsState()
    var crash by remember { mutableStateOf(CrashReporter.lastCrash(context)) }
    var showLogcat by remember { mutableStateOf(false) }
    var logcat by remember { mutableStateOf("") }

    val listState = rememberLazyListState()
    LaunchedEffect(lines.size) {
        if (lines.isNotEmpty()) {
            listState.animateScrollToItem(lines.size - 1)
        }
    }

    Box(Modifier.fillMaxSize().background(Color(0xF007090B))) {
        Column(Modifier.fillMaxSize().padding(16.dp)) {
            Text("Diagnostics", color = Color.White)

            crash?.let { report ->
                Card(Modifier.fillMaxWidth().padding(vertical = 8.dp)) {
                    Column(Modifier.padding(10.dp)) {
                        Text("The app crashed last time it ran", color = Color(0xFFFF5252))
                        Text(
                            text = report.lineSequence().take(12).joinToString("\n"),
                            color = Color.White,
                            fontFamily = FontFamily.Monospace,
                            fontSize = 10.sp,
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(top = 6.dp)
                                .verticalScroll(rememberScrollState()),
                        )
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            TextButton(onClick = { copyToClipboard(context, report) }) {
                                Text("Copy report")
                            }
                            TextButton(onClick = { share(context, report) }) { Text("Share") }
                            TextButton(onClick = {
                                CrashReporter.clear(context)
                                crash = null
                            }) { Text("Dismiss") }
                        }
                    }
                }
            }

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                TextButton(onClick = { copyToClipboard(context, DiagnosticsLog.snapshot()) }) {
                    Text("Copy log")
                }
                TextButton(onClick = { share(context, fullReport(context)) }) { Text("Share all") }
                TextButton(onClick = {
                    logcat = CrashReporter.captureLogcat()
                    showLogcat = !showLogcat
                }) { Text(if (showLogcat) "Hide logcat" else "Logcat") }
                TextButton(onClick = { DiagnosticsLog.clear() }) { Text("Clear") }
            }

            if (showLogcat) {
                Text(
                    text = logcat.takeLast(4000),
                    color = Color(0xFFB0BEC5),
                    fontFamily = FontFamily.Monospace,
                    fontSize = 9.sp,
                    modifier = Modifier
                        .fillMaxWidth()
                        .weight(1f)
                        .verticalScroll(rememberScrollState()),
                )
            } else {
                LazyColumn(state = listState, modifier = Modifier.fillMaxWidth().weight(1f)) {
                    items(lines) { line ->
                        Text(
                            text = line,
                            color = Color.White,
                            fontFamily = FontFamily.Monospace,
                            fontSize = 10.sp,
                        )
                    }
                }
            }

            Button(onClick = onDismiss, modifier = Modifier.fillMaxWidth()) { Text("Close") }
        }
    }
}

private fun fullReport(context: Context): String = buildString {
    appendLine(CrashReporter.deviceSummary())
    appendLine()
    appendLine("--- app log ---")
    appendLine(DiagnosticsLog.snapshot())
    appendLine()
    appendLine("--- logcat ---")
    appendLine(CrashReporter.captureLogcat())
}

private fun copyToClipboard(context: Context, text: String) {
    val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
    clipboard.setPrimaryClip(ClipData.newPlainText("OpenIPC diagnostics", text))
}

private fun share(context: Context, text: String) {
    // Shared as text rather than as a file attachment: no FileProvider to
    // configure, and it pastes straight into a chat or an issue.
    val intent = Intent(Intent.ACTION_SEND).apply {
        type = "text/plain"
        putExtra(Intent.EXTRA_SUBJECT, "OpenIPC Mobile GS diagnostics")
        putExtra(Intent.EXTRA_TEXT, text)
    }
    context.startActivity(Intent.createChooser(intent, "Share diagnostics"))
}
