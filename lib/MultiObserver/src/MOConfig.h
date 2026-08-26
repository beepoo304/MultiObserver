#pragma once

#include "MOMQTTPrefs.h"
#include "MOEtap2Prefs.h"
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

  [[nodiscard]] const MOEtap2Prefs& etap2() const noexcept;
  [[nodiscard]] MOEtap2Prefs& etap2() noexcept;

 private:
  MOWifiPrefs wifi_;
  MOMQTTPrefs mqtt_;
  MOEtap2Prefs etap2_;
};
