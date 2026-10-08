#include "MOWatchdog.h"

#include "MOLocalTime.h"

#include <Arduino.h>

#include <cstdio>
#include <ctime>
#include <string>

MOWatchdog::MOWatchdog(MOEtap2Prefs& prefs, MOWifi& wifi, MOMQTT& mqtt)
    : prefs_(prefs), wifi_(wifi), mqtt_(mqtt) {}

void MOWatchdog::begin() {
  bootAfterWatchdog_ = prefs_.consumeWatchdogReboot();
  restartGrace();
  Serial.printf("[MO][WDG] boot previous_wdg_reboot=%s grace=%us\n",
                bootAfterWatchdog_ ? "yes" : "no", prefs_.graceSeconds());
}

void MOWatchdog::loop() {
  if (!prefs_.watchdogEnabled()) return;
  const uint32_t now = millis();
  if (elapsed(now, nextLogRotationCheckMs_)) {
    nextLogRotationCheckMs_ = now + 1'000;
    struct tm local{};
    if (MOLocalTime::now(local)) {
      const int32_t dayId = (local.tm_year * 366) + local.tm_yday;
      if (dayId != logDayId_) {
        logDayId_ = dayId;
        char day[12]{};
        std::strftime(day, sizeof(day), "%Y-%m-%d", &local);
        std::string ignored;
        prefs_.readLastLogLines(day, 0, ignored);
      }
    }
  }
  if (phase_ == Phase::Grace) {
    const uint32_t graceMs = static_cast<uint32_t>(prefs_.graceSeconds()) * 1000U;
    if (elapsed(now, graceStartedMs_ + graceMs)) finishGrace(now);
    return;
  }
  if (phase_ == Phase::Silent) {
    if (elapsed(now, nextSilentCheckMs_)) runSilent(now);
    return;
  }
  runNormal(now);
  if (rebootScheduled_ && elapsed(now, rebootAtMs_)) {
    // Recheck all owners before committing the reboot. One recovered broker
    // cannot cancel a reboot still needed by another service.
    if (wifi_.healthy()) cancelReboot(Service::Wifi);
    if (!mqttSupervised(MOMQTT::BrokerId::Mqtt1) || mqttHealthy(MOMQTT::BrokerId::Mqtt1))
      cancelReboot(Service::Mqtt1);
    if (!mqttSupervised(MOMQTT::BrokerId::Mqtt2) || mqttHealthy(MOMQTT::BrokerId::Mqtt2))
      cancelReboot(Service::Mqtt2);
    if (!rebootScheduled_) return;
    logEvent("reboot");
    if (!prefs_.markWatchdogReboot()) {
      // A full/damaged filesystem must not veto the recovery action.
      Serial.println("[MO][WDG] reboot marker failed; rebooting anyway");
      logEvent("reboot_marker_error");
    }
    Serial.println("[MO][WDG] reboot");
    delay(20);
    ESP.restart();
  }
}

void MOWatchdog::setAlertSink(AlertSink sink, void* context) noexcept {
  alertSink_ = sink;
  alertContext_ = context;
}

void MOWatchdog::restartGrace() noexcept {
  phase_ = Phase::Grace;
  rebootScheduled_ = false;
  rebootServices_ = 0;
  graceStartedMs_ = millis();
  nextWifiCheckMs_ = graceStartedMs_;
  nextMqttCheckMs_ = graceStartedMs_;
  nextSilentCheckMs_ = graceStartedMs_;
  silentStartedMs_ = 0;
  nextLogRotationCheckMs_ = graceStartedMs_;
  logDayId_ = -1;
  silentWifi_ = false;
  silentMqtt1_ = false;
  silentMqtt2_ = false;
  clearTrack(wifiTrack_);
  clearTrack(mqtt1Track_);
  clearTrack(mqtt2Track_);
}

void MOWatchdog::setEnabled(bool enabled) noexcept {
  prefs_.setWatchdogEnabled(enabled);
  if (!enabled) {
    rebootScheduled_ = false;
    rebootServices_ = 0;
    clearTrack(wifiTrack_);
    clearTrack(mqtt1Track_);
    clearTrack(mqtt2Track_);
  } else {
    restartGrace();
  }
}

