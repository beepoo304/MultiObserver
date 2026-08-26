#include "MOWatchdog.h"

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
  if (rebootScheduled_ && elapsed(now, rebootAtMs_)) {
    logEvent("reboot");
    if (prefs_.markWatchdogReboot()) {
      Serial.println("[MO][WDG] reboot");
      delay(20);
      ESP.restart();
    }
    rebootScheduled_ = false;
    logEvent("reboot_marker_error");
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
}

void MOWatchdog::setAlertSink(AlertSink sink, void* context) noexcept {
  alertSink_ = sink;
  alertContext_ = context;
}

void MOWatchdog::restartGrace() noexcept {
  phase_ = Phase::Grace;
  rebootScheduled_ = false;
  graceStartedMs_ = millis();
  nextWifiCheckMs_ = graceStartedMs_;
  nextMqttCheckMs_ = graceStartedMs_;
  nextSilentCheckMs_ = graceStartedMs_;
  clearTrack(wifiTrack_);
  clearTrack(mqtt1Track_);
  clearTrack(mqtt2Track_);
}

void MOWatchdog::setEnabled(bool enabled) noexcept {
  prefs_.setWatchdogEnabled(enabled);
  if (!enabled) {
    rebootScheduled_ = false;
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
  if (bootAfterWatchdog_ && anyServiceDown()) {
    phase_ = Phase::Silent;
    nextSilentCheckMs_ = now;
    wifiTrack_.silentSinceMs = now;
    mqtt1Track_.silentSinceMs = now;
    mqtt2Track_.silentSinceMs = now;
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
  if (anyServiceDown()) return;

  char duration[20]{};
  const uint32_t started = wifiTrack_.silentSinceMs != 0
                               ? wifiTrack_.silentSinceMs
                               : now;
  durationText(now - started, duration, sizeof(duration));
  if (!wifi_.healthy()) {
    sendAlert("wifi.restore");
  }
  char alert[96]{};
  std::snprintf(alert, sizeof(alert), "services.restore silent=%s", duration);
  sendAlert(alert);
  logEvent("silent_restore");
  bootAfterWatchdog_ = false;
  phase_ = Phase::Normal;
  clearTrack(wifiTrack_);
  clearTrack(mqtt1Track_);
  clearTrack(mqtt2Track_);
  nextWifiCheckMs_ = now;
  nextMqttCheckMs_ = now;
}

void MOWatchdog::checkWifi(uint32_t now) {
  if (wifi_.healthy()) {
    if (wifiTrack_.down) logEvent("wifi_up");
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
      wifi_.restart(); logEvent("wifi_restart_1");
      wifiTrack_.stage = OutageStage::Second;
      wifiTrack_.nextActionAtMs = now + kWifiSecondMs;
      break;
    case OutageStage::Second:
      wifi_.restart(); logEvent("wifi_restart_2");
      wifiTrack_.stage = OutageStage::Third;
      wifiTrack_.nextActionAtMs = now + kWifiThirdMs;
      break;
    case OutageStage::Third:
      sendAlert("Restart.ESP.wifi.down");
      scheduleReboot("wifi", now);
      wifiTrack_.stage = OutageStage::Alerted;
      break;
    default: break;
  }
}

void MOWatchdog::checkMqtt(MOMQTT::BrokerId broker, ServiceTrack& track,
                           uint32_t now) {
  if (!mqttSupervised(broker)) { clearTrack(track); return; }
  if (mqttHealthy(broker)) {
    if (track.down) logEvent(broker == MOMQTT::BrokerId::Mqtt1 ? "mqtt1_up" : "mqtt2_up");
    clearTrack(track);
    return;
  }
  handleMqttDown(broker, track, now);
}

void MOWatchdog::handleMqttDown(MOMQTT::BrokerId broker, ServiceTrack& track,
                                uint32_t now) {
  const char* prefix = broker == MOMQTT::BrokerId::Mqtt1 ? "mqtt1" : "mqtt2";
  if (!track.down) {
    track.down = true; track.stage = OutageStage::First;
    track.nextActionAtMs = now + kMqttFirstMs;
    logEvent(broker == MOMQTT::BrokerId::Mqtt1 ? "mqtt1_down" : "mqtt2_down");
    return;
  }
  if (!elapsed(now, track.nextActionAtMs)) return;
  switch (track.stage) {
    case OutageStage::First:
      mqtt_.restart(broker); track.stage = OutageStage::Second;
      track.nextActionAtMs = now + kMqttSecondMs; break;
    case OutageStage::Second:
      mqtt_.restart(broker); track.stage = OutageStage::Third;
      track.nextActionAtMs = now + kMqttThirdMs; break;
    case OutageStage::Third:
      mqtt_.restart(broker); track.stage = OutageStage::Alerted;
      track.nextActionAtMs = now + kMqttFourthMs; break;
    case OutageStage::Alerted:
      {
        char alert[48]{};
        std::snprintf(alert, sizeof(alert), "Restart.ESP.%s.down", prefix);
        sendAlert(alert); scheduleReboot(prefix, now);
      }
      track.stage = OutageStage::None;
      break;
    default: break;
  }
}

void MOWatchdog::clearTrack(ServiceTrack& track) noexcept { track = {}; }

void MOWatchdog::scheduleReboot(const char* reason, uint32_t now) {
  if (rebootScheduled_) return;
  rebootScheduled_ = true;
  rebootAtMs_ = now + kRebootDelayMs;
  char event[48]{};
  std::snprintf(event, sizeof(event), "reboot_scheduled_%s", reason);
  logEvent(event);
}

void MOWatchdog::sendAlert(const char* text) {
  if (alertSink_ != nullptr && text != nullptr) alertSink_(alertContext_, text);
}

void MOWatchdog::logEvent(const char* event) const {
  const time_t now = time(nullptr);
  struct tm utc{};
  if (now < 1735689600 || gmtime_r(&now, &utc) == nullptr) return;
  char day[12]{}; char stamp[12]{}; char line[96]{};
  std::strftime(day, sizeof(day), "%Y-%m-%d", &utc);
  std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &utc);
  std::snprintf(line, sizeof(line), "%s %s", stamp, event);
  prefs_.appendLog(day, line);
}

bool MOWatchdog::mqttSupervised(MOMQTT::BrokerId broker) const noexcept {
  return mqtt_.configured(broker);
}

bool MOWatchdog::mqttHealthy(MOMQTT::BrokerId broker) const noexcept {
  return mqtt_.connected(broker);
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
  switch (phase) { case Phase::Grace: return "GRACE"; case Phase::Normal: return "NORMAL"; case Phase::Silent: return "SILENT"; }
  return "?";
}

void MOWatchdog::formatStatus(char* output, size_t outputSize) const noexcept {
  std::snprintf(output, outputSize, "WDG %s %s wifi=%s mqtt1=%s mqtt2=%s reboot=%s",
                prefs_.watchdogEnabled() ? "ON" : "OFF", phaseName(phase_),
                wifi_.healthy() ? "UP" : "DOWN",
                mqtt_.connected(MOMQTT::BrokerId::Mqtt1) ? "UP" : "DOWN",
                mqtt_.connected(MOMQTT::BrokerId::Mqtt2) ? "UP" : "DOWN",
                rebootScheduled_ ? "PENDING" : "NO");
}

bool MOWatchdog::formatLog(char* output, size_t outputSize) const noexcept {
  if (output == nullptr || outputSize == 0) return false;
  const time_t now = time(nullptr); struct tm utc{};
  if (now < 1735689600 || gmtime_r(&now, &utc) == nullptr) {
    std::snprintf(output, outputSize, "CLEAR"); return true;
  }
  char day[12]{}; std::strftime(day, sizeof(day), "%Y-%m-%d", &utc);
  std::string lines;
  if (!prefs_.readLastLogLines(day, 5, lines)) return false;
  std::snprintf(output, outputSize, "%s", lines.empty() ? "CLEAR" : lines.c_str());
  return true;
}

bool MOWatchdog::clearLog() const { return prefs_.clearLog(); }
