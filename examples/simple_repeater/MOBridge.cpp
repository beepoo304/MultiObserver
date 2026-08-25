#include "MOBridge.h"

void MOBridge::begin() {
  observer_.begin();
}

void MOBridge::loop() {
  if (!observer_.isStarted()) {
    return;
  }

  observer_.loop();
}

bool MOBridge::isStarted() const noexcept {
  return observer_.isStarted();
}