void MOWatchdog::finishGrace(uint32_t now) {
  Serial.println("[MO][WDG] start checking");
  logEvent("start");
  sendAlert("AlertChannel newStart");
  const bool shouldEnterSilent = bootAfterWatchdog_ && anyServiceDown();
  // The persisted marker was already consumed at boot. Its in-memory meaning
  // is also one-shot and must not survive a later CLI restart of WDG grace.
  bootAfterWatchdog_ = false;
  if (shouldEnterSilent) {
    phase_ = Phase::Silent;
    silentStartedMs_ = now;
    nextSilentCheckMs_ = now + kSilentCheckMs;
    silentWifi_ = !wifi_.healthy();
    silentMqtt1_ = !silentWifi_ && mqttSupervised(MOMQTT::BrokerId::Mqtt1) &&
                   !mqttHealthy(MOMQTT::BrokerId::Mqtt1);
    silentMqtt2_ = !silentWifi_ && mqttSupervised(MOMQTT::BrokerId::Mqtt2) &&
                   !mqttHealthy(MOMQTT::BrokerId::Mqtt2);
    wifiTrack_.silentSinceMs = silentWifi_ ? now : 0;
    mqtt1Track_.silentSinceMs = silentMqtt1_ ? now : 0;
    mqtt2Track_.silentSinceMs = silentMqtt2_ ? now : 0;
    logEvent("silent_start");
    return;
  }
  phase_ = Phase::Normal;
  nextWifiCheckMs_ = now;
  nextMqttCheckMs_ = now;
}

void MOWatchdog::runNormal(uint32_t now) {
  if (elapsed(now, nextWifiCheckMs_)) {
    nextWifiCheckMs_ = now + kWifiCheckMs;
    checkWifi(now);
  }
  if (!wifi_.healthy()) return;
  if (elapsed(now, nextMqttCheckMs_)) {
    nextMqttCheckMs_ = now + kMqttCheckMs;
    checkMqtt(MOMQTT::BrokerId::Mqtt1, mqtt1Track_, now);
    checkMqtt(MOMQTT::BrokerId::Mqtt2, mqtt2Track_, now);
  }
}

void MOWatchdog::runSilent(uint32_t now) {
  nextSilentCheckMs_ = now + kSilentCheckMs;
  if (now - silentStartedMs_ >= kSilentCooldownMs) {
    logEvent("silent.retry");
    Serial.println("[MO][WDG] cooldown expired: recovery rearmed");
    phase_ = Phase::Normal;
    silentWifi_ = silentMqtt1_ = silentMqtt2_ = false;
    clearTrack(wifiTrack_);
    clearTrack(mqtt1Track_);
    clearTrack(mqtt2Track_);
    nextWifiCheckMs_ = now;
    nextMqttCheckMs_ = now;
    return;
  }
  if (silentWifi_) {
    if (!wifi_.healthy()) return;
    char duration[20]{};
    char alert[64]{};
    durationText(now - wifiTrack_.silentSinceMs, duration, sizeof(duration));
    std::snprintf(alert, sizeof(alert), "wifi.restore duration=%s", duration);
    sendAlert(alert);
    logEvent("wifi.restore");
    silentWifi_ = false;
    // MQTT supervision resumes only after Wi-Fi itself has recovered.
    silentMqtt1_ = mqttSupervised(MOMQTT::BrokerId::Mqtt1) &&
                   !mqttHealthy(MOMQTT::BrokerId::Mqtt1);
    silentMqtt2_ = mqttSupervised(MOMQTT::BrokerId::Mqtt2) &&
                   !mqttHealthy(MOMQTT::BrokerId::Mqtt2);
    mqtt1Track_.silentSinceMs = silentMqtt1_ ? now : 0;
    mqtt2Track_.silentSinceMs = silentMqtt2_ ? now : 0;
  }
  if (silentMqtt1_ && mqttHealthy(MOMQTT::BrokerId::Mqtt1)) {
    char duration[20]{};
    char alert[64]{};
    durationText(now - mqtt1Track_.silentSinceMs, duration, sizeof(duration));
    std::snprintf(alert, sizeof(alert), "mqtt1.restore duration=%s", duration);
    sendAlert(alert);
    logEvent("mqtt1.restore");
    silentMqtt1_ = false;
  }
  if (silentMqtt1_ && !mqttSupervised(MOMQTT::BrokerId::Mqtt1)) {
    logEvent("mqtt1.off");
    silentMqtt1_ = false;
  }
  if (silentMqtt2_ && mqttHealthy(MOMQTT::BrokerId::Mqtt2)) {
    char duration[20]{};
    char alert[64]{};
    durationText(now - mqtt2Track_.silentSinceMs, duration, sizeof(duration));
    std::snprintf(alert, sizeof(alert), "mqtt2.restore duration=%s", duration);
    sendAlert(alert);
    logEvent("mqtt2.restore");
    silentMqtt2_ = false;
  }
  if (silentMqtt2_ && !mqttSupervised(MOMQTT::BrokerId::Mqtt2)) {
    logEvent("mqtt2.off");
    silentMqtt2_ = false;
  }
  if (silentWifi_ || silentMqtt1_ || silentMqtt2_) return;

  logEvent("silent.done");
  phase_ = Phase::Normal;
  clearTrack(wifiTrack_);
  clearTrack(mqtt1Track_);
  clearTrack(mqtt2Track_);
  nextWifiCheckMs_ = now;
  nextMqttCheckMs_ = now;
}

