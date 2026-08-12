// SPDX-License-Identifier: GPL-3.0-only
package org.openipc.mobilegs.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import org.openipc.gslib.GroundStationListener
import org.openipc.gslib.LinkStats
import org.openipc.gslib.NativeGroundStation
import org.openipc.gslib.OsdScreen
import org.openipc.gslib.SourceKind
import org.openipc.gslib.Telemetry
import org.openipc.gslib.VideoCodec
import org.openipc.mobilegs.MainActivity
import org.openipc.mobilegs.R
import org.openipc.mobilegs.diag.DiagnosticsLog
import org.openipc.mobilegs.dvr.VideoRecorder
import org.openipc.mobilegs.settings.Settings
import org.openipc.mobilegs.video.VideoDecoder

/**
 * Owns the ground station for the app's lifetime.
 *
 * On the SBC this role belongs to a set of init scripts; here it is a
 * foreground service, so receiving survives the screen going off and the
 * pipeline stops cleanly when the user is done. The `connectedDevice`
 * foreground type is the one Android expects for USB peripherals.
 */
class GroundStationService : Service(), GroundStationListener {

    inner class LocalBinder : Binder() {
        val service: GroundStationService get() = this@GroundStationService
    }

    private val binder = LocalBinder()

    private var station: NativeGroundStation? = null
    private val decoder = VideoDecoder()
    private val recorder = VideoRecorder()
    private var wakeLock: PowerManager.WakeLock? = null
    private var surface: Surface? = null
    private var pendingCodec: VideoCodec = VideoCodec.AUTO

    /** What the decoder is currently configured for, as opposed to requested. */
    private var activeCodec: VideoCodec = VideoCodec.AUTO
    private var isForeground = false

    private val _linkStats = MutableStateFlow(LinkStats())
    val linkStats: StateFlow<LinkStats> = _linkStats

    private val _telemetry = MutableStateFlow(Telemetry())
    val telemetry: StateFlow<Telemetry> = _telemetry

    private val _osd = MutableStateFlow<OsdScreen?>(null)
    val osd: StateFlow<OsdScreen?> = _osd

    private val _status = MutableStateFlow("idle")
    val status: StateFlow<String> = _status

    private val _running = MutableStateFlow(false)
    val running: StateFlow<Boolean> = _running

    /** The stream's pixel size, so the view can keep its aspect ratio. */
    private val _videoSize = MutableStateFlow(16 to 9)
    val videoSize: StateFlow<Pair<Int, Int>> = _videoSize

    /** Decoded frames per second, for the OSD's video widget. */
    private val _videoFps = MutableStateFlow(0)
    val videoFps: StateFlow<Int> = _videoFps

    private var lastFrameCount = 0L
    private var lastFpsAtMs = 0L

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
        decoder.onVideoSize = { width, height ->
            _videoSize.value = width to height
            DiagnosticsLog.append("video is ${width}x$height")
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // Deliberately not promoted to the foreground here. From Android 14 a
        // connectedDevice foreground service may only start while the app holds
        // a qualifying prerequisite - USB device permission, or one of the
        // network/Bluetooth permissions - and at this point we may hold none of
        // them. Promotion happens in startGroundStation(), by which time the
        // user has granted access to the adapter (or we are in APFPV mode,
        // covered by the Wi-Fi state permissions).
        return START_STICKY
    }

    /**
     * Promote to a foreground service so receiving survives the screen going
     * off. Refusal is not fatal: the ground station keeps running as an ordinary
     * bound service for as long as the activity is up, which is worth far more
     * to the user than a crash.
     */
    private fun promoteToForeground(text: String) {
        try {
            startForeground(NOTIFICATION_ID, buildNotification(text))
            isForeground = true
        } catch (e: Exception) {
            Log.w(TAG, "could not run in the foreground: ${e.message}")
            DiagnosticsLog.append("foreground refused: ${e.message}")
            isForeground = false
        }
    }

    override fun onDestroy() {
        stopGroundStation()
        super.onDestroy()
    }

    /**
     * Attach the surface video is decoded onto. Called when the SurfaceView
     * becomes available, which may be before or after the link starts.
     */
    fun attachSurface(surface: Surface?, codec: VideoCodec) {
        this.surface = surface
        pendingCodec = codec
        if (surface != null && _running.value) {
            val effective = _linkStats.value.detectedCodec.takeIf { it != VideoCodec.AUTO }
                ?: codec
            activeCodec = effective
            decoder.start(surface, effective)
        } else if (surface == null) {
            decoder.stop()
        }
    }

