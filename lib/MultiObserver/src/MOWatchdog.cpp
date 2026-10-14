#include "MOWatchdog.h"

#include <Arduino.h>

#include <cstdio>

MOWatchdog::MOWatchdog(MOEtap2Prefs& prefs, MOWifi& wifi, MOMQTT& mqtt)
    : prefs_(prefs), wifi_(wifi), mqtt_(mqtt) {}

void MOWatchdog::begin() {
  postRebootMask_ = prefs_.consumeWatchdogReboot();
  if (postRebootMask_ & kMqtt1Bit)
    mqtt_.setRetryInterval(MOMQTT::BrokerId::Mqtt1, kPostRebootRetryMs);
  if (postRebootMask_ & kMqtt2Bit)
    mqtt_.setRetryInterval(MOMQTT::BrokerId::Mqtt2, kPostRebootRetryMs);
  restartGrace();
  Serial.printf("[MO][WDG] start previous_mqtt_reboot=0x%02x grace=%us\n",
                postRebootMask_, prefs_.graceSeconds());
}

void MOWatchdog::loop() {
  if (!prefs_.watchdogEnabled()) return;
  const uint32_t now = millis();
  if (phase_ == Phase::Grace) {
    if (elapsed(now, graceStartedMs_ +
                         static_cast<uint32_t>(prefs_.graceSeconds()) * 1000U)) {
      finishGrace(now);
    }
    return;
  }

  if (elapsed(now, nextWifiCheckMs_)) {
    nextWifiCheckMs_ = now + kWifiCheckMs;
    checkWifi(now);
  }
  if (elapsed(now, nextServiceCheckMs_)) {
    nextServiceCheckMs_ = now + kServiceCheckMs;
    if (wifi_.healthy()) {
      checkBroker(MOMQTT::BrokerId::Mqtt1, mqtt1_, now);
      checkBroker(MOMQTT::BrokerId::Mqtt2, mqtt2_, now);
    }
  }
  checkReboot(now);
}

void MOWatchdog::setAlertSink(AlertSink sink, void* context) noexcept {
  alertSink_ = sink;
  alertContext_ = context;
}

void MOWatchdog::restartGrace() noexcept {
  phase_ = Phase::Grace;
  graceStartedMs_ = millis();
  nextWifiCheckMs_ = graceStartedMs_;
  nextServiceCheckMs_ = graceStartedMs_;
  wifiDownSinceMs_ = 0;
  wifiRestarted_ = false;
  rebootBrokerMask_ = 0;
  rebootAtMs_ = 0;
  mqtt1_ = {};
  mqtt2_ = {};
  mqtt1_.postReboot = (postRebootMask_ & kMqtt1Bit) != 0;
  mqtt2_.postReboot = (postRebootMask_ & kMqtt2Bit) != 0;
}

void MOWatchdog::setEnabled(bool enabled) noexcept {
  prefs_.setWatchdogEnabled(enabled);
  if (enabled) restartGrace();
  else {
    rebootBrokerMask_ = 0;
    rebootAtMs_ = 0;
  }
}

void MOWatchdog::finishGrace(uint32_t now) {
  phase_ = Phase::Normal;
  nextWifiCheckMs_ = now;
  nextServiceCheckMs_ = now;
  sendAlert("AlertChannel newStart");
  Serial.println("[MO][WDG] grace complete");
}

void MOWatchdog::checkWifi(uint32_t now) {
  if (wifi_.healthy()) {
    wifiDownSinceMs_ = 0;
    wifiRestarted_ = false;
    return;
  }
  if (wifiDownSinceMs_ == 0) wifiDownSinceMs_ = now;
  if (!wifiRestarted_ && now - wifiDownSinceMs_ >= kWifiRestartAfterMs) {
    wifi_.restart();
    wifiRestarted_ = true;
    Serial.println("[MO][WDG] wifi restart; connection retry interval=3m");
  }
}

