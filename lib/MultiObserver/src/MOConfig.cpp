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

  const bool wifiOk = wifi_.load();
  const bool mqttOk = mqtt_.load();

  return wifiOk || mqttOk;
}

bool MOConfig::save() {
  const bool wifiOk = wifi_.save();
  const bool mqttOk = mqtt_.save();

  return wifiOk && mqttOk;
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
