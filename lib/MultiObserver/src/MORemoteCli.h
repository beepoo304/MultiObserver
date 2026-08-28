#pragma once

#include "MOAlertChannel.h"
#include "MOEtap2Prefs.h"

#include <cstddef>
#include <cstdint>

class MORemoteCli {
 public:
  using Executor = void (*)(void* context, uint32_t senderTimestamp,
                            char* command, char* reply);

  MORemoteCli(MOEtap2Prefs& prefs, MOAlertChannel& channel);

  void setExecutor(Executor executor, void* context) noexcept;
  [[nodiscard]] bool ready() const noexcept;
  bool copyChannelSecret(uint8_t* secret, size_t capacity,
                         size_t& length) const noexcept;
  bool handle(uint32_t senderTimestamp, const char* localName,
              const char* channelText) noexcept;

 private:
  static constexpr size_t kCliBufferCapacity = 160;

  MOEtap2Prefs& prefs_;
  MOAlertChannel& channel_;
  Executor executor_{nullptr};
  void* executorContext_{nullptr};
};
