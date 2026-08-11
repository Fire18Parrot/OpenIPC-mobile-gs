// SPDX-License-Identifier: GPL-3.0-only
package org.openipc.gslib

/**
 * wfb-ng key file handling.
 *
 * These live on their own singleton rather than in [NativeGroundStation]'s
 * companion because a `@JvmStatic external` function in a companion object does
 * not produce the JNI symbol you would expect: the native method is declared on
 * the `Companion` class, so it resolves as
 * `Java_..._NativeGroundStation_00024Companion_...` while `@JvmStatic` only adds
 * a forwarder on the outer class. A plain `object` keeps the symbol name
 * predictable.
 */
object NativeKeys {

    init {
        System.loadLibrary("openipc_gs")
    }

    /**
     * Generate a fresh key pair. `gs.key` stays on the phone; `drone.key` must
     * be copied to the air unit, which is the only way the two ends can agree
     * on a session.
     */
    fun generateKeyPair(gsPath: String, dronePath: String): Boolean =
        nativeGenerateKeyPair(gsPath, dronePath)

    /** Returns null when the key file is usable, or a reason when it is not. */
    fun validateKey(path: String): String? =
        nativeValidateKey(path).takeIf { it.isNotEmpty() }

    private external fun nativeGenerateKeyPair(gsPath: String, dronePath: String): Boolean

    private external fun nativeValidateKey(path: String): String
}
