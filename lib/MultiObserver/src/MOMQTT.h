#pragma once

#include "MOMQTTPrefs.h"

#include <Arduino.h>
#include <mqtt_client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
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
    WaitingForTime,
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
    uint32_t lastPublishQueuedMs{0};
    uint32_t lastPublishConfirmedMs{0};
  };

  struct PacketData {
    std::string_view origin;
    std::string_view originId;
    std::string_view timestamp;
    std::string_view time;
    std::string_view date;
    std::string_view direction;
    std::string_view length;
    std::string_view packetType;
    std::string_view route;
    std::string_view payloadLength;
    std::string_view raw;
    std::string_view snr;
    std::string_view rssi;
    std::string_view hash;
    std::string_view score;
    std::string_view duration;
    std::string_view path;
  };

  struct RawData {
    std::string_view origin;
    std::string_view originId;
    std::string_view timestamp;
    std::string_view data;
  };

  struct StatusData {
    std::string_view status;
    std::string_view timestamp;
    std::string_view origin;
    std::string_view originId;
    std::string_view model;
    std::string_view firmwareVersion;
    std::string_view radio;
    std::string_view clientVersion;
    std::string_view repeat;
    uint32_t batteryMv{0};
    uint32_t uptimeSecs{0};
    uint32_t errors{0};
    uint32_t queueLen{0};
    int32_t noiseFloor{0};
    uint32_t txAirSecs{0};
    uint32_t rxAirSecs{0};
    uint32_t recvErrors{0};
    uint32_t packetsSent{0};
    uint32_t packetsReceived{0};
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
  [[nodiscard]] bool configured(BrokerId broker) const noexcept;

  void setObserverIdentity(std::string_view originId);

  bool publishPacket(const PacketData& packet);
  bool publishRaw(const RawData& raw);
  bool publishStatus(const StatusData& status, bool retain = true);
  void setStatusSnapshot(const StatusData& status);

  bool queuePublish(BrokerId broker, std::string_view topic,
                    std::string_view payload, bool retain = false);

 private:
  static constexpr size_t kPacketJsonBufferSize = 1280;
  static constexpr size_t kRawJsonBufferSize = 896;
  static constexpr size_t kStatusJsonBufferSize = 768;
  static constexpr uint32_t kKeepAliveSeconds = 30;
  static constexpr uint32_t kConnectTimeoutMs = 10'000;
  static constexpr uint32_t kRetryBaseMs = 10'000;
  static constexpr uint32_t kRetryMaxMs = 300'000;
  static constexpr uint32_t kStatusIntervalMs = 300'000;
  static constexpr size_t kMqttBufferSize = 768;
  static constexpr size_t kMqttOutBufferSize = 1280;

  struct BrokerRuntime {
    esp_mqtt_client_handle_t client{nullptr};
    State state{State::Disabled};
    uint32_t lastConnectAttemptMs{0};
    uint32_t nextConnectAttemptMs{0};
    uint32_t reconnectFailures{0};
    esp_err_t lastError{ESP_OK};
    bool started{false};
    bool reconnectPending{false};
    bool forcedEnabled{false};
    uint32_t connectedSinceMs{0};
    uint32_t lastPublishQueuedMs{0};
    uint32_t lastPublishConfirmedMs{0};
  };

  static void onMqttEvent(void* handlerArg, esp_event_base_t eventBase,
                          int32_t eventId, void* eventData);

  void handleMqttEvent(BrokerId broker, esp_mqtt_event_handle_t event);
  void ensureBroker(BrokerId broker, uint32_t now);
  bool startBroker(BrokerId broker, uint32_t now);
  void destroyBroker(BrokerId broker);
  void disconnectRuntime(BrokerId broker, bool clearForced);
  void scheduleRetry(BrokerId broker, uint32_t now);
  void clearRuntime(BrokerId broker);
  bool hasConnectHeadroom(BrokerId broker) const;
  bool isWiFiReady() const;
  bool isNetworkReady() const;
  State networkWaitState() const;

  bool buildClientConfig(BrokerId broker, esp_mqtt_client_config_t& config,
                         std::string& uri) const;
  bool publishStoredStatus(bool online);

  bool buildTopic(std::string_view leaf, char* buffer, size_t bufferSize) const;

  static bool buildPacketJson(const PacketData& packet, char* buffer,
                              size_t bufferSize, size_t& length);
  static bool buildRawJson(const RawData& raw, char* buffer, size_t bufferSize,
                           size_t& length);
  static bool buildStatusJson(const StatusData& status, char* buffer,
                              size_t bufferSize, size_t& length);

  static bool appendJsonString(char* buffer, size_t bufferSize, size_t& used,
                               std::string_view key, std::string_view value,
                               bool comma);
  static bool appendJsonLiteral(char* buffer, size_t bufferSize, size_t& used,
                                std::string_view key, std::string_view value,
                                bool comma);
  static bool appendEscaped(char* buffer, size_t bufferSize, size_t& used,
                            std::string_view value);

  static BrokerRuntime& runtime(std::array<BrokerRuntime, 2>& runtimes,
                                BrokerId broker) noexcept;
  static const BrokerRuntime& runtime(
      const std::array<BrokerRuntime, 2>& runtimes,
      BrokerId broker) noexcept;

  static const MOBrokerPrefs& prefs(const MOMQTTPrefs& prefs,
                                    BrokerId broker) noexcept;

  static size_t index(BrokerId broker) noexcept;
  static uint32_t retryDelayMs(uint32_t failures) noexcept;
  static const char* brokerName(BrokerId broker) noexcept;

  std::string observerId_;
  MOMQTTPrefs& prefs_;
  MOWifi& wifi_;
  std::array<BrokerRuntime, 2> brokers_{};
  struct StoredStatus {
    std::string status;
    std::string origin;
    std::string originId;
    std::string model;
    std::string firmwareVersion;
    std::string radio;
    std::string clientVersion;
    std::string repeat;
    uint32_t batteryMv{0};
    uint32_t uptimeSecs{0};
    uint32_t errors{0};
    uint32_t queueLen{0};
    int32_t noiseFloor{0};
    uint32_t txAirSecs{0};
    uint32_t rxAirSecs{0};
    uint32_t recvErrors{0};
    uint32_t packetsSent{0};
    uint32_t packetsReceived{0};
    bool valid{false};
  } statusSnapshot_{};
  uint32_t lastStatusPublishMs_{0};
  bool running_{false};
};
