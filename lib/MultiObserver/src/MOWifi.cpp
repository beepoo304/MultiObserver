#include "MOWifi.h"

#include "MOLocalTime.h"

#include <Arduino.h>
#include <esp_sntp.h>

#include <cstdio>
#include <ctime>

namespace {

const char* wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:
      return "idle";
    case WL_NO_SSID_AVAIL:
      return "no_ssid";
    case WL_SCAN_COMPLETED:
      return "scan_completed";
    case WL_CONNECTED:
      return "connected";
    case WL_CONNECT_FAILED:
      return "connect_failed";
    case WL_CONNECTION_LOST:
      return "connection_lost";
    case WL_DISCONNECTED:
      return "disconnected";
    default:
      return "unknown";
  }
}

}  // namespace

MOWifi::MOWifi() = default;

void MOWifi::begin(const std::string& ssid, const std::string& password) {
  MOLocalTime::configure();
  setCredentials(ssid, password);
  Serial.printf("[MO][WiFi] start t=%lu ssid=%s\n",
                static_cast<unsigned long>(millis()),
                ssid_.empty() ? "-" : ssid_.c_str());

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);

  state_ = State::Disconnected;
  stateSinceMs_ = millis();
  retryAtMs_ = stateSinceMs_;
  lastWifiStatus_ = -1;
  resetTimeSync();

  connect();
}

void MOWifi::loop() {
  const uint32_t now = millis();
  logWifiStatusTransition(now);

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

  updateTimeSync();
}

void MOWifi::connect() {
  if (ssid_.empty()) {
    disconnect();
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    markConnected(millis());
    return;
  }

  startConnection();
}

void MOWifi::disconnect() {
  Serial.printf("[MO][WiFi] disconnect requested t=%lu\n",
                static_cast<unsigned long>(millis()));
  WiFi.disconnect(false, false);
  state_ = State::Disconnected;
  stateSinceMs_ = millis();
  retryAtMs_ = stateSinceMs_ + kRetryIntervalMs;
  resetTimeSync();
}

void MOWifi::reconnect() {
  Serial.printf("[MO][WiFi] reconnect requested t=%lu\n",
                static_cast<unsigned long>(millis()));
  WiFi.disconnect(false, false);
  state_ = State::Disconnected;
  stateSinceMs_ = millis();
  retryAtMs_ = stateSinceMs_;
  resetTimeSync();
  connect();
}

void MOWifi::restart() {
  Serial.printf("[MO][WiFi] restart requested t=%lu\n",
                static_cast<unsigned long>(millis()));
  WiFi.disconnect(false, false);
  state_ = State::Disconnected;
  stateSinceMs_ = millis();
  retryAtMs_ = stateSinceMs_;
  resetTimeSync();
  connect();
}

bool MOWifi::connected() const noexcept {
  return state_ == State::Connected && WiFi.status() == WL_CONNECTED;
}

bool MOWifi::healthy() const noexcept {
  if (!connected()) {
    return false;
  }

  const IPAddress localIp = WiFi.localIP();
  const IPAddress gateway = WiFi.gatewayIP();
  return localIp != IPAddress(0, 0, 0, 0) &&
         gateway != IPAddress(0, 0, 0, 0);
}

bool MOWifi::hasTimeSync() const noexcept {
  return connected() && timeSynced_;
}

MOWifi::State MOWifi::state() const noexcept {
  return state_;
}

void MOWifi::formatStatus(char* buffer, size_t bufferSize) const noexcept {
  if (buffer == nullptr || bufferSize == 0) {
    return;
  }

  const wl_status_t status = WiFi.status();
  if (connected()) {
    std::snprintf(
        buffer, bufferSize,
        "CONNECTED %s ip=%s ch=%d rssi=%d ntp=%s code=%d",
        ssid_.c_str(), WiFi.localIP().toString().c_str(), WiFi.channel(),
        WiFi.RSSI(), timeSynced_ ? "SYNCED" : "WAIT",
        static_cast<int>(status));
    return;
  }

  const char* lifecycle = state_ == State::Connecting ? "CONNECTING" : "DISCONNECTED";
  std::snprintf(buffer, bufferSize, "%s %s code=%d state=%s ntp=WAIT",
                lifecycle, ssid_.empty() ? "-" : ssid_.c_str(),
                static_cast<int>(status), wifiStatusName(status));
}

void MOWifi::setCredentials(const std::string& ssid,
                            const std::string& password) {
  ssid_ = ssid;
  password_ = password;
}

