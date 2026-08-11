// SPDX-License-Identifier: GPL-3.0-only
package org.openipc.mobilegs.usb

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.os.Build
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * Finds a supported Wi-Fi adapter, asks the user for permission, and opens it.
 *
 * An unprivileged Android app may never open a USB device itself. The framework
 * opens it after the user consents and hands back a file descriptor, which is
 * what devourer adopts through `libusb_wrap_sys_device`. That is the whole
 * reason no root and no custom kernel is needed - unlike the SBC ground
 * station, which loads an out-of-tree rtl8812au module.
 */
class UsbAdapterManager(private val context: Context) {

    /** Realtek's vendor id; every adapter devourer supports is one of theirs. */
    private val supportedVendorId = 0x0bda

    private val supportedProductIds = setOf(
        // Jaguar1: RTL8812AU / 8811AU / 8821AU / 8814AU
        0x8812, 0x0811, 0xa811, 0xb811, 0x8813,
        // Jaguar2: RTL8822BU
        0xb812, 0xb82c,
        // Jaguar3: RTL8822CU / 8812CU / 8812EU / 8822EU
        0xc82c, 0xc82e, 0xc812, 0x881a, 0x881b, 0x881c, 0xa81a, 0xe822, 0xa82a,
    )

    private val usbManager: UsbManager =
        context.getSystemService(Context.USB_SERVICE) as UsbManager

    private val _attachedDevice = MutableStateFlow<UsbDevice?>(null)
    val attachedDevice: StateFlow<UsbDevice?> = _attachedDevice

    private var permissionReceiver: BroadcastReceiver? = null
    private var openConnection: UsbDeviceConnection? = null

    fun findSupportedDevice(): UsbDevice? =
        usbManager.deviceList.values.firstOrNull { it.isSupported() }
            .also { _attachedDevice.value = it }

    fun hasPermission(device: UsbDevice): Boolean = usbManager.hasPermission(device)

    /**
     * Ask for permission. [onResult] fires with the user's answer; if it is
     * already granted it fires immediately.
     */
    fun requestPermission(device: UsbDevice, onResult: (Boolean) -> Unit) {
        if (usbManager.hasPermission(device)) {
            onResult(true)
            return
        }

        unregisterReceiver()
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(receiverContext: Context, intent: Intent) {
                if (intent.action != ACTION_USB_PERMISSION) return
                val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                unregisterReceiver()
                onResult(granted)
            }
        }
        permissionReceiver = receiver

        val filter = IntentFilter(ACTION_USB_PERMISSION)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            context.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("UnspecifiedRegisterReceiverFlag")
            context.registerReceiver(receiver, filter)
        }

        // FLAG_MUTABLE is required: the framework fills in the device and the
        // grant result before delivering the intent back to us.
        val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            PendingIntent.FLAG_MUTABLE
        } else {
            0
        }
        val pendingIntent = PendingIntent.getBroadcast(
            context,
            0,
            Intent(ACTION_USB_PERMISSION).setPackage(context.packageName),
            flags,
        )
        usbManager.requestPermission(device, pendingIntent)
    }

    /**
     * Open the device and return its file descriptor, or null if it could not
     * be opened. The connection is retained here and released by [close], since
     * the descriptor stays valid only while the connection is open.
     */
    fun openFileDescriptor(device: UsbDevice): Int? {
        close()
        val connection = usbManager.openDevice(device) ?: return null
        openConnection = connection
        return connection.fileDescriptor.takeIf { it >= 0 }
    }

    fun close() {
        openConnection?.close()
        openConnection = null
    }

    private fun unregisterReceiver() {
        permissionReceiver?.let {
            runCatching { context.unregisterReceiver(it) }
            permissionReceiver = null
        }
    }

    private fun UsbDevice.isSupported(): Boolean {
        if (vendorId == supportedVendorId && productId in supportedProductIds) {
            return true
        }
        // Rebadged adapters enumerate under an OEM vendor id, so fall back to
        // the descriptor shape: a vendor-specific interface with bulk endpoints
        // is what devourer actually needs.
        return (0 until interfaceCount).any { index ->
            getInterface(index).interfaceClass == UsbConstants.USB_CLASS_VENDOR_SPEC
        } && vendorId == supportedVendorId
    }

    companion object {
        private const val ACTION_USB_PERMISSION = "org.openipc.mobilegs.USB_PERMISSION"
    }
}
