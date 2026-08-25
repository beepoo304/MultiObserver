#include "MOWifiPrefs.h"

#include <FS.h>
#include <SPIFFS.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>

namespace {
constexpr uint32_t kMagic = 0x4D4F5746;  // MOWF
constexpr char kFilename[] = "/mo_wifi_prefs";

struct PersistedWifi {
  uint32_t magic;
  char ssid[65];
  char password[129];
};

bool readPersisted(PersistedWifi& persisted) {
  if (!SPIFFS.exists(kFilename)) {
    return false;
  }

  File file = SPIFFS.open(kFilename, "r");
  if (!file) {
    return false;
  }

  const size_t size = std::min(static_cast<size_t>(file.size()),
                               sizeof(persisted));
  const bool ok = size >= sizeof(persisted.magic) &&
                  file.read(reinterpret_cast<uint8_t*>(&persisted), size) ==
                      size;
  file.close();

  return ok && persisted.magic == kMagic;
}

bool writePersisted(const PersistedWifi& persisted) {
  if (SPIFFS.exists(kFilename) && !SPIFFS.remove(kFilename)) {
    return false;
  }

  File file = SPIFFS.open(kFilename, "w", true);
  if (!file) {
    return false;
  }

  const bool ok = file.write(
                      reinterpret_cast<const uint8_t*>(&persisted),
                      sizeof(persisted)) == sizeof(persisted);
  file.close();
  return ok;
}

void copyString(char* destination, size_t capacity, const std::string& value) {
  if (capacity == 0) {
    return;
  }

  const size_t length = std::min(value.size(), capacity - 1);
  std::memcpy(destination, value.data(), length);
  destination[length] = '\0';
}
}  // namespace

void MOWifiPrefs::defaults() {
  ssid_.clear();
  password_.clear();
}

bool MOWifiPrefs::load() {
  defaults();

  PersistedWifi persisted{};
  if (!readPersisted(persisted)) {
    return false;
  }

  ssid_ = persisted.ssid;
  password_ = persisted.password;
  return true;
}

bool MOWifiPrefs::save() const {
  PersistedWifi persisted{};
  persisted.magic = kMagic;
  copyString(persisted.ssid, sizeof(persisted.ssid), ssid_);
  copyString(persisted.password, sizeof(persisted.password), password_);
  return writePersisted(persisted);
}

const std::string& MOWifiPrefs::ssid() const noexcept {
  return ssid_;
}

const std::string& MOWifiPrefs::password() const noexcept {
  return password_;
}

void MOWifiPrefs::setSsid(std::string value) {
  ssid_ = std::move(value);
}

void MOWifiPrefs::setPassword(std::string value) {
  password_ = std::move(value);
}
