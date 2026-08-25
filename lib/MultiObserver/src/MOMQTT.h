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
    std::string_view batteryMv;
    std::string_view uptimeSecs;
    std::string_view errors;
    std::string_view queueLen;
    std::string_view noiseFloor;
    std::string_view txAirSecs;
    std::string_view rxAirSecs;
    std::string_view recvErrors;
    std::string_view packetsSent;
    std::string_view packetsReceived;
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

  void setObserverIdentity(std::string_view originId);\n\n  bool publishPacket(const PacketData& packet);
  bool publishRaw(const RawData& raw);
  bool publishStatus(const StatusData& status, bool retain = true);

  bool queuePublish(BrokerId broker, std::string_view topic,
                    std::string_view payload, bool retain = false);

 private:
  static constexpr size_t kPacketJsonBufferSize = 1280;
  static constexpr size_t kRawJsonBufferSize = 896;
  static constexpr size_t kStatusJsonBufferSize = 768;

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

  MOMQTTPrefs& prefs_;
  MOWifi& wifi_;
  std::array<BrokerRuntime, 2> brokers_{};
  bool running_{false};
};
