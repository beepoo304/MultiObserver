#include "MOLocalTime.h"

#include <cstdlib>

namespace MOLocalTime {

void configure() noexcept {
  setenv("TZ", MULTIOBSERVER_TZ, 1);
  tzset();
}

const char* timezone() noexcept { return MULTIOBSERVER_TZ; }

bool now(struct tm& local) noexcept {
  const time_t current = time(nullptr);
  return current >= kMinSaneEpoch && localtime_r(&current, &local) != nullptr;
}

}  // namespace MOLocalTime
