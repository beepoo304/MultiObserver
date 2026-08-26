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
  bool formatLog(char* output, size_t outputSize) const noexcept;
  bool clearLog() const;

 private:
  enum class Phase : uint8_t { Grace, Normal, Silent };
  enum class OutageStage : uint8_t { None, First, Second, Third, Alerted };

  struct ServiceTrack {
    bool down{false};
    OutageStage stage{OutageStage::None};
    uint32_t nextActionAtMs{0};
    uint32_t silentSinceMs{0};
  };

  static constexpr uint32_t kWifiCheckMs = 5'000;
  static constexpr uint32_t kMqttCheckMs = 15'000;
  static constexpr uint32_t kSilentCheckMs = 15 * 60'000;
  static constexpr uint32_t kWifiFirstMs = 3 * 60'000;
  static constexpr uint32_t kWifiSecondMs = 5 * 60'000;
  static constexpr uint32_t kWifiThirdMs = 30 * 60'000;
  static constexpr uint32_t kMqttFirstMs = 3 * 60'000;
  static constexpr uint32_t kMqttSecondMs = 5 * 60'000;
  static constexpr uint32_t kMqttThirdMs = 15 * 60'000;
  static constexpr uint32_t kMqttFourthMs = 30 * 60'000;
  static constexpr uint32_t kRebootDelayMs = 3 * 60'000;

  void finishGrace(uint32_t now);
  void runNormal(uint32_t now);
  void runSilent(uint32_t now);
  void checkWifi(uint32_t now);
  void checkMqtt(MOMQTT::BrokerId broker, ServiceTrack& track,
                 uint32_t now);
  void handleWifiDown(uint32_t now);
  void handleMqttDown(MOMQTT::BrokerId broker, ServiceTrack& track,
                      uint32_t now);
  void clearTrack(ServiceTrack& track) noexcept;
  void scheduleReboot(const char* reason, uint32_t now);
  void sendAlert(const char* text);
  void logEvent(const char* event) const;
  bool mqttSupervised(MOMQTT::BrokerId broker) const noexcept;
  bool mqttHealthy(MOMQTT::BrokerId broker) const noexcept;
  bool anyServiceDown() const noexcept;
  static bool elapsed(uint32_t now, uint32_t at) noexcept;
  static void durationText(uint32_t milliseconds, char* output,
                           size_t outputSize) noexcept;
  static const char* phaseName(Phase phase) noexcept;

  MOEtap2Prefs& prefs_;
  MOWifi& wifi_;
  MOMQTT& mqtt_;
  AlertSink alertSink_{nullptr};
  void* alertContext_{nullptr};
  Phase phase_{Phase::Grace};
  bool bootAfterWatchdog_{false};
  bool rebootScheduled_{false};
  uint32_t rebootAtMs_{0};
  uint32_t graceStartedMs_{0};
  uint32_t nextWifiCheckMs_{0};
  uint32_t nextMqttCheckMs_{0};
  uint32_t nextSilentCheckMs_{0};
  ServiceTrack wifiTrack_{};
  ServiceTrack mqtt1Track_{};
  ServiceTrack mqtt2Track_{};
};
