#include "MOAlertChannel.h"

#include <Arduino.h>

#include <cstdio>
#include <cstring>
#include <ctime>

namespace {
int hexNibble(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  return -1;
}
}  // namespace

MOAlertChannel::MOAlertChannel(MOEtap2Prefs& prefs) : prefs_(prefs) {}

void MOAlertChannel::setSender(Sender sender, void* context) noexcept {
  sender_ = sender;
  senderContext_ = context;
}

bool MOAlertChannel::ready() const noexcept {
  return prefs_.channelEnabled() && sender_ != nullptr &&
         (prefs_.channelKey().size() == MOEtap2Prefs::kChannelKeyHexLength ||
          prefs_.channelKey().size() == MOEtap2Prefs::kChannelKeyMaxHexLength);
}

bool MOAlertChannel::send(const char* text) const noexcept {
  if (text == nullptr || *text == '\0') return false;
  return sendFormatted(text);
}

bool MOAlertChannel::test(const char* watchdogStatus) const noexcept {
  char message[kMaxMessageLength + 1]{};
  std::snprintf(message, sizeof(message), "test alertchannel %s",
                watchdogStatus != nullptr ? watchdogStatus : "");
  return sendFormatted(message);
}

bool MOAlertChannel::decodeKey(uint8_t* secret,
                               size_t& secretLength) const noexcept {
  const size_t hexLength = prefs_.channelKey().size();
  if (secret == nullptr ||
      (hexLength != MOEtap2Prefs::kChannelKeyHexLength &&
       hexLength != MOEtap2Prefs::kChannelKeyMaxHexLength)) {
    return false;
  }
  secretLength = hexLength / 2;
  for (size_t index = 0; index < secretLength; ++index) {
    const int high = hexNibble(prefs_.channelKey()[index * 2]);
    const int low = hexNibble(prefs_.channelKey()[index * 2 + 1]);
    if (high < 0 || low < 0) return false;
    secret[index] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

bool MOAlertChannel::sendFormatted(const char* text) const noexcept {
  if (!ready()) {
    Serial.println("[MO][CHANNEL] skip disabled/key/sender");
    return false;
  }
  uint8_t secret[kSecretLength]{};
  size_t secretLength = 0;
  if (!decodeKey(secret, secretLength)) return false;
  char stamp[24]{};
  timestamp(stamp, sizeof(stamp));
  char message[kMaxMessageLength + 1]{};
  const int written = std::snprintf(message, sizeof(message), "%s %s", text, stamp);
  if (written < 0) return false;
  message[kMaxMessageLength] = '\0';
  const bool queued = sender_(senderContext_, secret, secretLength, message);
  Serial.printf("[MO][CHANNEL] %s text=%s\n",
                queued ? "queued" : "rejected", message);
  return queued;
}

void MOAlertChannel::timestamp(char* output, size_t outputSize) noexcept {
  if (output == nullptr || outputSize == 0) return;
  const time_t now = time(nullptr);
  struct tm utc{};
  if (now < 1735689600 || gmtime_r(&now, &utc) == nullptr) {
    std::snprintf(output, outputSize, "time-unknown");
    return;
  }
  std::strftime(output, outputSize, "%Y-%m-%d %H:%M:%S", &utc);
}
