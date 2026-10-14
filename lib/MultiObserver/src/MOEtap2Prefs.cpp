#include "MOEtap2Prefs.h"

#include <FS.h>
#include <SPIFFS.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace {
constexpr uint32_t kPrefsMagic = 0x4D4F4532;  // MOE2
constexpr uint32_t kRebootMagic = 0x4D4F5754; // MOWT + broker mask
constexpr char kPrefsFilename[] = "/mo_etap2_prefs";
constexpr char kRebootFilename[] = "/mo_wdg_reboot";
constexpr char kObsoleteWdgLogFilename[] = "/mo_wdg_log";
constexpr char kRemoteCliFilename[] = "/mo_rcli_on";
constexpr uint32_t kRemoteCliMagic = 0x4D4F5243; // MORC

struct PersistedPrefs {
  uint32_t magic;
  uint8_t watchdogEnabled;
  uint16_t graceSeconds;
  uint8_t channelEnabled;
  char channelKey[MOEtap2Prefs::kChannelKeyMaxHexLength + 1];
};

bool validKey(std::string_view key) {
  if (key.empty()) return true;
  if (key.size() != MOEtap2Prefs::kChannelKeyHexLength &&
      key.size() != MOEtap2Prefs::kChannelKeyMaxHexLength) return false;
  return std::all_of(key.begin(), key.end(), [](unsigned char c) {
    return std::isxdigit(c) != 0;
  });
}

bool readExact(const char* filename, void* buffer, size_t length) {
  if (!SPIFFS.exists(filename)) return false;
  File file = SPIFFS.open(filename, "r");
  if (!file) return false;
  const bool ok = file.size() == static_cast<int>(length) &&
                  file.read(static_cast<uint8_t*>(buffer), length) == length;
  file.close();
  return ok;
}

bool writeExact(const char* filename, const void* buffer, size_t length) {
  if (SPIFFS.exists(filename) && !SPIFFS.remove(filename)) return false;
  File file = SPIFFS.open(filename, "w", true);
  if (!file) return false;
  const bool ok = file.write(static_cast<const uint8_t*>(buffer), length) == length;
  file.close();
  return ok;
}

void copyKey(char* destination, size_t capacity, std::string_view value) {
  const size_t length = std::min(capacity - 1, value.size());
  std::memcpy(destination, value.data(), length);
  destination[length] = '\0';
}
}  // namespace

void MOEtap2Prefs::defaults() {
  watchdogEnabled_ = true;
  graceSeconds_ = kDefaultGraceSeconds;
  channelEnabled_ = false;
  channelKey_.clear();
  remoteCliEnabled_ = false;
}

bool MOEtap2Prefs::load() {
  defaults();
  if (SPIFFS.exists(kObsoleteWdgLogFilename)) SPIFFS.remove(kObsoleteWdgLogFilename);
  PersistedPrefs persisted{};
  if (!readExact(kPrefsFilename, &persisted, sizeof(persisted)) ||
      persisted.magic != kPrefsMagic) {
    return false;
  }

  watchdogEnabled_ = persisted.watchdogEnabled != 0;
  graceSeconds_ = persisted.graceSeconds;
  if (graceSeconds_ < kMinGraceSeconds || graceSeconds_ > kMaxGraceSeconds) {
    graceSeconds_ = kDefaultGraceSeconds;
  }
  channelEnabled_ = persisted.channelEnabled != 0;
  channelKey_ = persisted.channelKey;
  if (!validKey(channelKey_)) channelKey_.clear();
  uint32_t remoteCliMarker = 0;
  remoteCliEnabled_ = readExact(kRemoteCliFilename, &remoteCliMarker,
                                sizeof(remoteCliMarker)) &&
                      remoteCliMarker == kRemoteCliMagic;
  return true;
}

bool MOEtap2Prefs::save() const {
  PersistedPrefs persisted{};
  persisted.magic = kPrefsMagic;
  persisted.watchdogEnabled = watchdogEnabled_ ? 1 : 0;
  persisted.graceSeconds = graceSeconds_;
  persisted.channelEnabled = channelEnabled_ ? 1 : 0;
  copyKey(persisted.channelKey, sizeof(persisted.channelKey), channelKey_);
  if (!writeExact(kPrefsFilename, &persisted, sizeof(persisted))) return false;
  if (remoteCliEnabled_) {
    return writeExact(kRemoteCliFilename, &kRemoteCliMagic,
                      sizeof(kRemoteCliMagic));
  }
  if (SPIFFS.exists(kRemoteCliFilename)) SPIFFS.remove(kRemoteCliFilename);
  return true;
}

bool MOEtap2Prefs::watchdogEnabled() const noexcept { return watchdogEnabled_; }
uint16_t MOEtap2Prefs::graceSeconds() const noexcept { return graceSeconds_; }
bool MOEtap2Prefs::channelEnabled() const noexcept { return channelEnabled_; }
const std::string& MOEtap2Prefs::channelKey() const noexcept { return channelKey_; }
bool MOEtap2Prefs::remoteCliEnabled() const noexcept { return remoteCliEnabled_; }
void MOEtap2Prefs::setWatchdogEnabled(bool enabled) noexcept { watchdogEnabled_ = enabled; }
bool MOEtap2Prefs::setGraceSeconds(uint16_t seconds) noexcept {
  if (seconds < kMinGraceSeconds || seconds > kMaxGraceSeconds) return false;
  graceSeconds_ = seconds;
  return true;
}
void MOEtap2Prefs::setChannelEnabled(bool enabled) noexcept { channelEnabled_ = enabled; }
bool MOEtap2Prefs::setChannelKey(std::string_view key) noexcept {
  if (!validKey(key)) return false;
  channelKey_.assign(key.data(), key.size());
  std::transform(channelKey_.begin(), channelKey_.end(), channelKey_.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return true;
}
void MOEtap2Prefs::setRemoteCliEnabled(bool enabled) noexcept {
  remoteCliEnabled_ = enabled;
}

bool MOEtap2Prefs::markWatchdogReboot(uint8_t brokerMask) const {
  const uint32_t marker = (kRebootMagic & 0xFFFFFFFCU) | (brokerMask & 0x03U);
  return writeExact(kRebootFilename, &marker, sizeof(marker));
}

uint8_t MOEtap2Prefs::consumeWatchdogReboot() const {
  uint32_t marker = 0;
  const bool marked = readExact(kRebootFilename, &marker, sizeof(marker)) &&
                      (marker & 0xFFFFFFFCU) == (kRebootMagic & 0xFFFFFFFCU);
  if (SPIFFS.exists(kRebootFilename)) {
    if (!SPIFFS.remove(kRebootFilename)) {
      // Removal failed: neutralize the marker so the next boot cannot
      // mistakenly re-enter post-reboot mode.
      const uint32_t zero = 0;
      (void)writeExact(kRebootFilename, &zero, sizeof(zero));
    }
  }
  return marked ? static_cast<uint8_t>(marker & 0x03U) : 0;
}