// SPDX-License-Identifier: GPL-3.0-only
//
// The JNI surface. Deliberately narrow: one handle, a start/stop pair, a few
// setters, and one listener interface. Everything else lives in C++.
//
// Video is delivered as a direct ByteBuffer over the native NAL buffer, valid
// only for the duration of the callback. The Kotlin side copies it straight
// into a MediaCodec input buffer, so no allocation happens per frame.

#include <jni.h>

#include <memory>
#include <string>
#include <vector>

#include "ground_station.h"

namespace {

using openipc::gs::AlinkConfig;
using openipc::gs::Bandwidth;
using openipc::gs::GroundStation;
using openipc::gs::GroundStationCallbacks;
using openipc::gs::GsConfig;
using openipc::gs::LinkSnapshot;
using openipc::gs::MavlinkEndpoint;
using openipc::gs::OsdFrame;
using openipc::gs::SourceKind;
using openipc::gs::TelemetryState;
using openipc::gs::VideoCodec;

JavaVM* g_vm = nullptr;

// Index constants for the stats arrays. Mirrored by NativeStats in Kotlin;
// keep the two in step.
enum StatsInt {
    kStatBestRssi = 0,
    kStatBestSnr,
    kStatPacketsAll,
    kStatPacketsLost,
    kStatPacketsRecovered,
    kStatPacketsBad,
    kStatPacketsDecryptErr,
    kStatFecK,
    kStatFecN,
    kStatAntennas,
    kStatSessionEstablished,
    kStatBytesAll,
    kStatArmed,
    kStatGpsFix,
    kStatSatellites,
    kStatDetectedCodec,
    kStatIntCount,
};

enum StatsFloat {
    kStatRoll = 0,
    kStatPitch,
    kStatYaw,
    kStatLatitude,
    kStatLongitude,
    kStatAltitude,
    kStatRelativeAltitude,
    kStatGroundSpeed,
    kStatAirSpeed,
    kStatClimb,
    kStatHeading,
    kStatThrottle,
    kStatBatteryVoltage,
    kStatBatteryCurrent,
    kStatBatteryRemaining,
    kStatFloatCount,
};

// Holds the handle plus the JNI plumbing needed to call back into Kotlin from
// the native worker threads.
struct Session {
    GroundStation station;
    jobject listener = nullptr;
    jmethodID on_video = nullptr;
    jmethodID on_osd = nullptr;
    jmethodID on_stats = nullptr;
    jmethodID on_status = nullptr;
};

Session* AsSession(jlong handle) { return reinterpret_cast<Session*>(handle); }

// Attaches the calling native thread to the JVM for as long as it is in scope.
class ScopedEnv {
public:
    ScopedEnv() {
        if (g_vm == nullptr) {
            return;
        }
        const jint status = g_vm->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6);
        if (status == JNI_EDETACHED) {
            if (g_vm->AttachCurrentThread(&env_, nullptr) == JNI_OK) {
                attached_ = true;
            } else {
                env_ = nullptr;
            }
        } else if (status != JNI_OK) {
            env_ = nullptr;
        }
    }

    ~ScopedEnv() {
        if (attached_ && g_vm != nullptr) {
            g_vm->DetachCurrentThread();
        }
    }

    JNIEnv* get() const { return env_; }

private:
    JNIEnv* env_ = nullptr;
    bool attached_ = false;
};

std::string ToStdString(JNIEnv* env, jstring value) {
    if (value == nullptr) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string result = chars != nullptr ? chars : "";
    if (chars != nullptr) {
        env->ReleaseStringUTFChars(value, chars);
    }
    return result;
}

Bandwidth ToBandwidth(jint value) {
    switch (value) {
        case 5:
            return Bandwidth::k5;
        case 10:
            return Bandwidth::k10;
        case 40:
            return Bandwidth::k40;
        case 80:
            return Bandwidth::k80;
        default:
            return Bandwidth::k20;
    }
}

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    return JNI_VERSION_1_6;
}

JNIEXPORT jlong JNICALL Java_org_openipc_gslib_NativeGroundStation_nativeCreate(JNIEnv*, jobject) {
    return reinterpret_cast<jlong>(new Session());
}

