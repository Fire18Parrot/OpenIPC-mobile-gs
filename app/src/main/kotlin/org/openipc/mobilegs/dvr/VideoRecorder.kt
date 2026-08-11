// SPDX-License-Identifier: GPL-3.0-only
package org.openipc.mobilegs.dvr

import android.content.Context
import android.os.Environment
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.atomic.AtomicBoolean
import org.openipc.gslib.VideoCodec

/**
 * The DVR, replacing the SBC ground station's dvrui.
 *
 * Recording is of the raw Annex-B elementary stream rather than a muxed
 * container. That is deliberate: the stream arrives with no reliable timestamps
 * and with gaps wherever FEC could not recover a block, and MediaMuxer rejects
 * both. Writing the bitstream verbatim means a recording is never lost to a
 * muxer error mid-flight, and the result still plays in ffmpeg/VLC and can be
 * remuxed afterwards with `ffmpeg -i flight.h265 -c copy flight.mp4`.
 */
class VideoRecorder {

    private var output: FileOutputStream? = null
    private var file: File? = null
    private val recording = AtomicBoolean(false)
    private var bytesWritten = 0L

    val isRecording: Boolean get() = recording.get()
    val currentFile: File? get() = file
    val recordedBytes: Long get() = bytesWritten

    @Synchronized
    fun start(context: Context, codec: VideoCodec): Boolean {
        if (recording.get()) return true

        val directory = recordingDirectory(context)
        if (!directory.exists() && !directory.mkdirs()) {
            Log.e(TAG, "could not create $directory")
            return false
        }

        val extension = if (codec == VideoCodec.H265) "h265" else "h264"
        val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
        val target = File(directory, "flight-$stamp.$extension")

        return try {
            output = FileOutputStream(target)
            file = target
            bytesWritten = 0
            recording.set(true)
            Log.i(TAG, "recording to $target")
            true
        } catch (e: Exception) {
            Log.e(TAG, "could not open $target", e)
            false
        }
    }

    @Synchronized
    fun stop() {
        if (!recording.getAndSet(false)) return
        runCatching { output?.flush() }
        runCatching { output?.close() }
        output = null
        Log.i(TAG, "recording stopped: $file ($bytesWritten bytes)")
    }

    /**
     * Append one NAL unit. Called from the native receive thread; the write is
     * buffered by the OS, so it does not stall the video path in practice.
     */
    fun write(buffer: ByteBuffer, size: Int) {
        if (!recording.get()) return
        val stream = output ?: return
        try {
            val bytes = ByteArray(size)
            val view = buffer.duplicate()
            view.limit(size)
            view.get(bytes)
            stream.write(bytes)
            bytesWritten += size
        } catch (e: Exception) {
            Log.e(TAG, "write failed, stopping the recording", e)
            stop()
        }
    }

    private fun recordingDirectory(context: Context): File =
        File(
            context.getExternalFilesDir(Environment.DIRECTORY_MOVIES)
                ?: context.filesDir,
            "dvr",
        )

    fun listRecordings(context: Context): List<File> =
        recordingDirectory(context).listFiles()
            ?.filter { it.isFile }
            ?.sortedByDescending { it.lastModified() }
            ?: emptyList()

    companion object {
        private const val TAG = "openipc-dvr"
    }
}