void MOWifi::startConnection() {
  if (ssid_.empty()) {
    Serial.println("[MO][WiFi] unconfigured: SSID is empty");
    return;
  }

  WiFi.begin(ssid_.c_str(), password_.c_str());

  state_ = State::Connecting;
  stateSinceMs_ = millis();
  Serial.printf("[MO][WiFi] begin t=%lu ssid=%s channel=scan timeout_ms=%lu\n",
                static_cast<unsigned long>(stateSinceMs_), ssid_.c_str(),
                static_cast<unsigned long>(kConnectTimeoutMs));
}

void MOWifi::handleConnecting(uint32_t now) {
  if (WiFi.status() == WL_CONNECTED) {
    markConnected(now);
    return;
  }

  if (now - stateSinceMs_ >= kConnectTimeoutMs) {
    WiFi.disconnect(false, false);
    state_ = State::Disconnected;
    const wl_status_t status = WiFi.status();
    Serial.printf(
        "[MO][WiFi] timeout t=%lu elapsed_ms=%lu code=%d state=%s retry_ms=%lu\n",
        static_cast<unsigned long>(now),
        static_cast<unsigned long>(now - stateSinceMs_),
        static_cast<int>(status), wifiStatusName(status),
        static_cast<unsigned long>(kRetryIntervalMs));
    stateSinceMs_ = now;
    retryAtMs_ = now + kRetryIntervalMs;
    resetTimeSync();
  }
}

void MOWifi::handleConnected(uint32_t now) {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  state_ = State::Disconnected;
  stateSinceMs_ = now;
  retryAtMs_ = now;
  const wl_status_t status = WiFi.status();
  Serial.printf("[MO][WiFi] link lost t=%lu code=%d state=%s\n",
                static_cast<unsigned long>(now), static_cast<int>(status),
                wifiStatusName(status));
  resetTimeSync();
}

void MOWifi::handleDisconnected(uint32_t now) {
  if (ssid_.empty()) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    markConnected(now);
    return;
  }

  if (static_cast<int32_t>(now - retryAtMs_) >= 0) {
    startConnection();
  }
}

void MOWifi::markConnected(uint32_t now) {
  const bool announce = state_ != State::Connected;
  state_ = State::Connected;
  stateSinceMs_ = now;
  retryAtMs_ = 0;
  if (announce) {
    Serial.printf(
        "[MO][WiFi] connected t=%lu ip=%s channel=%d rssi=%d\n",
        static_cast<unsigned long>(now), WiFi.localIP().toString().c_str(),
        WiFi.channel(), WiFi.RSSI());
  }
}

void MOWifi::updateTimeSync() {
  if (!connected()) {
    if (timeSynced_) {
      Serial.printf("[MO][NTP] lost t=%lu\n",
                    static_cast<unsigned long>(millis()));
    }
    resetTimeSync();
    return;
  }

  if (!timeSyncStarted_) {
    configTzTime(MOLocalTime::timezone(), "pool.ntp.org", "time.google.com",
                 "time.cloudflare.com");
    timeSyncStarted_ = true;
    Serial.printf("[MO][NTP] start t=%lu\n",
                  static_cast<unsigned long>(millis()));
  }

  const time_t now = time(nullptr);
  const sntp_sync_status_t syncStatus = sntp_get_sync_status();
  const bool ready =
      now >= MOLocalTime::kMinSaneEpoch &&
      (syncStatus == SNTP_SYNC_STATUS_COMPLETED ||
       syncStatus == SNTP_SYNC_STATUS_IN_PROGRESS);
  if (ready && !timeSynced_) {
    timeSynced_ = true;
    Serial.printf("[MO][NTP] synced t=%lu epoch=%lld status=%d\n",
                  static_cast<unsigned long>(millis()),
                  static_cast<long long>(now), static_cast<int>(syncStatus));
  }
}

void MOWifi::resetTimeSync() {
  timeSyncStarted_ = false;
  timeSynced_ = false;
}

void MOWifi::logWifiStatusTransition(uint32_t now) {
  const int status = static_cast<int>(WiFi.status());
  if (status == lastWifiStatus_) {
    return;
  }
  lastWifiStatus_ = status;
  Serial.printf("[MO][WiFi] status t=%lu code=%d state=%s\n",
                static_cast<unsigned long>(now), status,
                wifiStatusName(static_cast<wl_status_t>(status)));
}
