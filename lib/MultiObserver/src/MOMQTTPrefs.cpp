#include "MOMQTTPrefs.h"

#include <FS.h>
#include <SPIFFS.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>

namespace {
constexpr uint32_t kMagic = 0x4D4F4D51;  // MOMQ
constexpr char kFilename[] = "/mo_mqtt_prefs";

struct PersistedBroker {
  uint8_t state;
  uint16_t port;
  uint8_t transport;
  char host[129];
  char username[65];
  char password[129];
};

struct PersistedMqtt {
  uint32_t magic;
  PersistedBroker mqtt1;
  PersistedBroker mqtt2;
  char iata[8];
};

void setDefaults(PersistedMqtt& persisted) {
  std::memset(&persisted, 0, sizeof(persisted));
  persisted.magic = kMagic;
  persisted.mqtt1.port = 1883;
  persisted.mqtt2.port = 1883;
}

bool readPersisted(PersistedMqtt& persisted) {
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

bool writePersisted(const PersistedMqtt& persisted) {
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

void copyBroker(const MOBrokerPrefs& source, PersistedBroker& destination) {
  destination.state = static_cast<uint8_t>(source.state);
  destination.port = source.port;
  destination.transport = static_cast<uint8_t>(source.transport);
  copyString(destination.host, sizeof(destination.host), source.host);
  copyString(destination.username, sizeof(destination.username),
             source.username);
  copyString(destination.password, sizeof(destination.password),
             source.password);
}

void restoreBroker(const PersistedBroker& source, MOBrokerPrefs& destination) {
  destination.state =
      source.state == static_cast<uint8_t>(MOOnOff::On) ? MOOnOff::On
                                                        : MOOnOff::Off;
  destination.port = source.port == 0 ? 1883 : source.port;
  destination.transport =
      source.transport == static_cast<uint8_t>(MOTransport::Wss)
          ? MOTransport::Wss
          : MOTransport::Tcp;
  destination.host = source.host;
  destination.username = source.username;
  destination.password = source.password;
}
}  // namespace

void MOMQTTPrefs::defaults() {
  mqtt1_ = {};
  mqtt2_ = {};
  mqtt1_.port = 1883;
  mqtt2_.port = 1883;
  mqtt1_.transport = MOTransport::Tcp;
  mqtt2_.transport = MOTransport::Tcp;
  iata_.clear();
}

bool MOMQTTPrefs::load() {
  defaults();

  PersistedMqtt persisted{};
  if (!readPersisted(persisted)) {
    return false;
  }

  restoreBroker(persisted.mqtt1, mqtt1_);
  restoreBroker(persisted.mqtt2, mqtt2_);
  iata_ = persisted.iata;
  return true;
}

bool MOMQTTPrefs::save() const {
  PersistedMqtt persisted{};
  setDefaults(persisted);
  copyBroker(mqtt1_, persisted.mqtt1);
  copyBroker(mqtt2_, persisted.mqtt2);
  copyString(persisted.iata, sizeof(persisted.iata), iata_);
  return writePersisted(persisted);
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

void MOMQTTPrefs::setIata(std::string value) {
  iata_ = std::move(value);
}
