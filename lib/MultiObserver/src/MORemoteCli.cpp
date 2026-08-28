#include "MORemoteCli.h"

#include <cstring>

MORemoteCli::MORemoteCli(MOEtap2Prefs& prefs, MOAlertChannel& channel)
    : prefs_(prefs), channel_(channel) {}

void MORemoteCli::setExecutor(Executor executor, void* context) noexcept {
  executor_ = executor;
  executorContext_ = context;
}

bool MORemoteCli::ready() const noexcept {
  return prefs_.remoteCliEnabled() && channel_.ready() && executor_ != nullptr;
}

bool MORemoteCli::copyChannelSecret(uint8_t* secret, size_t capacity,
                                    size_t& length) const noexcept {
  if (!ready()) return false;
  return channel_.copySecret(secret, capacity, length);
}

bool MORemoteCli::handle(uint32_t senderTimestamp, const char* localName,
                         const char* channelText) noexcept {
  if (!ready() || localName == nullptr || channelText == nullptr) return false;

  const char* separator = std::strstr(channelText, ": ");
  if (separator == nullptr) return false;

  const size_t senderLength = static_cast<size_t>(separator - channelText);
  if (std::strlen(localName) == senderLength &&
      std::memcmp(channelText, localName, senderLength) == 0) {
    return false;
  }

  const char* input = separator + 2;
  const size_t commandLength = strnlen(input, kCliBufferCapacity + 1);
  if (commandLength == 0 || commandLength > kCliBufferCapacity) return false;

  char command[kCliBufferCapacity + 1]{};
  char reply[kCliBufferCapacity + 1]{};
  std::memcpy(command, input, commandLength);
  executor_(executorContext_, senderTimestamp, command, reply);

  if (reply[0] == '\0') return true;
  return channel_.sendRaw(reply);
}
