#pragma once

#include <Arduino.h>
#include <WiFi.h>

#include <cstdint>
#include <string>

class MOWifi {
 public:
  enum class State : uint8_t {
    Disconnected,
    Connecting,
    Connected,
  };

  MOWifi();

  void begin(const std::string& ssid, const std::string& password);
  void loop();

  void connect();
  void disconnect();
  void reconnect();
  void restart();

  [[nodiscard]] bool connected() const noexcept;
  [[nodiscard]] State state() const noexcept;

  void setCredentials(const std::string& ssid, const std::string& password);

 private:
  static constexpr uint32_t kConnectTimeoutMs = 45'000;
  static constexpr uint32_t kRetryIntervalMs = 15'000;

  void startConnection();
  void handleConnecting(uint32_t now);
  void handleConnected(uint32_t now);
  void handleDisconnected(uint32_t now);

  std::string ssid_;
  std::string password_;

  State state_{State::Disconnected};
  uint32_t stateSinceMs_{0};
  uint32_t retryAtMs_{0};
};
