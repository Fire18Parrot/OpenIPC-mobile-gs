// SPDX-License-Identifier: GPL-3.0-only
package org.openipc.mobilegs.video

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong
import org.openipc.gslib.VideoCodec

/**
 * Hardware video decode straight onto a Surface.
 *
 * This is the app's answer to the SBC ground station's Rockchip MPP player. The
 * priorities are the same as any FPV link: never buffer, never reorder, and get
 * each NAL to the display as soon as it exists. Frames are released with
 * `releaseOutputBuffer(index, true)` immediately rather than being scheduled
 * against a presentation timestamp, because for a live feed the only sensible
 * moment to show a frame is now.
 */
class VideoDecoder {

    private var codec: MediaCodec? = null
    private var surface: Surface? = null
    private var configuredCodec: VideoCodec = VideoCodec.AUTO

    private val running = AtomicBoolean(false)
    private val framesDecoded = AtomicLong(0)
    private val framesDropped = AtomicLong(0)

    /** Set once the first keyframe has been fed; before that, output is noise. */
    private var sawKeyframe = false

    var onFirstFrame: (() -> Unit)? = null
    var onError: ((String) -> Unit)? = null

    /**
     * The stream's real pixel dimensions, once the decoder reports them. The
     * view has to be sized from this: a Surface that simply fills the screen
     * stretches a 16:9 stream across a 20:9 phone, and everything on screen
     * ends up subtly wrong.
     */
    var onVideoSize: ((Int, Int) -> Unit)? = null

    val decodedFrames: Long get() = framesDecoded.get()
    val droppedFrames: Long get() = framesDropped.get()
    val isRunning: Boolean get() = running.get()

    @Synchronized
    fun start(surface: Surface, codecType: VideoCodec, width: Int = 1920, height: Int = 1080) {
        stop()
        this.surface = surface
        configuredCodec = codecType

        val mime = when (codecType) {
            VideoCodec.H265 -> MediaFormat.MIMETYPE_VIDEO_HEVC
            // AUTO starts as H.264 and is reconfigured if the stream turns out
            // to be H.265; the depacketiser reports which it detected.
            else -> MediaFormat.MIMETYPE_VIDEO_AVC
        }

        val format = MediaFormat.createVideoFormat(mime, width, height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                // The documented switch: tell the decoder not to hold frames
                // back for reordering. Worth several frames of latency.
                setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            }
            // Qualcomm's vendor key predates KEY_LOW_LATENCY and is still what
            // most Snapdragon phones actually honour. Setting an unknown key is
            // ignored, so both can be set safely.
            setInteger("vendor.qti-ext-dec-low-latency.enable", 1)
            setInteger("vendor.qti-ext-dec-picture-order.enable", 0)

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                setInteger(MediaFormat.KEY_PRIORITY, 0) // realtime
            }
        }

        try {
            val created = MediaCodec.createDecoderByType(mime)
            created.configure(format, surface, null, 0)
            created.start()
            codec = created
            running.set(true)
            sawKeyframe = false
            Log.i(TAG, "decoder started: $mime")
        } catch (e: Exception) {
            Log.e(TAG, "could not start the decoder", e)
            onError?.invoke("could not start the $mime decoder: ${e.message}")
            codec = null
            running.set(false)
        }
    }

    @Synchronized
    fun stop() {
        codec?.let {
            runCatching { it.stop() }
            runCatching { it.release() }
        }
        codec = null
        running.set(false)
        sawKeyframe = false
    }

    /**
     * Feed one Annex-B NAL unit.
     *
     * Called from the native receive thread. The buffer is a direct view of
     * native memory valid only for this call, so it is copied into the codec's
     * input buffer here and not retained.
     */
    fun submitNal(buffer: ByteBuffer, size: Int) {
        val active = codec ?: return
        if (!running.get()) return

        // Wait for a keyframe before feeding anything: starting mid-GOP makes
        // most decoders emit a screen of garbage before they resynchronise.
        if (!sawKeyframe) {
            if (!isKeyframe(buffer, size)) {
                return
            }
            sawKeyframe = true
        }

        try {
            val inputIndex = active.dequeueInputBuffer(INPUT_TIMEOUT_US)
            if (inputIndex < 0) {
                // No input buffer free: the decoder is behind. Dropping now is
                // better than blocking the receive thread.
                framesDropped.incrementAndGet()
                return
            }
            val input = active.getInputBuffer(inputIndex) ?: return
            input.clear()
            if (input.remaining() < size) {
                framesDropped.incrementAndGet()
                active.queueInputBuffer(inputIndex, 0, 0, 0, 0)
                return
            }
            val slice = buffer.duplicate()
            slice.limit(size)
            input.put(slice)

            active.queueInputBuffer(inputIndex, 0, size, System.nanoTime() / 1000, 0)
            drainOutput(active)
        } catch (e: IllegalStateException) {
            Log.e(TAG, "decoder rejected input", e)
            onError?.invoke("the decoder failed: ${e.message}")
            running.set(false)
        }
    }

    private fun drainOutput(active: MediaCodec) {
        val info = MediaCodec.BufferInfo()
        while (true) {
            val index = active.dequeueOutputBuffer(info, 0)
            when {
                index >= 0 -> {
                    // `true` renders to the surface at once; for a live feed
                    // there is nothing to be gained by scheduling it later.
                    active.releaseOutputBuffer(index, true)
                    if (framesDecoded.incrementAndGet() == 1L) {
                        onFirstFrame?.invoke()
                    }
                }

                index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                    val format = active.outputFormat
                    Log.i(TAG, "output format: $format")
                    runCatching {
                        val width = format.getInteger(MediaFormat.KEY_WIDTH)
                        val height = format.getInteger(MediaFormat.KEY_HEIGHT)
                        if (width > 0 && height > 0) onVideoSize?.invoke(width, height)
                    }
                }

                else -> return
            }
        }
    }

    /**
     * Recognise an IDR so playback starts on a clean picture. The NAL type sits
     * in different bits in the two codecs, and the start code may be three or
     * four bytes.
     */
    private fun isKeyframe(buffer: ByteBuffer, size: Int): Boolean {
        val view = buffer.duplicate()
        if (size < 5) return false

        var offset = 0
        // Skip the Annex-B start code.
        if (view.get(0).toInt() == 0 && view.get(1).toInt() == 0) {
            offset = when {
                view.get(2).toInt() == 1 -> 3
                size > 4 && view.get(2).toInt() == 0 && view.get(3).toInt() == 1 -> 4
                else -> return false
            }
        }
        if (offset >= size) return false

        val first = view.get(offset).toInt() and 0xff
        return when (configuredCodec) {
            VideoCodec.H265 -> {
                // Types 16..21 are the IRAP range: BLA, IDR and CRA.
                val type = (first shr 1) and 0x3f
                type in 16..21 || type == 32 || type == 33 || type == 34 // plus VPS/SPS/PPS
            }

            else -> {
                val type = first and 0x1f
                type == 5 || type == 7 || type == 8 // IDR, SPS, PPS
            }
        }
    }

    companion object {
        private const val TAG = "openipc-decoder"
        private const val INPUT_TIMEOUT_US = 10_000L
    }
}
