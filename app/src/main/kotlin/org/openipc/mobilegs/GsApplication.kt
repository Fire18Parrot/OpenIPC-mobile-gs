// SPDX-License-Identifier: GPL-3.0-only
package org.openipc.mobilegs

import android.app.Application
import org.openipc.mobilegs.diag.CrashReporter
import org.openipc.mobilegs.diag.DiagnosticsLog

/**
 * Exists to get diagnostics running before anything else can fail.
 *
 * A ground station is used outdoors, away from a computer, so a crash that can
 * only be diagnosed over adb is a crash the user cannot report. Installing the
 * handler here means even a failure during the first activity's onCreate is
 * captured and readable on the phone.
 */
class GsApplication : Application() {

    override fun onCreate() {
        super.onCreate()
        DiagnosticsLog.install(this)
        CrashReporter.install(this)
        DiagnosticsLog.append(CrashReporter.deviceSummary())
    }
}