    fun startGroundStation(settings: Settings, usbFd: Int, keyPath: String): Boolean {
        if (_running.value) return true

        val instance = NativeGroundStation()
        station = instance

        instance.setMavlinkEndpoints(settings.toMavlinkEndpoints())
        instance.setAlink(settings.toAlinkConfig())
        instance.setVideoCodec(settings.codec)

        val started = instance.start(
            settings.toGroundStationConfig(keyPath),
            usbFd,
            this,
        )

        if (!started) {
            DiagnosticsLog.append("start failed: ${instance.lastError}")
            _status.value = instance.lastError.ifEmpty { "could not start the ground station" }
            instance.close()
            station = null
            return false
        }

        _running.value = true
        acquireWakeLock()
        surface?.let {
            activeCodec = settings.codec
            decoder.start(it, settings.codec)
        }
        if (settings.recordVideo) {
            recorder.start(this, settings.codec)
        }
        promoteToForeground("Receiving")
        return true
    }

    fun stopGroundStation() {
        decoder.stop()
        recorder.stop()
        station?.stop()
        station?.close()
        station = null
        _running.value = false
        releaseWakeLock()
        if (isForeground) {
            @Suppress("DEPRECATION")
            stopForeground(true)
            isForeground = false
        }
        _status.value = "stopped"
    }

    fun setChannel(channel: Int, bandwidthMhz: Int) {
        station?.setChannel(
            channel,
            org.openipc.gslib.Bandwidth.entries.firstOrNull { it.mhz == bandwidthMhz }
                ?: org.openipc.gslib.Bandwidth.BW_20,
        )
    }

    fun applySettings(settings: Settings) {
        station?.setMavlinkEndpoints(settings.toMavlinkEndpoints())
        station?.setAlink(settings.toAlinkConfig())
        station?.setVideoCodec(settings.codec)
    }

    val isRecording: Boolean get() = recorder.isRecording

    fun toggleRecording(codec: VideoCodec): Boolean {
        return if (recorder.isRecording) {
            recorder.stop()
            false
        } else {
            recorder.start(this, codec)
        }
    }

    // --- GroundStationListener, all called on native threads ---------------

    override fun onVideoNal(buffer: ByteBuffer, size: Int) {
        decoder.submitNal(buffer, size)
        // The recorder needs its own read of the buffer, since the decoder
        // consumed the position of the copy it was handed.
        if (recorder.isRecording) {
            recorder.write(buffer.duplicate(), size)
        }
    }

    override fun onOsdScreen(screen: OsdScreen) {
        _osd.value = screen
    }

    override fun onStats(stats: LinkStats, telemetry: Telemetry) {
        _linkStats.value = stats
        _telemetry.value = telemetry

        // The decoder cannot infer the codec: fed an H.265 stream it never sees
        // a keyframe, so it stays silent while data pours in. The depacketiser
        // knows what is actually on the wire, so follow it.
        val detected = stats.detectedCodec
        if (detected != VideoCodec.AUTO && detected != activeCodec) {
            val target = surface
            if (target != null) {
                DiagnosticsLog.append("video is $detected - restarting the decoder")
                activeCodec = detected
                decoder.start(target, detected)
            }
        }

        // Stats arrive every 100 ms; frames are counted over a whole second so
        // the number on screen is steady enough to read in flight.
        val now = System.currentTimeMillis()
        if (lastFpsAtMs == 0L) {
            lastFpsAtMs = now
            lastFrameCount = decoder.decodedFrames
        } else if (now - lastFpsAtMs >= 1000) {
            val frames = decoder.decodedFrames
            _videoFps.value = (frames - lastFrameCount).toInt()
            lastFrameCount = frames
            lastFpsAtMs = now
        }
    }

    override fun onStatus(message: String) {
        Log.i(TAG, message)
        DiagnosticsLog.append(message)
        _status.value = message
    }

    // --- notification and wake lock ---------------------------------------

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val channel = NotificationChannel(
            CHANNEL_ID,
            getString(R.string.channel_name),
            NotificationManager.IMPORTANCE_LOW,
        ).apply {
            description = getString(R.string.channel_description)
            setShowBadge(false)
        }
        (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager)
            .createNotificationChannel(channel)
    }

    private fun buildNotification(text: String): Notification {
        val intent = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val builder = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Notification.Builder(this, CHANNEL_ID)
        } else {
            @Suppress("DEPRECATION")
            Notification.Builder(this)
        }
        return builder
            .setContentTitle(getString(R.string.notification_title))
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setContentIntent(intent)
            .setOngoing(true)
            .build()
    }

    private fun updateNotification(text: String) {
        if (!isForeground) return
        (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager)
            .notify(NOTIFICATION_ID, buildNotification(text))
    }

    private fun acquireWakeLock() {
        if (wakeLock != null) return
        val power = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "openipc:groundstation")
            .also { it.acquire() }
    }

    private fun releaseWakeLock() {
        wakeLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
    }

    companion object {
        private const val TAG = "openipc-service"
        private const val CHANNEL_ID = "ground_station"
        private const val NOTIFICATION_ID = 1

        /**
         * Start the service so it outlives the activity that bound it. Plain
         * startService, not startForegroundService: the service promotes itself
         * once the link is up and it holds a prerequisite the platform accepts.
         * Called from a user-visible action, so background-start limits do not
         * apply.
         */
        fun start(context: Context) {
            context.startService(Intent(context, GroundStationService::class.java))
        }
    }
}
