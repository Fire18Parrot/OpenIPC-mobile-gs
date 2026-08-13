// SPDX-License-Identifier: GPL-3.0-only
package org.openipc.mobilegs.net

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.util.Log

/**
 * Pins the app's sockets to the Wi-Fi network, for APFPV.
 *
 * An air unit's access point has no internet behind it, and modern Android
 * treats that as a fault rather than a choice: it marks the network unvalidated,
 * keeps the default route on mobile data, and will drop the association
 * entirely if nothing objects. The symptom is a UDP socket that binds fine and
 * then receives nothing, or a link that works for a minute and dies - because
 * the datagrams are being sent to a cellular interface that has never heard of
 * the drone.
 *
 * Requesting a network with the internet capability *removed* is what tells the
 * platform this is deliberate. Holding that request keeps the association up,
 * and binding the process to it puts our sockets on the right interface.
 *
 * The binding is process-wide, so it is taken only while an APFPV link is up
 * and dropped as soon as it stops - a MAVLink endpoint pointed at a GCS on the
 * normal network would otherwise become unreachable.
 */
class WifiLink(private val context: Context) {

    private val manager: ConnectivityManager? =
        context.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager

    private var callback: ConnectivityManager.NetworkCallback? = null

    val isBound: Boolean get() = callback != null

    /**
     * Ask for the Wi-Fi network and route this process over it. [onStatus] is
     * called when the network arrives or goes away, so the flight view can say
     * which it was rather than leaving the user with a silent black screen.
     */
    fun bind(onStatus: (String) -> Unit) {
        val cm = manager ?: return
        if (callback != null) return

        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            // The whole point: an air unit AP routes nowhere, and a request that
            // insists on internet would never be satisfied by it.
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .build()

        val cb = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                val bound = runCatching { cm.bindProcessToNetwork(network) }.getOrDefault(false)
                onStatus(
                    if (bound) {
                        "using the Wi-Fi network for video"
                    } else {
                        "could not pin video to Wi-Fi - it may go out over mobile data"
                    },
                )
            }

            override fun onLost(network: Network) {
                onStatus("Wi-Fi went away - rejoin the air unit's network")
            }

            override fun onUnavailable() {
                onStatus("no Wi-Fi network available for APFPV")
            }
        }

        try {
            cm.requestNetwork(request, cb)
            callback = cb
        } catch (e: SecurityException) {
            // CHANGE_NETWORK_STATE is declared, but a restricted profile or an
            // OEM policy can still refuse. Not fatal: on many phones the
            // default route is the Wi-Fi anyway and video arrives regardless.
            Log.w(TAG, "could not request the Wi-Fi network: ${e.message}")
            onStatus("could not pin video to Wi-Fi: ${e.message}")
        }
    }

    fun release() {
        val cm = manager ?: return
        callback?.let {
            runCatching { cm.unregisterNetworkCallback(it) }
            callback = null
        }
        runCatching { cm.bindProcessToNetwork(null) }
    }

    private companion object {
        const val TAG = "openipc-wifi"
    }
}
