#pragma once

#include <ctime>

#ifndef MULTIOBSERVER_TZ
#define MULTIOBSERVER_TZ "CET-1CEST,M3.5.0,M10.5.0/3"
#endif

namespace MOLocalTime {

constexpr time_t kMinSaneEpoch = 1735689600;  // 2025-01-01T00:00:00Z

// Configure only the human-readable local-time representation. Unix epochs,
// MeshCore packet timestamps and MQTT timestamps remain UTC.
void configure() noexcept;
const char* timezone() noexcept;
bool now(struct tm& local) noexcept;

}  // namespace MOLocalTime
