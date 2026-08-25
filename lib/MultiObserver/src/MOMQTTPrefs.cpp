#include "MOMQTTPrefs.h"

#include <LittleFS.h>

namespace {

constexpr char kMagic[] = "MOMQTT1";

void writeField(File& file, const std::string& value) {
  file.println(value.c_str());
}

bool readField(File& file, std::string& value) {
  if (!file.available()) {
    return false;
  }

  String line = file.readStringUntil('\n');
  line.trim();
  value = line.c_str();
  return true;
}

bool writeBroker(File& file, const MOBrokerPrefs& broker) {
  file.println(static_cast<int>(broker.state));
  file.println(broker.port);
  writeField(file, broker.host);
  writeField(file, broker.username);
  writeField(file, broker.password);
  writeField(file, broker.clientId);
  return file.getWriteError() == 0;
}

bool readBroker(File& file, MOBrokerPrefs& broker) {
  if (!file.available()) {
    return false;
  }

  String state = file.readStringUntil('\n');
  state.trim();
  broker.state = state.toInt() == 1 ? MOOnOff::On : MOOnOff::Off;

  if (!file.available()) {
    return false;
  }

  String port = file.readStringUntil('\n');
  port.trim();
  const long parsedPort = port.toInt();
  if (parsedPort < 1 || parsedPort > 65535) {
    return false;
  }
  broker.port = static_cast<uint16_t>(parsedPort);

  return readField(file, broker.host) &&
         readField(file, broker.username) &&
         readField(file, broker.password) &&
         readField(file, broker.clientId);
}

}  // namespace

void MOMQTTPrefs::defaults() {
  mqtt1_ = {};
  mqtt2_ = {};
  mqtt1_.port = 1883;
  mqtt2_.port = 1883;
  iata_.clear();
}

bool MOMQTTPrefs::load() {
  defaults();

  if (!LittleFS.exists(kStoragePath)) {
    return false;
  }

  File file = LittleFS.open(kStoragePath, "r");
  if (!file) {
    return false;
  }

  String magic = file.readStringUntil('\n');
  magic.trim();

  if (magic != kMagic) {
    file.close();
    return false;
  }

  String version = file.readStringUntil('\n');
  version.trim();

  if (version.toInt() != static_cast<int>(kFormatVersion)) {
    file.close();
    return false;
  }

  const bool ok = readBroker(file, mqtt1_) &&
                  readBroker(file, mqtt2_) &&
                  readField(file, iata_);

  file.close();
  if (!ok) {
    defaults();
  }

  return ok;
}

bool MOMQTTPrefs::save() const {
  if (!LittleFS.exists("/multiobserver")) {
    if (!LittleFS.mkdir("/multiobserver")) {
      return false;
    }
  }

  File file = LittleFS.open(kStoragePath, "w");
  if (!file) {
    return false;
  }

  file.println(kMagic);
  file.println(kFormatVersion);

  const bool ok = writeBroker(file, mqtt1_) &&
                  writeBroker(file, mqtt2_);

  if (ok) {
    writeField(file, iata_);
  }

  const bool result = ok && file.getWriteError() == 0;
  file.close();
  return result;
}

const MOBrokerPrefs& MOMQTTPrefs::mqtt1() const noexcept {
  return mqtt1_;
}

MOBrokerPrefs& MOMQTTPrefs::mqtt1() noexcept {
  return mqtt1_;
}

const MOBrokerPrefs& MOMQTTPrefs::mqtt2() const noexcept {
  return mqtt2_;
}

MOBrokerPrefs& MOMQTTPrefs::mqtt2() noexcept {
  return mqtt2_;
}

const std::string& MOMQTTPrefs::iata() const noexcept {
  return iata_;
}

void MOMQTTPrefs::setIata(const std::string& value) {
  iata_ = value;
}
