// SPDX-License-Identifier: GPL-3.0-only
package org.openipc.mobilegs.diag

import android.content.Context
import android.util.Log
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * An in-app log, so a problem in the field can be read on the phone instead of
 * needing a laptop and adb.
 *
 * Two things are kept: a bounded in-memory buffer the UI observes live, and an
 * append-only file that survives a crash and a restart. Both are deliberately
 * cheap - a pilot debugging a dead link should not be paying for the logging.
 */
object DiagnosticsLog {

    private const val TAG = "openipc-diag"
    private const val MAX_LINES = 500
    private const val MAX_FILE_BYTES = 256 * 1024L

    private val timestamp = SimpleDateFormat("HH:mm:ss.SSS", Locale.US)

    private val _lines = MutableStateFlow<List<String>>(emptyList())

    /** The live log, newest last. */
    val lines: StateFlow<List<String>> = _lines

    private var logFile: File? = null

    fun install(context: Context) {
        val directory = File(context.filesDir, "diagnostics")
        directory.mkdirs()
        logFile = File(directory, "session.log")
        // Start each run with a bounded file rather than growing forever.
        logFile?.let { file ->
            if (file.exists() && file.length() > MAX_FILE_BYTES) {
                runCatching { file.delete() }
            }
        }
        append("--- session started ---")
    }

    fun append(message: String) {
        val line = "${timestamp.format(Date())}  $message"
        Log.i(TAG, message)

        val current = _lines.value
        _lines.value = if (current.size >= MAX_LINES) {
            current.drop(current.size - MAX_LINES + 1) + line
        } else {
            current + line
        }

        // Best effort: never let logging take the app down.
        runCatching {
            logFile?.appendText(line + "\n")
        }
    }

    fun clear() {
        _lines.value = emptyList()
        runCatching { logFile?.writeText("") }
    }

    /** The full text a user can share or copy. */
    fun snapshot(): String = _lines.value.joinToString("\n")
}
