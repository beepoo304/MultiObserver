#pragma once

#include "MO.h"

class MOBridge {
 public:
  void begin();
  void loop();

  [[nodiscard]] bool isStarted() const noexcept;

 private:
  MO observer_;
};
