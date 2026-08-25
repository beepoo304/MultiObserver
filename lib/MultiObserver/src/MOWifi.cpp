#include "MOWifi.h"

#include <Arduino.h>

MOWifi::MOWifi() = default;

void MOWifi::begin(const std::string& ssid, const std::string& password) {
  Serial.println("[MO] WiFi start");
  setCredentials(ssid, password);

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);

  state_ = State::Disconnected;
  stateSinceMs_ = millis();
  retryAtMs_ = stateSinceMs_;

  connect();
}

void MOWifi::loop() {
  const uint32_t now = millis();

  switch (state_) {
    case State::Disconnected:
      handleDisconnected(now);
      break;

    case State::Connecting:
      handleConnecting(now);
      break;

    case State::Connected:
      handleConnected(now);
      break;
  }
}

void MOWifi::connect() {
  if (ssid_.empty()) {
    disconnect();
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    state_ = State::Connected;
    stateSinceMs_ = millis();
    Serial.println("[MO] WiFi connected");
    return;
  }

  startConnection();
}

void MOWifi::disconnect() {
  WiFi.disconnect(false, false);
  state_ = State::Disconnected;
  stateSinceMs_ = millis();
  retryAtMs_ = stateSinceMs_ + kRetryIntervalMs;
}

void MOWifi::reconnect() {
  WiFi.disconnect(false, false);
  state_ = State::Disconnected;
  stateSinceMs_ = millis();
  retryAtMs_ = stateSinceMs_;
  connect();
}

void MOWifi::restart() {
  WiFi.disconnect(false, false);
  state_ = State::Disconnected;
  stateSinceMs_ = millis();
  retryAtMs_ = stateSinceMs_;
  connect();
}

bool MOWifi::connected() const noexcept {
  return state_ == State::Connected && WiFi.status() == WL_CONNECTED;
}

MOWifi::State MOWifi::state() const noexcept {
  return state_;
}

void MOWifi::setCredentials(const std::string& ssid,
                            const std::string& password) {
  ssid_ = ssid;
  password_ = password;
}

void MOWifi::startConnection() {
  if (ssid_.empty()) {
    return;
  }

  WiFi.begin(ssid_.c_str(), password_.c_str());

  state_ = State::Connecting;
  Serial.println("[MO] WiFi connecting");
  stateSinceMs_ = millis();
}

void MOWifi::handleConnecting(uint32_t now) {
  if (WiFi.status() == WL_CONNECTED) {
    state_ = State::Connected;
    stateSinceMs_ = now;
    Serial.println("[MO] WiFi connected");
    return;
  }

  if (now - stateSinceMs_ >= kConnectTimeoutMs) {
    WiFi.disconnect(false, false);
    state_ = State::Disconnected;
    Serial.println("[MO] WiFi connect timeout");
    stateSinceMs_ = now;
    retryAtMs_ = now + kRetryIntervalMs;
  }
}

void MOWifi::handleConnected(uint32_t now) {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  state_ = State::Disconnected;
  stateSinceMs_ = now;
  retryAtMs_ = now;
  Serial.println("[MO] WiFi disconnected");
}

void MOWifi::handleDisconnected(uint32_t now) {
  if (ssid_.empty()) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    state_ = State::Connected;
    stateSinceMs_ = now;
    return;
  }

  if (static_cast<int32_t>(now - retryAtMs_) >= 0) {
    startConnection();
  }
}
