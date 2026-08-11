// SPDX-License-Identifier: GPL-3.0-only
package org.openipc.mobilegs

import android.Manifest
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.PackageManager
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import org.openipc.gslib.SourceKind
import org.openipc.mobilegs.service.GroundStationService
import org.openipc.mobilegs.settings.Settings
import org.openipc.mobilegs.settings.SettingsRepository
import org.openipc.mobilegs.ui.GroundStationApp
import java.io.File

/**
 * The single activity. It binds the service, drives the USB permission flow,
 * and hands both to Compose.
 */
class MainActivity : ComponentActivity() {

    private lateinit var settingsRepository: SettingsRepository
    private val service = MutableStateFlow<GroundStationService?>(null)

    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            service.value = (binder as? GroundStationService.LocalBinder)?.service
            // A plugged-in adapter at launch means the user almost certainly
            // wants to fly, so try to bring the link up without a further tap.
            maybeAutoStart()
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            service.value = null
        }
    }

    private val notificationPermission =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        settingsRepository = SettingsRepository(applicationContext)

        // A ground station that blanks mid-flight is useless.
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
        }

        // Only bound here. The service is started - and promotes itself to the
        // foreground - once there is actually a link to keep alive; starting a
        // connectedDevice foreground service at launch, holding none of the
        // prerequisites the platform requires from Android 14, is refused with
        // an exception.
        bindService(
            Intent(this, GroundStationService::class.java),
            connection,
            Context.BIND_AUTO_CREATE,
        )

        setContent {
            GroundStationApp(
                serviceFlow = service,
                settingsRepository = settingsRepository,
                onStartRequested = { settings -> startLink(settings) },
                onStopRequested = { service.value?.stopGroundStation() },
                keyFile = gsKeyFile(),
            )
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        // Delivered when a supported adapter is plugged in while we are running.
        if (intent.action == UsbManager.ACTION_USB_DEVICE_ATTACHED) {
            maybeAutoStart()
        }
    }

    override fun onDestroy() {
        runCatching { unbindService(connection) }
        super.onDestroy()
    }

    private fun gsKeyFile(): File = File(filesDir, "gs.key")

    private fun maybeAutoStart() {
        lifecycleScope.launch {
            val settings = settingsRepository.settings.first()
            val active = service.value ?: return@launch
            if (active.running.value) return@launch
            if (settings.source == SourceKind.UDP || gsKeyFile().exists()) {
                startLink(settings)
            }
        }
    }

    /**
     * Bring the link up. For a wfb link this needs a USB adapter and the user's
     * consent to use it; APFPV needs neither.
     */
    private fun startLink(settings: Settings) {
        val active = service.value ?: return

        if (settings.source == SourceKind.UDP) {
            GroundStationService.start(this)
            active.startGroundStation(settings, usbFd = -1, keyPath = gsKeyFile().absolutePath)
            return
        }

        val usb = org.openipc.mobilegs.usb.UsbAdapterManager(this)
        val device = usb.findSupportedDevice()
        if (device == null) {
            active.onStatus("no supported Wi-Fi adapter found - plug in an RTL8812AU or 8812EU")
            return
        }

        usb.requestPermission(device) { granted ->
            if (!granted) {
                active.onStatus("USB permission denied, so the adapter cannot be opened")
                return@requestPermission
            }
            val fd = usb.openFileDescriptor(device)
            if (fd == null) {
                active.onStatus("could not open the adapter")
                return@requestPermission
            }
            // Now that USB permission is granted the platform will accept a
            // connectedDevice foreground service.
            GroundStationService.start(this)
            active.startGroundStation(settings, fd, gsKeyFile().absolutePath)
        }
    }
}