void MOWatchdog::checkBroker(MOMQTT::BrokerId broker,
                             BrokerRecovery& recovery, uint32_t now) {
  const uint8_t bit = brokerBit(broker);
  if (!brokerConfigured(broker)) {
    postRebootMask_ &= static_cast<uint8_t>(~bit);
    if (recovery.down) mqtt_.setRetryInterval(broker, kRetryIntervalMs);
    recovery = {};
    rebootBrokerMask_ &= static_cast<uint8_t>(~bit);
    if (rebootBrokerMask_ == 0) rebootAtMs_ = 0;
    return;
  }
  if (!brokerReady(broker)) return;

  if (brokerConnected(broker)) {
    const bool recovered = recovery.down || recovery.postReboot ||
                           (postRebootMask_ & bit) != 0;
    if (recovered) {
      sendAlert(broker == MOMQTT::BrokerId::Mqtt1 ? "mqtt1.restore"
                                                   : "mqtt2.restore");
    }
    postRebootMask_ &= static_cast<uint8_t>(~bit);
    rebootBrokerMask_ &= static_cast<uint8_t>(~bit);
    if (rebootBrokerMask_ == 0) rebootAtMs_ = 0;
    mqtt_.setRetryInterval(broker, kRetryIntervalMs);
    recovery = {};
    return;
  }

  if ((rebootBrokerMask_ & bit) != 0) return;

  if (recovery.postReboot || (postRebootMask_ & bit) != 0) {
    recovery.postReboot = true;
    mqtt_.setRetryInterval(broker, kPostRebootRetryMs);
    if (recovery.nextActionMs == 0) {
      // Let any in-flight handshake finish before declaring the broker down.
      recovery.nextActionMs = now + 15'000;
      return;
    }
    if (elapsed(now, recovery.nextActionMs) &&
        mqtt_.status(broker).state != MOMQTT::State::Connecting) {
      if (!recovery.down) {
        sendAlert(broker == MOMQTT::BrokerId::Mqtt1 ? "mqtt1.down"
                                                     : "mqtt2.down");
        recovery.down = true;
      }
      mqtt_.restart(broker);
      recovery.nextActionMs = now + kPostRebootRetryMs;
      Serial.printf("[MO][WDG][%s] post-reboot retry every 5m\n",
                    brokerName(broker));
    }
    return;
  }

  if (!recovery.down) {
    const MOMQTT::BrokerStatus st = mqtt_.status(broker);
    if (st.state == MOMQTT::State::Connecting) return;  // handshake in progress
    recovery.down = true;
    recovery.attempts = 1;
    recovery.nextActionMs = now + kRetryIntervalMs;
    mqtt_.setRetryInterval(broker, kRetryIntervalMs);
    if (st.deferredForHeap) {
      // Client could not even start (low heap): restarting it would not help.
      Serial.printf("[MO][WDG][%s] outage; attempt 1/3 (heap-deferred)\n",
                    brokerName(broker));
    } else {
      mqtt_.restart(broker);
      Serial.printf("[MO][WDG][%s] outage; attempt 1/3\n", brokerName(broker));
    }
    return;
  }

  if (!elapsed(now, recovery.nextActionMs)) return;
  if (mqtt_.status(broker).state == MOMQTT::State::Connecting) {
    recovery.nextActionMs = now + 15'000;
    return;
  }
  if (recovery.attempts < 3) {
    ++recovery.attempts;
    const bool heapDeferred = mqtt_.status(broker).deferredForHeap;
    if (!heapDeferred) mqtt_.restart(broker);
    recovery.nextActionMs = now + kRetryIntervalMs;
    Serial.printf("[MO][WDG][%s] attempt %u/3%s\n", brokerName(broker),
                  recovery.attempts, heapDeferred ? " (heap-deferred)" : "");
    return;
  }

  sendAlert(broker == MOMQTT::BrokerId::Mqtt1 ? "ESP restart.mqtt1"
                                               : "ESP restart.mqtt2");
  scheduleReboot(bit, now);
}

void MOWatchdog::scheduleReboot(uint8_t brokerBitValue, uint32_t now) {
  rebootBrokerMask_ |= brokerBitValue;
  if (rebootAtMs_ == 0) rebootAtMs_ = now + kRebootDelayMs;
  Serial.printf("[MO][WDG] reboot scheduled mask=0x%02x delay=30s\n",
                rebootBrokerMask_);
}

