#pragma once

#include "MOMQTTPrefs.h"
#include "MOWifiPrefs.h"

class MOConfig {
 public:
  MOConfig();

  void defaults();
  bool load();
  bool save();

  [[nodiscard]] const MOWifiPrefs& wifi() const noexcept;
  [[nodiscard]] MOWifiPrefs& wifi() noexcept;

  [[nodiscard]] const MOMQTTPrefs& mqtt() const noexcept;
  [[nodiscard]] MOMQTTPrefs& mqtt() noexcept;

 private:
  MOWifiPrefs wifi_;
  MOMQTTPrefs mqtt_;
};
