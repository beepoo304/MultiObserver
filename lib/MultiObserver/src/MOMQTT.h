#pragma once

#include "MOMQTTPrefs.h"

#include <Arduino.h>
#include <mqtt_client.h>

#include <array>
#include <cstdint>
#include <string_view>

class MOWifi;

class MOMQTT {
 public:
  enum class BrokerId : uint8_t {
    Mqtt1 = 0,
    Mqtt2 = 1,
  };

  enum class State : uint8_t {
    Disabled,
    WaitingForWiFi,
    Disconnected,
    Connecting,
    Connected,
    Backoff,
    Error,
  };

  struct BrokerStatus {
    State state{State::Disabled};
    uint32_t reconnectFailures{0};
    esp_err_t lastError{ESP_OK};
  };

  MOMQTT(MOMQTTPrefs& prefs, MOWifi& wifi);
  MOMQTT(const MOMQTT&) = delete;
  MOMQTT& operator=(const MOMQTT&) = delete;
  ~MOMQTT();

  void begin();
  void end();
  void loop();

  void connect(BrokerId broker);
  void disconnect(BrokerId broker);
  void reconnect(BrokerId broker);
  void restart(BrokerId broker);

  [[nodiscard]] BrokerStatus status(BrokerId broker) const noexcept;
  [[nodiscard]] bool connected(BrokerId broker) const noexcept;

  bool queuePublish(BrokerId broker, std::string_view topic,
                    std::string_view payload, bool retain = false);

 private:
  struct BrokerRuntime {
    esp_mqtt_client_handle_t client{nullptr};
    State state{State::Disabled};
    uint32_t lastConnectAttemptMs{0};
    uint32_t nextConnectAttemptMs{0};
    uint32_t reconnectFailures{0};
    esp_err_t lastError{ESP_OK};
    bool started{false};
    bool reconnectPending{false};
  };

  static constexpr uint32_t kRetryBaseMs = 10'000;
  static constexpr uint32_t kRetryMaxMs = 300'000;
  static constexpr uint32_t kConnectTimeoutMs = 10'000;
  static constexpr uint32_t kKeepAliveSeconds = 30;
  static constexpr size_t kMqttBufferSize = 768;
  static constexpr size_t kMqttOutBufferSize = 1280;

  static void onMqttEvent(void* handlerArg, esp_event_base_t eventBase,
                          int32_t eventId, void* eventData);

  void handleMqttEvent(BrokerId broker, esp_mqtt_event_handle_t event);
  void ensureBroker(BrokerId broker, uint32_t now);
  bool startBroker(BrokerId broker, uint32_t now);
  void destroyBroker(BrokerId broker);
  void scheduleRetry(BrokerId broker, uint32_t now);
  void clearRuntime(BrokerId broker);
  bool hasConnectHeadroom(BrokerId broker) const;
  bool isWiFiReady() const;

  bool buildClientConfig(BrokerId broker, esp_mqtt_client_config_t& config,
                          std::string& uri) const;

  static BrokerRuntime& runtime(std::array<BrokerRuntime, 2>& runtimes,
                                BrokerId broker) noexcept;
  static const BrokerRuntime& runtime(
      const std::array<BrokerRuntime, 2>& runtimes,
      BrokerId broker) noexcept;

  static const MOBrokerPrefs& prefs(const MOMQTTPrefs& prefs,
                                    BrokerId broker) noexcept;

  static size_t index(BrokerId broker) noexcept;
  static uint32_t retryDelayMs(uint32_t failures) noexcept;

  MOMQTTPrefs& prefs_;
  MOWifi& wifi_;
  std::array<BrokerRuntime, 2> brokers_{};
  bool running_{false};
};
