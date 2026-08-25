#include "MO.h"

void MO::begin() {
  if (started_) {
    return;
  }

  started_ = true;
}

void MO::loop() {
  if (!started_) {
    return;
  }
}

bool MO::isStarted() const noexcept {
  return started_;
}
