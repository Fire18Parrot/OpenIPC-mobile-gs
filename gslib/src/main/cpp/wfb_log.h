// SPDX-License-Identifier: GPL-3.0-only
//
// Injected into wfb-ng's sources through PREINCLUDE_FILE so its logging and IPC
// macros land in logcat instead of stdout, which nothing reads on Android.

#pragma once

#include <android/log.h>

#define WFB_ERR(...) __android_log_print(ANDROID_LOG_ERROR, "wfb-ng", __VA_ARGS__)
#define WFB_INFO(...) __android_log_print(ANDROID_LOG_INFO, "wfb-ng", __VA_ARGS__)
#define WFB_DBG(...) ((void)0)

// wfb-ng writes its statistics as text lines on stdout for wfb-cli to scrape.
// We read the counters directly out of the Aggregator instead, so these only
// need to go somewhere harmless.
#define ANDROID_IPC_MSG(...) __android_log_print(ANDROID_LOG_DEBUG, "wfb-ng", __VA_ARGS__)
#define IPC_MSG(...) ((void)0)
#define IPC_MSG_SEND() ((void)0)