void MOWatchdog::checkWifi(uint32_t now) {
  if (wifi_.healthy()) {
    if (wifiTrack_.down) {
      logEvent("wifi_up");
      cancelReboot(Service::Wifi);
    }
    clearTrack(wifiTrack_);
    return;
  }
  handleWifiDown(now);
}

void MOWatchdog::handleWifiDown(uint32_t now) {
  if (!wifiTrack_.down) {
    wifiTrack_.down = true;
    wifiTrack_.stage = OutageStage::First;
    wifiTrack_.nextActionAtMs = now + kWifiFirstMs;
    logEvent("wifi_down");
    return;
  }
  if (!elapsed(now, wifiTrack_.nextActionAtMs)) return;
  switch (wifiTrack_.stage) {
    case OutageStage::First:
      wifi_.restart();
      logEvent("wifi.r1");
      wifiTrack_.stage = OutageStage::Second;
      wifiTrack_.nextActionAtMs = now + kWifiSecondMs;
      break;
    case OutageStage::Second:
      wifi_.restart();
      logEvent("wifi.r2");
      wifiTrack_.stage = OutageStage::Third;
      wifiTrack_.nextActionAtMs = now + kWifiThirdMs;
      break;
    case OutageStage::Third:
      sendAlert("Restart.ESP.wifi.down");
      scheduleReboot(Service::Wifi, now);
      wifiTrack_.stage = OutageStage::Alerted;
      break;
    default: break;
  }
}

void MOWatchdog::checkMqtt(MOMQTT::BrokerId broker, ServiceTrack& track,
                           uint32_t now) {
  if (!mqttSupervised(broker)) {
    cancelReboot(broker == MOMQTT::BrokerId::Mqtt1 ? Service::Mqtt1
                                                    : Service::Mqtt2);
    clearTrack(track);
    return;
  }
  if (mqttHealthy(broker)) {
    if (track.down) logEvent(broker == MOMQTT::BrokerId::Mqtt1 ? "mqtt1_up" : "mqtt2_up");
    cancelReboot(broker == MOMQTT::BrokerId::Mqtt1 ? Service::Mqtt1
                                                    : Service::Mqtt2);
    clearTrack(track);
    return;
  }
  handleMqttDown(broker, track, now);
}

void MOWatchdog::handleMqttDown(MOMQTT::BrokerId broker, ServiceTrack& track,
                                uint32_t now) {
  const char* prefix = broker == MOMQTT::BrokerId::Mqtt1 ? "mqtt1" : "mqtt2";
  if (!track.down) {
    track.down = true;
    track.stage = OutageStage::First;
    track.nextActionAtMs = now + kMqttFirstMs;
    logEvent(broker == MOMQTT::BrokerId::Mqtt1 ? "mqtt1_down" : "mqtt2_down");
    return;
  }
  if (!elapsed(now, track.nextActionAtMs)) return;
  switch (track.stage) {
    case OutageStage::First:
      mqtt_.restart(broker);
      track.stage = OutageStage::Second;
      logEvent(broker == MOMQTT::BrokerId::Mqtt1 ? "mqtt1.r1" : "mqtt2.r1");
      track.nextActionAtMs = now + kMqttSecondMs;
      break;
    case OutageStage::Second:
      mqtt_.restart(broker);
      track.stage = OutageStage::Third;
      logEvent(broker == MOMQTT::BrokerId::Mqtt1 ? "mqtt1.r2" : "mqtt2.r2");
      track.nextActionAtMs = now + kMqttThirdMs;
      break;
    case OutageStage::Third:
      mqtt_.restart(broker);
      track.stage = OutageStage::Alerted;
      logEvent(broker == MOMQTT::BrokerId::Mqtt1 ? "mqtt1.r3" : "mqtt2.r3");
      track.nextActionAtMs = now + kMqttFourthMs;
      break;
    case OutageStage::Alerted:
      {
        char alert[48]{};
        std::snprintf(alert, sizeof(alert), "Restart.ESP.%s.down", prefix);
        sendAlert(alert);
        scheduleReboot(broker == MOMQTT::BrokerId::Mqtt1 ? Service::Mqtt1
                                                          : Service::Mqtt2,
                       now);
      }
      track.stage = OutageStage::RebootPending;
      break;
    default:
      break;
  }
}

void MOWatchdog::clearTrack(ServiceTrack& track) noexcept { track = {}; }

