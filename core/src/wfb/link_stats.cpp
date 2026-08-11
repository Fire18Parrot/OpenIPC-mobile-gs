// SPDX-License-Identifier: GPL-3.0-only

#include "openipc/link_stats.h"

#include <sys/time.h>

namespace openipc::gs {

void LinkStats::Publish(const LinkSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_ = snapshot;
}

LinkSnapshot LinkStats::Get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

uint64_t NowMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000ULL + static_cast<uint64_t>(tv.tv_usec) / 1000ULL;
}

}  // namespace openipc::gs