void MOWatchdog::checkReboot(uint32_t now) {
  if (rebootBrokerMask_ == 0 || !elapsed(now, rebootAtMs_)) return;
  for (MOMQTT::BrokerId broker : {MOMQTT::BrokerId::Mqtt1,
                                  MOMQTT::BrokerId::Mqtt2}) {
    const uint8_t bit = brokerBit(broker);
    if ((rebootBrokerMask_ & bit) != 0 &&
        (!brokerConfigured(broker) || !brokerReady(broker) ||
         brokerConnected(broker))) {
      rebootBrokerMask_ &= static_cast<uint8_t>(~bit);
    }
  }
  if (rebootBrokerMask_ == 0) {
    rebootAtMs_ = 0;
    return;
  }
  if (!prefs_.markWatchdogReboot(rebootBrokerMask_)) {
    Serial.println("[MO][WDG] reboot cancelled: cannot persist recovery state");
    for (MOMQTT::BrokerId broker : {MOMQTT::BrokerId::Mqtt1,
                                    MOMQTT::BrokerId::Mqtt2}) {
      if ((rebootBrokerMask_ & brokerBit(broker)) != 0) {
        BrokerRecovery& recovery = broker == MOMQTT::BrokerId::Mqtt1 ? mqtt1_ : mqtt2_;
        recovery.postReboot = true;
        recovery.nextActionMs = now + kPostRebootRetryMs;
        mqtt_.setRetryInterval(broker, kPostRebootRetryMs);
      }
    }
    rebootBrokerMask_ = 0;
    rebootAtMs_ = 0;
    return;
  }
  Serial.println("[MO][WDG] reboot");
  delay(20);
  ESP.restart();
}

void MOWatchdog::sendAlert(const char* text) {
  if (alertSink_ != nullptr && text != nullptr) alertSink_(alertContext_, text);
}

bool MOWatchdog::brokerConfigured(MOMQTT::BrokerId broker) const noexcept {
  return mqtt_.configured(broker);
}

bool MOWatchdog::brokerReady(MOMQTT::BrokerId broker) const noexcept {
  return mqtt_.canAttempt(broker);
}

bool MOWatchdog::brokerConnected(MOMQTT::BrokerId broker) const noexcept {
  return mqtt_.connected(broker);
}

bool MOWatchdog::elapsed(uint32_t now, uint32_t at) noexcept {
  return static_cast<int32_t>(now - at) >= 0;
}

const char* MOWatchdog::phaseName(Phase phase) noexcept {
  return phase == Phase::Grace ? "GRACE" : "NORMAL";
}

const char* MOWatchdog::brokerName(MOMQTT::BrokerId broker) noexcept {
  return broker == MOMQTT::BrokerId::Mqtt1 ? "MQTT1" : "MQTT2";
}

uint8_t MOWatchdog::brokerBit(MOMQTT::BrokerId broker) noexcept {
  return broker == MOMQTT::BrokerId::Mqtt1 ? kMqtt1Bit : kMqtt2Bit;
}

void MOWatchdog::formatStatus(char* output, size_t outputSize) const noexcept {
  if (output == nullptr || outputSize == 0) return;
  std::snprintf(output, outputSize,
                "WDG %s phase=%s wifi=%s mqtt1=%s mqtt2=%s reboot=%s",
                prefs_.watchdogEnabled() ? "ON" : "OFF",
                phaseName(phase_), wifi_.healthy() ? "UP" : "DOWN",
                !brokerConfigured(MOMQTT::BrokerId::Mqtt1) ? "OFF" :
                    (brokerConnected(MOMQTT::BrokerId::Mqtt1) ? "UP" : "DOWN"),
                !brokerConfigured(MOMQTT::BrokerId::Mqtt2) ? "OFF" :
                    (brokerConnected(MOMQTT::BrokerId::Mqtt2) ? "UP" : "DOWN"),
                rebootBrokerMask_ != 0 ? "PENDING" : "NO");
}