void MOWatchdog::scheduleReboot(Service service, uint32_t now) {
  if (service == Service::None) return;
  const uint8_t bit = 1U << static_cast<uint8_t>(service);
  if (rebootServices_ & bit) return;
  rebootServices_ |= bit;
  if (!rebootScheduled_) rebootAtMs_ = now + kRebootDelayMs;
  rebootScheduled_ = true;
  char event[48]{};
  std::snprintf(event, sizeof(event), "reboot.%s", serviceName(service));
  logEvent(event);
}

void MOWatchdog::cancelReboot(Service service) {
  const uint8_t bit = 1U << static_cast<uint8_t>(service);
  if (!rebootScheduled_ || !(rebootServices_ & bit)) return;
  rebootServices_ &= ~bit;
  if (rebootServices_ != 0) return;
  rebootScheduled_ = false;
  logEvent("reboot.cancel");
}

void MOWatchdog::sendAlert(const char* text) {
  if (alertSink_ != nullptr && text != nullptr) alertSink_(alertContext_, text);
}

void MOWatchdog::logEvent(const char* event) const {
  struct tm local{};
  if (!MOLocalTime::now(local)) return;
  char day[12]{};
  char stamp[12]{};
  char line[96]{};
  std::strftime(day, sizeof(day), "%Y-%m-%d", &local);
  std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &local);
  std::snprintf(line, sizeof(line), "%s %s", stamp, event);
  prefs_.appendLog(day, line);
}

bool MOWatchdog::mqttSupervised(MOMQTT::BrokerId broker) const noexcept {
  return mqtt_.configured(broker);
}

bool MOWatchdog::mqttHealthy(MOMQTT::BrokerId broker) const noexcept {
  return mqtt_.publishingHealthy(broker);
}

bool MOWatchdog::anyServiceDown() const noexcept {
  if (!wifi_.healthy()) return true;
  return (mqttSupervised(MOMQTT::BrokerId::Mqtt1) && !mqttHealthy(MOMQTT::BrokerId::Mqtt1)) ||
         (mqttSupervised(MOMQTT::BrokerId::Mqtt2) && !mqttHealthy(MOMQTT::BrokerId::Mqtt2));
}

bool MOWatchdog::elapsed(uint32_t now, uint32_t at) noexcept {
  return static_cast<int32_t>(now - at) >= 0;
}

void MOWatchdog::durationText(uint32_t milliseconds, char* output,
                              size_t outputSize) noexcept {
  std::snprintf(output, outputSize, "%lum", static_cast<unsigned long>(milliseconds / 60'000));
}

const char* MOWatchdog::phaseName(Phase phase) noexcept {
  switch (phase) {
    case Phase::Grace:
      return "GRACE";
    case Phase::Normal:
      return "NORMAL";
    case Phase::Silent:
      return "SILENT";
  }
  return "?";
}

const char* MOWatchdog::serviceName(Service service) noexcept {
  switch (service) {
    case Service::Wifi:
      return "wifi";
    case Service::Mqtt1:
      return "mqtt1";
    case Service::Mqtt2:
      return "mqtt2";
    case Service::None:
      return "none";
  }
  return "none";
}

void MOWatchdog::formatStatus(char* output, size_t outputSize) const noexcept {
  std::snprintf(output, outputSize, "WDG %s %s wifi=%s mqtt1=%s mqtt2=%s reboot=%s",
                prefs_.watchdogEnabled() ? "ON" : "OFF", phaseName(phase_),
                wifi_.healthy() ? "UP" : "DOWN",
                !mqtt_.configured(MOMQTT::BrokerId::Mqtt1) ? "OFF" :
                    (mqtt_.publishingHealthy(MOMQTT::BrokerId::Mqtt1) ? "UP" : "DOWN"),
                !mqtt_.configured(MOMQTT::BrokerId::Mqtt2) ? "OFF" :
                    (mqtt_.publishingHealthy(MOMQTT::BrokerId::Mqtt2) ? "UP" : "DOWN"),
                rebootScheduled_ ? "PENDING" : "NO");
}

bool MOWatchdog::formatLog(char* output, size_t outputSize) const noexcept {
  if (output == nullptr || outputSize == 0) return false;
  struct tm local{};
  if (!MOLocalTime::now(local)) {
    std::snprintf(output, outputSize, "CLEAR");
    return true;
  }
  char day[12]{};
  std::strftime(day, sizeof(day), "%Y-%m-%d", &local);
  std::string lines;
  if (!prefs_.readLastLogLines(day, 5, lines)) return false;
  std::snprintf(output, outputSize, "%s", lines.empty() ? "CLEAR" : lines.c_str());
  return true;
}

bool MOWatchdog::clearLog() const { return prefs_.clearLog(); }
