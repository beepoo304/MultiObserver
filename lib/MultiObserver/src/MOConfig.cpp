#include "MOConfig.h"

MOConfig::MOConfig() {
  defaults();
}

void MOConfig::defaults() {
  wifi_.defaults();
  mqtt_.defaults();
}

bool MOConfig::load() {
  defaults();
  const bool wifi_ok = wifi_.load();
  const bool mqtt_ok = mqtt_.load();
  return wifi_ok && mqtt_ok;
}

bool MOConfig::save() {
  return wifi_.save() && mqtt_.save();
}

const MOWifiPrefs& MOConfig::wifi() const noexcept {
  return wifi_;
}

MOWifiPrefs& MOConfig::wifi() noexcept {
  return wifi_;
}

const MOMQTTPrefs& MOConfig::mqtt() const noexcept {
  return mqtt_;
}

MOMQTTPrefs& MOConfig::mqtt() noexcept {
  return mqtt_;
}
