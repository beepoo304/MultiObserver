#include "MOEtap2Prefs.h"

#include <FS.h>
#include <SPIFFS.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <vector>

namespace {
constexpr uint32_t kPrefsMagic = 0x4D4F4532;  // MOE2
constexpr uint32_t kRebootMagic = 0x4D4F5752; // MOWR
constexpr char kPrefsFilename[] = "/mo_etap2_prefs";
constexpr char kRebootFilename[] = "/mo_wdg_reboot";
constexpr char kLogFilename[] = "/mo_wdg_log";

struct PersistedPrefs {
  uint32_t magic;
  uint8_t watchdogEnabled;
  uint16_t graceSeconds;
  uint8_t channelEnabled;
  char channelKey[MOEtap2Prefs::kChannelKeyHexLength + 1];
};

bool validKey(std::string_view key) {
  if (key.empty()) return true;
  if (key.size() != MOEtap2Prefs::kChannelKeyHexLength) return false;
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
}

bool MOEtap2Prefs::load() {
  defaults();
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
  return true;
}

bool MOEtap2Prefs::save() const {
  PersistedPrefs persisted{};
  persisted.magic = kPrefsMagic;
  persisted.watchdogEnabled = watchdogEnabled_ ? 1 : 0;
  persisted.graceSeconds = graceSeconds_;
  persisted.channelEnabled = channelEnabled_ ? 1 : 0;
  copyKey(persisted.channelKey, sizeof(persisted.channelKey), channelKey_);
  return writeExact(kPrefsFilename, &persisted, sizeof(persisted));
}

bool MOEtap2Prefs::watchdogEnabled() const noexcept { return watchdogEnabled_; }
uint16_t MOEtap2Prefs::graceSeconds() const noexcept { return graceSeconds_; }
bool MOEtap2Prefs::channelEnabled() const noexcept { return channelEnabled_; }
const std::string& MOEtap2Prefs::channelKey() const noexcept { return channelKey_; }
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

bool MOEtap2Prefs::markWatchdogReboot() const {
  return writeExact(kRebootFilename, &kRebootMagic, sizeof(kRebootMagic));
}

bool MOEtap2Prefs::consumeWatchdogReboot() const {
  uint32_t marker = 0;
  const bool marked = readExact(kRebootFilename, &marker, sizeof(marker)) &&
                      marker == kRebootMagic;
  if (SPIFFS.exists(kRebootFilename)) SPIFFS.remove(kRebootFilename);
  return marked;
}

bool MOEtap2Prefs::appendLog(std::string_view day, std::string_view entry) const {
  if (day.empty() || entry.empty() || day.find('\n') != std::string_view::npos ||
      entry.find('\n') != std::string_view::npos) return false;

  std::string existing;
  if (SPIFFS.exists(kLogFilename)) {
    File in = SPIFFS.open(kLogFilename, "r");
    if (!in) return false;
    existing.reserve(in.size());
    while (in.available()) existing.push_back(static_cast<char>(in.read()));
    in.close();
  }
  const std::string header = "D " + std::string(day) + "\n";
  if (existing.rfind(header, 0) != 0) existing = header;
  existing += std::string(entry) + "\n";
  return writeExact(kLogFilename, existing.data(), existing.size());
}

bool MOEtap2Prefs::readLastLogLines(std::string_view day, size_t maxLines,
                                    std::string& output) const {
  output.clear();
  if (maxLines == 0 || !SPIFFS.exists(kLogFilename)) return true;
  File file = SPIFFS.open(kLogFilename, "r");
  if (!file) return false;
  std::string content;
  content.reserve(file.size());
  while (file.available()) content.push_back(static_cast<char>(file.read()));
  file.close();
  const std::string header = "D " + std::string(day) + "\n";
  if (content.rfind(header, 0) != 0) return true;
  std::vector<std::string> lines;
  size_t start = header.size();
  while (start < content.size()) {
    const size_t end = content.find('\n', start);
    if (end == std::string::npos) break;
    if (end > start) lines.emplace_back(content.substr(start, end - start));
    start = end + 1;
  }
  const size_t first = lines.size() > maxLines ? lines.size() - maxLines : 0;
  for (size_t i = first; i < lines.size(); ++i) {
    if (!output.empty()) output += " | ";
    output += lines[i];
  }
  return true;
}

bool MOEtap2Prefs::clearLog() const {
  return !SPIFFS.exists(kLogFilename) || SPIFFS.remove(kLogFilename);
}