JNIEXPORT void JNICALL Java_org_openipc_gslib_NativeGroundStation_nativeDestroy(JNIEnv* env,
                                                                               jobject,
                                                                               jlong handle) {
    Session* session = AsSession(handle);
    if (session == nullptr) {
        return;
    }
    session->station.Stop();
    if (session->listener != nullptr) {
        env->DeleteGlobalRef(session->listener);
    }
    delete session;
}

JNIEXPORT jboolean JNICALL Java_org_openipc_gslib_NativeGroundStation_nativeStart(
    JNIEnv* env, jobject, jlong handle, jint source_kind, jint channel, jint bandwidth,
    jint link_id, jstring key_path, jstring udp_bind, jint udp_port, jstring mirror_host,
    jint mirror_port, jint usb_fd, jobject listener) {
    Session* session = AsSession(handle);
    if (session == nullptr) {
        return JNI_FALSE;
    }

    if (session->listener != nullptr) {
        env->DeleteGlobalRef(session->listener);
    }
    session->listener = env->NewGlobalRef(listener);

    jclass listener_class = env->GetObjectClass(listener);
    session->on_video =
        env->GetMethodID(listener_class, "onVideoNal", "(Ljava/nio/ByteBuffer;I)V");
    session->on_osd = env->GetMethodID(listener_class, "onOsdFrame", "(II[I[I)V");
    session->on_stats = env->GetMethodID(listener_class, "onStats", "([I[F)V");
    session->on_status = env->GetMethodID(listener_class, "onStatus", "(Ljava/lang/String;)V");

    GsConfig config;
    config.source = source_kind == 1 ? SourceKind::kUdp : SourceKind::kDevourer;
    config.radio.channel = channel;
    config.radio.bandwidth = ToBandwidth(bandwidth);
    config.radio.link_id = static_cast<uint32_t>(link_id);
    config.radio.key_path = ToStdString(env, key_path);
    config.udp.bind_addr = ToStdString(env, udp_bind);
    config.udp.video_port = udp_port;
    config.video_mirror_host = ToStdString(env, mirror_host);
    config.video_mirror_port = mirror_port;

    GroundStationCallbacks callbacks;

    callbacks.on_video_nal = [session](const uint8_t* data, size_t size) {
        ScopedEnv scoped;
        JNIEnv* env = scoped.get();
        if (env == nullptr || session->on_video == nullptr) {
            return;
        }
        // The buffer wraps the native NAL directly - no copy on this path.
        jobject buffer = env->NewDirectByteBuffer(const_cast<uint8_t*>(data), static_cast<jlong>(size));
        if (buffer != nullptr) {
            env->CallVoidMethod(session->listener, session->on_video, buffer,
                                static_cast<jint>(size));
            env->DeleteLocalRef(buffer);
        }
    };

    callbacks.on_osd_frame = [session](const OsdFrame& frame) {
        ScopedEnv scoped;
        JNIEnv* env = scoped.get();
        if (env == nullptr || session->on_osd == nullptr) {
            return;
        }
        const jsize count = static_cast<jsize>(frame.cells.size());
        jintArray glyphs = env->NewIntArray(count);
        jintArray attributes = env->NewIntArray(count);
        if (glyphs == nullptr || attributes == nullptr) {
            return;
        }
        std::vector<jint> glyph_values(count);
        std::vector<jint> attribute_values(count);
        for (jsize i = 0; i < count; ++i) {
            glyph_values[i] = frame.cells[i].glyph;
            attribute_values[i] = frame.cells[i].attributes;
        }
        env->SetIntArrayRegion(glyphs, 0, count, glyph_values.data());
        env->SetIntArrayRegion(attributes, 0, count, attribute_values.data());
        env->CallVoidMethod(session->listener, session->on_osd, frame.rows, frame.cols, glyphs,
                            attributes);
        env->DeleteLocalRef(glyphs);
        env->DeleteLocalRef(attributes);
    };

    callbacks.on_stats = [session](const LinkSnapshot& snapshot, const TelemetryState& telemetry) {
        ScopedEnv scoped;
        JNIEnv* env = scoped.get();
        if (env == nullptr || session->on_stats == nullptr) {
            return;
        }
        jint ints[kStatIntCount] = {};
        ints[kStatBestRssi] = snapshot.best_rssi;
        ints[kStatBestSnr] = snapshot.best_snr;
        ints[kStatPacketsAll] = static_cast<jint>(snapshot.packets_all);
        ints[kStatPacketsLost] = static_cast<jint>(snapshot.packets_lost);
        ints[kStatPacketsRecovered] = static_cast<jint>(snapshot.packets_fec_recovered);
        ints[kStatPacketsBad] = static_cast<jint>(snapshot.packets_bad);
        ints[kStatPacketsDecryptErr] = static_cast<jint>(snapshot.packets_decrypt_err);
        ints[kStatFecK] = snapshot.fec_k;
        ints[kStatFecN] = snapshot.fec_n;
        ints[kStatAntennas] = snapshot.num_antennas;
        ints[kStatSessionEstablished] = snapshot.session_established ? 1 : 0;
        ints[kStatBytesAll] = static_cast<jint>(snapshot.bytes_all);
        ints[kStatArmed] = telemetry.armed ? 1 : 0;
        ints[kStatGpsFix] = telemetry.gps_fix_type;
        ints[kStatSatellites] = telemetry.satellites;
        switch (session->station.detected_codec()) {
            case VideoCodec::kH264:
                ints[kStatDetectedCodec] = 1;
                break;
            case VideoCodec::kH265:
                ints[kStatDetectedCodec] = 2;
                break;
            default:
                ints[kStatDetectedCodec] = 0;
                break;
        }

        jfloat floats[kStatFloatCount] = {};
        floats[kStatRoll] = telemetry.roll_rad;
        floats[kStatPitch] = telemetry.pitch_rad;
        floats[kStatYaw] = telemetry.yaw_rad;
        floats[kStatLatitude] = telemetry.latitude_e7 / 1e7f;
        floats[kStatLongitude] = telemetry.longitude_e7 / 1e7f;
        floats[kStatAltitude] = telemetry.altitude_mm / 1000.0f;
        floats[kStatRelativeAltitude] = telemetry.relative_altitude_mm / 1000.0f;
        floats[kStatGroundSpeed] = telemetry.ground_speed_ms;
        floats[kStatAirSpeed] = telemetry.air_speed_ms;
        floats[kStatClimb] = telemetry.climb_ms;
        floats[kStatHeading] = telemetry.heading_deg;
        floats[kStatThrottle] = static_cast<jfloat>(telemetry.throttle_pct);
        floats[kStatBatteryVoltage] = telemetry.battery_voltage_v;
        floats[kStatBatteryCurrent] = telemetry.battery_current_a;
        floats[kStatBatteryRemaining] = static_cast<jfloat>(telemetry.battery_remaining_pct);

        jintArray int_array = env->NewIntArray(kStatIntCount);
        jfloatArray float_array = env->NewFloatArray(kStatFloatCount);
        if (int_array == nullptr || float_array == nullptr) {
            return;
        }
        env->SetIntArrayRegion(int_array, 0, kStatIntCount, ints);
        env->SetFloatArrayRegion(float_array, 0, kStatFloatCount, floats);
        env->CallVoidMethod(session->listener, session->on_stats, int_array, float_array);
        env->DeleteLocalRef(int_array);
        env->DeleteLocalRef(float_array);
    };

    callbacks.on_status = [session](const std::string& message) {
        ScopedEnv scoped;
        JNIEnv* env = scoped.get();
        if (env == nullptr || session->on_status == nullptr) {
            return;
        }
        jstring text = env->NewStringUTF(message.c_str());
        env->CallVoidMethod(session->listener, session->on_status, text);
        env->DeleteLocalRef(text);
    };

    return session->station.Start(config, usb_fd, std::move(callbacks)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_openipc_gslib_NativeGroundStation_nativeStop(JNIEnv*, jobject,
                                                                            jlong handle) {
    Session* session = AsSession(handle);
    if (session != nullptr) {
        session->station.Stop();
    }
}

JNIEXPORT jstring JNICALL Java_org_openipc_gslib_NativeGroundStation_nativeLastError(
    JNIEnv* env, jobject, jlong handle) {
    Session* session = AsSession(handle);
    return env->NewStringUTF(session != nullptr ? session->station.last_error().c_str() : "");
}

JNIEXPORT jboolean JNICALL Java_org_openipc_gslib_NativeGroundStation_nativeSetChannel(
    JNIEnv*, jobject, jlong handle, jint channel, jint bandwidth) {
    Session* session = AsSession(handle);
    if (session == nullptr) {
        return JNI_FALSE;
    }
    return session->station.SetChannel(channel, ToBandwidth(bandwidth)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_openipc_gslib_NativeGroundStation_nativeSetVideoCodec(
    JNIEnv*, jobject, jlong handle, jint codec) {
    Session* session = AsSession(handle);
    if (session == nullptr) {
        return;
    }
    VideoCodec value = VideoCodec::kUnknown;
    if (codec == 1) {
        value = VideoCodec::kH264;
    } else if (codec == 2) {
        value = VideoCodec::kH265;
    }
    session->station.SetVideoCodec(value);
}

// Endpoints arrive as parallel arrays so the bridge stays free of per-field
// object marshalling.
JNIEXPORT void JNICALL Java_org_openipc_gslib_NativeGroundStation_nativeSetMavlinkEndpoints(
    JNIEnv* env, jobject, jlong handle, jintArray kinds, jobjectArray hosts, jintArray ports,
    jbooleanArray uplink_flags) {
    Session* session = AsSession(handle);
    if (session == nullptr) {
        return;
    }
    const jsize count = env->GetArrayLength(kinds);
    std::vector<jint> kind_values(count);
    std::vector<jint> port_values(count);
    std::vector<jboolean> uplink_values(count);
    env->GetIntArrayRegion(kinds, 0, count, kind_values.data());
    env->GetIntArrayRegion(ports, 0, count, port_values.data());
    env->GetBooleanArrayRegion(uplink_flags, 0, count, uplink_values.data());

    std::vector<MavlinkEndpoint> endpoints;
    endpoints.reserve(static_cast<size_t>(count));
    for (jsize i = 0; i < count; ++i) {
        MavlinkEndpoint endpoint;
        switch (kind_values[i]) {
            case 1:
                endpoint.kind = MavlinkEndpoint::Kind::kUdpServer;
                break;
            case 2:
                endpoint.kind = MavlinkEndpoint::Kind::kTcpServer;
                break;
            default:
                endpoint.kind = MavlinkEndpoint::Kind::kUdpOut;
                break;
        }
        auto host = reinterpret_cast<jstring>(env->GetObjectArrayElement(hosts, i));
        endpoint.host = ToStdString(env, host);
        env->DeleteLocalRef(host);
        endpoint.port = port_values[i];
        endpoint.allow_uplink = uplink_values[i] == JNI_TRUE;
        endpoint.enabled = true;
        endpoints.push_back(endpoint);
    }
    session->station.SetMavlinkEndpoints(endpoints);
}

JNIEXPORT void JNICALL Java_org_openipc_gslib_NativeGroundStation_nativeSetAlink(
    JNIEnv* env, jobject, jlong handle, jboolean enabled, jstring host, jint port, jint interval_ms,
    jfloat snr_weight, jfloat rssi_weight, jboolean allow_idr, jboolean allow_penalty,
    jboolean allow_fec_increase) {
    Session* session = AsSession(handle);
    if (session == nullptr) {
        return;
    }
    AlinkConfig config;
    config.enabled = enabled == JNI_TRUE;
    config.udp_host = ToStdString(env, host);
    config.udp_port = port;
    config.interval_ms = interval_ms;
    config.snr_weight = snr_weight;
    config.rssi_weight = rssi_weight;
    config.allow_idr = allow_idr == JNI_TRUE;
    config.allow_penalty = allow_penalty == JNI_TRUE;
    config.allow_fec_increase = allow_fec_increase == JNI_TRUE;
    session->station.SetAlinkConfig(config);
}

JNIEXPORT jboolean JNICALL Java_org_openipc_gslib_NativeKeys_nativeGenerateKeyPair(
    JNIEnv* env, jobject, jstring gs_path, jstring drone_path) {
    std::string error;
    return openipc::gs::GenerateKeyPair(ToStdString(env, gs_path), ToStdString(env, drone_path),
                                        &error)
               ? JNI_TRUE
               : JNI_FALSE;
}

JNIEXPORT jstring JNICALL Java_org_openipc_gslib_NativeKeys_nativeValidateKey(
    JNIEnv* env, jobject, jstring path) {
    const openipc::gs::KeyValidation validation =
        openipc::gs::ValidateGsKey(ToStdString(env, path));
    return env->NewStringUTF(validation.ok ? "" : validation.error.c_str());
}

}  // extern "C"
