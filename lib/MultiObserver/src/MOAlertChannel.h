#pragma once

#include "MOEtap2Prefs.h"

#include <cstddef>
#include <cstdint>

class MOAlertChannel {
 public:
  using Sender = bool (*)(void* context, const uint8_t* secret,
                          size_t secretLength, const char* text);

  explicit MOAlertChannel(MOEtap2Prefs& prefs);

  void setSender(Sender sender, void* context) noexcept;
  [[nodiscard]] bool ready() const noexcept;
  bool send(const char* text) const noexcept;
  bool test(const char* watchdogStatus) const noexcept;

 private:
  static constexpr size_t kSecretLength = 32;
  static constexpr size_t kMaxMessageLength = 130;

  bool decodeKey(uint8_t* secret, size_t& secretLength) const noexcept;
  bool sendFormatted(const char* text) const noexcept;
  static void timestamp(char* output, size_t outputSize) noexcept;

  MOEtap2Prefs& prefs_;
  Sender sender_{nullptr};
  void* senderContext_{nullptr};
};
