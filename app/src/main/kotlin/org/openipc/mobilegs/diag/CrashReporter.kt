// SPDX-License-Identifier: GPL-3.0-only
package org.openipc.mobilegs.diag

import android.content.Context
import android.os.Build
import android.os.Process
import java.io.File
import java.io.PrintWriter
import java.io.StringWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Captures a crash to a file on the device so it can be read, copied or shared
 * from inside the app.
 *
 * An app may always read its own logcat, so the report carries the last of that
 * too - which is where the native side's messages land, and those are the ones
 * that matter when an adapter fails to come up.
 */
object CrashReporter {

    private const val REPORT_NAME = "last-crash.txt"
    private const val LOGCAT_LINES = 400

    private lateinit var appContext: Context

    fun install(context: Context) {
        appContext = context.applicationContext
        val previous = Thread.getDefaultUncaughtExceptionHandler()

        Thread.setDefaultUncaughtExceptionHandler { thread, error ->
            // Everything here is best effort: a failure while reporting a crash
            // must not replace the original crash with a less useful one.
            runCatching { writeReport(thread, error) }
            // Chain to the platform handler so the process still dies properly
            // and the system records it as it normally would.
            previous?.uncaughtException(thread, error)
        }
    }

    private fun reportFile(context: Context): File =
        File(File(context.filesDir, "diagnostics").apply { mkdirs() }, REPORT_NAME)

    private fun writeReport(thread: Thread, error: Throwable) {
        val stack = StringWriter().also { error.printStackTrace(PrintWriter(it)) }.toString()
        val when_ = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date())

        val report = buildString {
            appendLine("OpenIPC Mobile GS crash report")
            appendLine("time    : $when_")
            appendLine("thread  : ${thread.name}")
            appendLine(deviceSummary())
            appendLine()
            appendLine("--- stack trace ---")
            appendLine(stack)
            appendLine("--- app log ---")
            appendLine(DiagnosticsLog.snapshot())
            appendLine()
            appendLine("--- logcat ---")
            appendLine(captureLogcat())
        }
        reportFile(appContext).writeText(report)
    }

    fun deviceSummary(): String = buildString {
        appendLine("device  : ${Build.MANUFACTURER} ${Build.MODEL}")
        appendLine("android : ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})")
        append("abis    : ${Build.SUPPORTED_ABIS.joinToString()}")
    }

    /**
     * Read this process's own logcat. No permission is needed for an app to
     * read its own output, and it is the only way to see what the native layer
     * printed.
     */
    fun captureLogcat(): String = runCatching {
        val command = arrayOf(
            "logcat", "-d", "-v", "time", "-t", LOGCAT_LINES.toString(),
            "--pid=${Process.myPid()}",
        )
        Runtime.getRuntime().exec(command).inputStream.bufferedReader().use { it.readText() }
    }.recoverCatching {
        // --pid is not honoured everywhere; fall back to an unfiltered tail.
        Runtime.getRuntime()
            .exec(arrayOf("logcat", "-d", "-v", "time", "-t", LOGCAT_LINES.toString()))
            .inputStream.bufferedReader().use { it.readText() }
    }.getOrElse { "logcat unavailable: ${it.message}" }

    /** The report from the previous run, if the app died last time. */
    fun lastCrash(context: Context): String? {
        val file = reportFile(context)
        return if (file.exists() && file.length() > 0) file.readText() else null
    }

    fun clear(context: Context) {
        runCatching { reportFile(context).delete() }
    }
}
