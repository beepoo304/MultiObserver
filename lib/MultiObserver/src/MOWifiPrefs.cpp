#include "MOWifiPrefs.h"

#include <Arduino.h>
#include <LittleFS.h>

#include <cstdint>

namespace {

constexpr char kMagic[] = "MOWIFI1";

bool readLine(File& file, std::string& value) {
  String line = file.readStringUntil('\n');
  line.trim();
  value = line.c_str();
  return true;
}

}  // namespace

void MOWifiPrefs::defaults() {
  ssid_.clear();
  password_.clear();
}

bool MOWifiPrefs::load() {
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

  readLine(file, ssid_);
  readLine(file, password_);

  file.close();
  return true;
}

bool MOWifiPrefs::save() const {
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
  file.println(ssid_.c_str());
  file.println(password_.c_str());

  const bool ok = file.getWriteError() == 0;
  file.close();
  return ok;
}

const std::string& MOWifiPrefs::ssid() const noexcept {
  return ssid_;
}

const std::string& MOWifiPrefs::password() const noexcept {
  return password_;
}

void MOWifiPrefs::setSsid(const std::string& value) {
  ssid_ = value;
}

void MOWifiPrefs::setPassword(const std::string& value) {
  password_ = value;
}
