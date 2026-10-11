#pragma once

#include "MOEtap2Prefs.h"
#include "MOMQTT.h"
#include "MOWifi.h"

#include <cstddef>
#include <cstdint>

class MOWatchdog {
 public:
  using AlertSink = bool (*)(void* context, const char* text);

  MOWatchdog(MOEtap2Prefs& prefs, MOWifi& wifi, MOMQTT& mqtt);

  void begin();
  void loop();
  void setAlertSink(AlertSink sink, void* context) noexcept;
  void restartGrace() noexcept;
  void setEnabled(bool enabled) noexcept;
  void formatStatus(char* output, size_t outputSize) const noexcept;

 private:
  enum class Phase : uint8_t { Grace, Normal };
  struct BrokerRecovery {
    bool down{false};
    bool postReboot{false};
    uint8_t attempts{0};
    uint32_t nextActionMs{0};
  };

  static constexpr uint8_t kMqtt1Bit = 1U << 0;
  static constexpr uint8_t kMqtt2Bit = 1U << 1;
  static constexpr uint32_t kWifiCheckMs = 5'000;
  static constexpr uint32_t kServiceCheckMs = 5'000;
  static constexpr uint32_t kWifiRestartAfterMs = 30'000;
  static constexpr uint32_t kRetryIntervalMs = 3 * 60'000;
  static constexpr uint32_t kPostRebootRetryMs = 5 * 60'000;
  static constexpr uint32_t kRebootDelayMs = 30'000;

  void finishGrace(uint32_t now);
  void checkWifi(uint32_t now);
  void checkBroker(MOMQTT::BrokerId broker, BrokerRecovery& recovery,
                   uint32_t now);
  void scheduleReboot(uint8_t brokerBit, uint32_t now);
  void checkReboot(uint32_t now);
  void sendAlert(const char* text);
  bool brokerConfigured(MOMQTT::BrokerId broker) const noexcept;
  bool brokerReady(MOMQTT::BrokerId broker) const noexcept;
  bool brokerConnected(MOMQTT::BrokerId broker) const noexcept;
  static bool elapsed(uint32_t now, uint32_t at) noexcept;
  static const char* phaseName(Phase phase) noexcept;
  static const char* brokerName(MOMQTT::BrokerId broker) noexcept;
  static uint8_t brokerBit(MOMQTT::BrokerId broker) noexcept;

  MOEtap2Prefs& prefs_;
  MOWifi& wifi_;
  MOMQTT& mqtt_;
  AlertSink alertSink_{nullptr};
  void* alertContext_{nullptr};
  Phase phase_{Phase::Grace};
  uint32_t graceStartedMs_{0};
  uint32_t nextWifiCheckMs_{0};
  uint32_t nextServiceCheckMs_{0};
  uint32_t wifiDownSinceMs_{0};
  bool wifiRestarted_{false};
  uint8_t postRebootMask_{0};
  uint8_t rebootBrokerMask_{0};
  uint32_t rebootAtMs_{0};
  BrokerRecovery mqtt1_{};
  BrokerRecovery mqtt2_{};
};
