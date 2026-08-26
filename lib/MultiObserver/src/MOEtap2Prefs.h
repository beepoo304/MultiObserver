#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

class MOEtap2Prefs {
 public:
  static constexpr uint16_t kDefaultGraceSeconds = 120;
  static constexpr uint16_t kMinGraceSeconds = 120;
  static constexpr uint16_t kMaxGraceSeconds = 300;
  static constexpr size_t kChannelKeyHexLength = 32;
  static constexpr size_t kChannelKeyMaxHexLength = 64;

  void defaults();
  bool load();
  bool save() const;

  [[nodiscard]] bool watchdogEnabled() const noexcept;
  [[nodiscard]] uint16_t graceSeconds() const noexcept;
  [[nodiscard]] bool channelEnabled() const noexcept;
  [[nodiscard]] const std::string& channelKey() const noexcept;

  void setWatchdogEnabled(bool enabled) noexcept;
  bool setGraceSeconds(uint16_t seconds) noexcept;
  void setChannelEnabled(bool enabled) noexcept;
  bool setChannelKey(std::string_view key) noexcept;

  // This marker is deliberately separate from user preferences. It is written
  // immediately before an ESP restart and consumed exactly once at next boot.
  bool markWatchdogReboot() const;
  bool consumeWatchdogReboot() const;

  // Each entry is short. The file keeps all state transitions for the current
  // repeater-clock day; callers request only the last few lines for CLI.
  bool appendLog(std::string_view day, std::string_view entry) const;
  bool readLastLogLines(std::string_view day, size_t maxLines,
                        std::string& output) const;
  bool clearLog() const;

 private:
  bool watchdogEnabled_{true};
  uint16_t graceSeconds_{kDefaultGraceSeconds};
  bool channelEnabled_{false};
  std::string channelKey_;
};
