#include "MOMQTT.h"

#include "MOWifi.h"

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

namespace {

const char* transportName(MOTransport transport) {
  return transport == MOTransport::Wss ? "wss" : "tcp";
}

bool isEnabled(const MOBrokerPrefs& prefs) {
  return prefs.state == MOOnOff::On && !prefs.host.empty() && prefs.port != 0;
}

bool appendChar(char* buffer, size_t bufferSize, size_t& used, char value) {
  if (used + 1 >= bufferSize) {
    return false;
  }
  buffer[used++] = value;
  buffer[used] = '\0';
  return true;
}

}  // namespace

MOMQTT::MOMQTT(MOMQTTPrefs& prefs, MOWifi& wifi)
    : prefs_(prefs), wifi_(wifi) {}

MOMQTT::~MOMQTT() {
  end();
}

void MOMQTT::begin() {
  if (running_) return;
  brokers_.fill({});
  running_ = true;
}

void MOMQTT::end() {
  if (!running_) return;
  destroyBroker(BrokerId::Mqtt1);
  destroyBroker(BrokerId::Mqtt2);
  running_ = false;
}

void MOMQTT::loop() {
  if (!running_) return;
  const uint32_t now = millis();

  if (!isWiFiReady()) {
    for (BrokerId broker : {BrokerId::Mqtt1, BrokerId::Mqtt2}) {
      BrokerRuntime& state = runtime(brokers_, broker);
      if (state.client != nullptr) destroyBroker(broker);
      state.state = isEnabled(prefs(prefs_, broker))
                        ? State::WaitingForWiFi
                        : State::Disabled;
    }
    return;
  }

  bool connectStarted = false;
  for (BrokerId broker : {BrokerId::Mqtt1, BrokerId::Mqtt2}) {
    BrokerRuntime& state = runtime(brokers_, broker);

    if (!isEnabled(prefs(prefs_, broker))) {
      if (state.client != nullptr) destroyBroker(broker);
      state.state = State::Disabled;
      continue;
    }

    if (state.client != nullptr && state.state == State::Connecting &&
        now - state.lastConnectAttemptMs >= kConnectTimeoutMs) {
      destroyBroker(broker);
      scheduleRetry(broker, now);
    }

    if (state.client == nullptr && !connectStarted) {
      const State previous = state.state;
      ensureBroker(broker, now);
      if (previous != State::Connected && state.client != nullptr &&
          state.state == State::Connecting) {
        connectStarted = true;
      }
    }
  }
}

void MOMQTT::connect(BrokerId broker) {
  if (!running_ || !isWiFiReady() || !isEnabled(prefs(prefs_, broker))) return;
  BrokerRuntime& state = runtime(brokers_, broker);
  state.reconnectPending = true;
  state.nextConnectAttemptMs = millis();
  state.state = State::Disconnected;
}

void MOMQTT::disconnect(BrokerId broker) {
  BrokerRuntime& state = runtime(brokers_, broker);
  if (state.client != nullptr) {
    esp_mqtt_client_stop(state.client);
    destroyBroker(broker);
  } else {
    clearRuntime(broker);
  }
  if (isEnabled(prefs(prefs_, broker))) {
    state.state = isWiFiReady() ? State::Disconnected : State::WaitingForWiFi;
  }
}

void MOMQTT::reconnect(BrokerId broker) {
  disconnect(broker);
  connect(broker);
}

void MOMQTT::restart(BrokerId broker) {
  reconnect(broker);
}

MOMQTT::BrokerStatus MOMQTT::status(BrokerId broker) const noexcept {
  const BrokerRuntime& state = runtime(brokers_, broker);
  return {.state = state.state,
          .reconnectFailures = state.reconnectFailures,
          .lastError = state.lastError};
}

bool MOMQTT::connected(BrokerId broker) const noexcept {
  return runtime(brokers_, broker).state == State::Connected;
}

void MOMQTT::setObserverIdentity(std::string_view originId) {
  observerId_.assign(originId.data(), originId.size());
}

bool MOMQTT::publishPacket(const PacketData& packet) {
  char payload[kPacketJsonBufferSize];
  size_t length = 0;
  if (!buildPacketJson(packet, payload, sizeof(payload), length)) return false;

  char topic[192];
  if (!buildTopic("packets", topic, sizeof(topic))) return false;

  bool queued = false;
  for (BrokerId broker : {BrokerId::Mqtt1, BrokerId::Mqtt2}) {
    queued |= queuePublish(broker, topic, std::string_view(payload, length),
                            false);
  }
  return queued;
}

bool MOMQTT::publishRaw(const RawData& raw) {
  char payload[kRawJsonBufferSize];
  size_t length = 0;
  if (!buildRawJson(raw, payload, sizeof(payload), length)) return false;

  char topic[192];
  if (!buildTopic("raw", topic, sizeof(topic))) return false;

  bool queued = false;
  for (BrokerId broker : {BrokerId::Mqtt1, BrokerId::Mqtt2}) {
    queued |= queuePublish(broker, topic, std::string_view(payload, length),
                            false);
  }
  return queued;
}

bool MOMQTT::publishStatus(const StatusData& status, bool retain) {
  char payload[kStatusJsonBufferSize];
  size_t length = 0;
  if (!buildStatusJson(status, payload, sizeof(payload), length)) return false;

  char topic[192];
  if (!buildTopic("status", topic, sizeof(topic))) return false;

  bool queued = false;
  for (BrokerId broker : {BrokerId::Mqtt1, BrokerId::Mqtt2}) {
    queued |= queuePublish(broker, topic, std::string_view(payload, length),
                            retain);
  }
  return queued;
}

bool MOMQTT::queuePublish(BrokerId broker, std::string_view topic,
                          std::string_view payload, bool retain) {
  const BrokerRuntime& state = runtime(brokers_, broker);
  if (state.client == nullptr || state.state != State::Connected ||
      topic.empty() || payload.empty()) return false;

  std::string topicCopy(topic);
  std::string payloadCopy(payload);
  const int rc = esp_mqtt_client_enqueue(
      state.client, topicCopy.c_str(), payloadCopy.c_str(), 0, 1,
      retain ? 1 : 0, true);
  return rc >= 0;
}

void MOMQTT::onMqttEvent(void* handlerArg, esp_event_base_t eventBase,
                         int32_t eventId, void* eventData) {
  (void)eventBase;
  (void)eventId;
  if (handlerArg == nullptr || eventData == nullptr) return;

  auto* self = static_cast<MOMQTT*>(handlerArg);
  auto* event = static_cast<esp_mqtt_event_handle_t>(eventData);

  for (BrokerId broker : {BrokerId::Mqtt1, BrokerId::Mqtt2}) {
    if (self->runtime(self->brokers_, broker).client == event->client) {
      self->handleMqttEvent(broker, event);
      return;
    }
  }
}

void MOMQTT::handleMqttEvent(BrokerId broker,
                             esp_mqtt_event_handle_t event) {
  BrokerRuntime& state = runtime(brokers_, broker);
  switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
      state.state = State::Connected;
      state.reconnectFailures = 0;
      state.lastError = ESP_OK;
      state.reconnectPending = false;
      state.nextConnectAttemptMs = 0;
      break;
    case MQTT_EVENT_DISCONNECTED:
      state.state = State::Backoff;
      scheduleRetry(broker, millis());
      break;
    case MQTT_EVENT_ERROR:
      state.lastError = event->error_handle != nullptr
                            ? event->error_handle->esp_tls_last_esp_err
                            : ESP_FAIL;
      state.state = State::Error;
      scheduleRetry(broker, millis());
      break;
    default:
      break;
  }
}

void MOMQTT::ensureBroker(BrokerId broker, uint32_t now) {
  BrokerRuntime& state = runtime(brokers_, broker);
  if (state.client != nullptr) return;
  if (state.nextConnectAttemptMs != 0 &&
      static_cast<int32_t>(now - state.nextConnectAttemptMs) < 0) return;
  if (!state.reconnectPending && state.state != State::Disconnected &&
      state.state != State::Backoff && state.state != State::Error) return;

  if (!hasConnectHeadroom(broker)) {
    state.state = State::Backoff;
    state.reconnectPending = true;
    state.nextConnectAttemptMs = now + kRetryBaseMs;
    return;
  }

  state.reconnectPending = false;
  state.lastConnectAttemptMs = now;
  state.state = State::Connecting;
  if (!startBroker(broker, now)) scheduleRetry(broker, now);
}

bool MOMQTT::startBroker(BrokerId broker, uint32_t now) {
  BrokerRuntime& state = runtime(brokers_, broker);
  esp_mqtt_client_config_t config{};
  std::string uri;

  if (!buildClientConfig(broker, config, uri)) {
    state.lastError = ESP_ERR_INVALID_ARG;
    return false;
  }

  esp_mqtt_client_handle_t client = esp_mqtt_client_init(&config);
  if (client == nullptr) {
    state.lastError = ESP_ERR_NO_MEM;
    return false;
  }

  esp_err_t rc = esp_mqtt_client_register_event(
      client, MQTT_EVENT_ANY, &MOMQTT::onMqttEvent, this);
  if (rc != ESP_OK) {
    esp_mqtt_client_destroy(client);
    state.lastError = rc;
    return false;
  }

  state.client = client;
  state.started = true;
  state.state = State::Connecting;
  state.lastConnectAttemptMs = now;

  rc = esp_mqtt_client_start(client);
  if (rc != ESP_OK) {
    state.lastError = rc;
    destroyBroker(broker);
    return false;
  }
  return true;
}

void MOMQTT::destroyBroker(BrokerId broker) {
  BrokerRuntime& state = runtime(brokers_, broker);
  if (state.client != nullptr) {
    esp_mqtt_client_stop(state.client);
    esp_mqtt_client_destroy(state.client);
  }
  clearRuntime(broker);
}

void MOMQTT::scheduleRetry(BrokerId broker, uint32_t now) {
  BrokerRuntime& state = runtime(brokers_, broker);
  state.reconnectFailures =
      std::min<uint32_t>(state.reconnectFailures + 1, 31);
  state.reconnectPending = true;
  state.state = State::Backoff;
  state.nextConnectAttemptMs = now + retryDelayMs(state.reconnectFailures);
}

void MOMQTT::clearRuntime(BrokerId broker) {
  BrokerRuntime& state = runtime(brokers_, broker);
  state.client = nullptr;
  state.started = false;
  state.reconnectPending = false;
  state.nextConnectAttemptMs = 0;
}

bool MOMQTT::hasConnectHeadroom(BrokerId broker) const {
  const bool otherConnected =
      broker == BrokerId::Mqtt1 ? connected(BrokerId::Mqtt2)
                               : connected(BrokerId::Mqtt1);
  if (!otherConnected) return true;

  const size_t freeHeap =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t largestHeap =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  constexpr size_t kDualBrokerMinFreeHeap = 80U * 1024U;
  constexpr size_t kDualBrokerMinLargestHeap = 32U * 1024U;
  return freeHeap >= kDualBrokerMinFreeHeap &&
         largestHeap >= kDualBrokerMinLargestHeap;
}

bool MOMQTT::isWiFiReady() const {
  return wifi_.connected();
}

bool MOMQTT::buildClientConfig(BrokerId broker,
                               esp_mqtt_client_config_t& config,
                               std::string& uri) const {
  const MOBrokerPrefs& brokerPrefs = prefs(prefs_, broker);
  if (!isEnabled(brokerPrefs)) return false;

#if ESP_IDF_VERSION_MAJOR >= 5
  config.credentials.username =
      brokerPrefs.username.empty() ? nullptr : brokerPrefs.username.c_str();
  config.credentials.client_id = nullptr;
  config.credentials.authentication.password =
      brokerPrefs.password.empty() ? nullptr : brokerPrefs.password.c_str();
  config.session.keepalive = kKeepAliveSeconds;
  config.network.reconnect_timeout_ms = kConnectTimeoutMs;
  config.network.timeout_ms = kConnectTimeoutMs;
  config.network.disable_auto_reconnect = true;
  config.buffer.size = kMqttBufferSize;
  config.buffer.out_size = kMqttOutBufferSize;

  if (brokerPrefs.transport == MOTransport::Wss) {
    uri = "wss://" + brokerPrefs.host + ":" +
          std::to_string(brokerPrefs.port) + "/mqtt";
    config.broker.address.uri = uri.c_str();
    config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
  } else {
    config.broker.address.hostname = brokerPrefs.host.c_str();
    config.broker.address.port = brokerPrefs.port;
    config.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
  }
#else
  config.username =
      brokerPrefs.username.empty() ? nullptr : brokerPrefs.username.c_str();
  config.password =
      brokerPrefs.password.empty() ? nullptr : brokerPrefs.password.c_str();
  config.client_id = nullptr;
  config.keepalive = kKeepAliveSeconds;
  config.reconnect_timeout_ms = kConnectTimeoutMs;
  config.network_timeout_ms = kConnectTimeoutMs;
  config.disable_auto_reconnect = true;
  config.buffer_size = kMqttBufferSize;
  config.out_buffer_size = kMqttOutBufferSize;

  if (brokerPrefs.transport == MOTransport::Wss) {
    uri = "wss://" + brokerPrefs.host + ":" +
          std::to_string(brokerPrefs.port) + "/mqtt";
    config.uri = uri.c_str();
    config.crt_bundle_attach = esp_crt_bundle_attach;
  } else {
    config.host = brokerPrefs.host.c_str();
    config.port = brokerPrefs.port;
    config.transport = MQTT_TRANSPORT_OVER_TCP;
  }
#endif
  return true;
}

bool MOMQTT::buildTopic(std::string_view leaf, char* buffer,
                        size_t bufferSize) const {
  const std::string& iata = prefs_.iata();
  if (iata.empty() || observerId_.empty() || leaf.empty()) {
    return false;
  }

  const int written = std::snprintf(
      buffer, bufferSize, "meshcore/%.*s/%.*s/%.*s",
      static_cast<int>(iata.size()), iata.data(),
      static_cast<int>(observerId_.size()), observerId_.data(),
      static_cast<int>(leaf.size()), leaf.data());

  return written >= 0 && static_cast<size_t>(written) < bufferSize;
}

bool MOMQTT::appendEscaped(char* buffer, size_t bufferSize, size_t& used,
                           std::string_view value) {
  for (char c : value) {
    switch (c) {
      case '"':
        if (!appendChar(buffer, bufferSize, used, '\\') ||
            !appendChar(buffer, bufferSize, used, '"')) return false;
        break;
      case '\\':
        if (!appendChar(buffer, bufferSize, used, '\\') ||
            !appendChar(buffer, bufferSize, used, '\\')) return false;
        break;
      case '\b':
        if (!appendChar(buffer, bufferSize, used, '\\') ||
            !appendChar(buffer, bufferSize, used, 'b')) return false;
        break;
      case '\f':
        if (!appendChar(buffer, bufferSize, used, '\\') ||
            !appendChar(buffer, bufferSize, used, 'f')) return false;
        break;
      case '\n':
        if (!appendChar(buffer, bufferSize, used, '\\') ||
            !appendChar(buffer, bufferSize, used, 'n')) return false;
        break;
      case '\r':
        if (!appendChar(buffer, bufferSize, used, '\\') ||
            !appendChar(buffer, bufferSize, used, 'r')) return false;
        break;
      case '\t':
        if (!appendChar(buffer, bufferSize, used, '\\') ||
            !appendChar(buffer, bufferSize, used, 't')) return false;
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) return false;
        if (!appendChar(buffer, bufferSize, used, c)) return false;
        break;
    }
  }
  return true;
}

bool MOMQTT::appendJsonString(char* buffer, size_t bufferSize, size_t& used,
                              std::string_view key, std::string_view value,
                              bool comma) {
  if (comma && !appendChar(buffer, bufferSize, used, ',')) return false;
  if (!appendChar(buffer, bufferSize, used, '"')) return false;
  if (!appendEscaped(buffer, bufferSize, used, key)) return false;
  if (!appendChar(buffer, bufferSize, used, '"') ||
      !appendChar(buffer, bufferSize, used, ':') ||
      !appendChar(buffer, bufferSize, used, '"')) return false;
  if (!appendEscaped(buffer, bufferSize, used, value)) return false;
  return appendChar(buffer, bufferSize, used, '"');
}

bool MOMQTT::appendJsonLiteral(char* buffer, size_t bufferSize, size_t& used,
                               std::string_view key, std::string_view value,
                               bool comma) {
  if (comma && !appendChar(buffer, bufferSize, used, ',')) return false;
  if (!appendChar(buffer, bufferSize, used, '"')) return false;
  if (!appendEscaped(buffer, bufferSize, used, key)) return false;
  if (!appendChar(buffer, bufferSize, used, '"') ||
      !appendChar(buffer, bufferSize, used, ':')) return false;
  return appendEscaped(buffer, bufferSize, used, value);
}

bool MOMQTT::buildPacketJson(const PacketData& p, char* buffer,
                             size_t bufferSize, size_t& length) {
  length = 0;
  if (!appendChar(buffer, bufferSize, length, '{')) return false;

  bool comma = false;
  const auto add = [&](std::string_view key, std::string_view value) {
    const bool ok = appendJsonString(buffer, bufferSize, length, key, value, comma);
    if (ok) comma = true;
    return ok;
  };

  if (!add("origin", p.origin) ||
      !add("origin_id", p.originId) ||
      !add("timestamp", p.timestamp) ||
      !add("type", "PACKET") ||
      !add("direction", p.direction) ||
      !add("time", p.time) ||
      !add("date", p.date) ||
      !add("len", p.length) ||
      !add("packet_type", p.packetType) ||
      !add("route", p.route) ||
      !add("payload_len", p.payloadLength) ||
      !add("raw", p.raw) ||
      !add("SNR", p.snr) ||
      !add("RSSI", p.rssi)) {
    return false;
  }

  if (!p.hash.empty() && !add("hash", p.hash)) return false;
  if (!p.score.empty() && !add("score", p.score)) return false;
  if (!p.duration.empty() && !add("duration", p.duration)) return false;
  if (!p.path.empty() && !add("path", p.path)) return false;

  return appendChar(buffer, bufferSize, length, '}');
}

bool MOMQTT::buildRawJson(const RawData& r, char* buffer, size_t bufferSize,
                          size_t& length) {
  length = 0;
  if (!appendChar(buffer, bufferSize, length, '{')) return false;
  bool comma = false;

  const auto add = [&](std::string_view key, std::string_view value) {
    const bool ok = appendJsonString(buffer, bufferSize, length, key, value, comma);
    if (ok) comma = true;
    return ok;
  };

  if (!add("origin", r.origin) ||
      !add("origin_id", r.originId) ||
      !add("timestamp", r.timestamp) ||
      !add("type", "RAW") ||
      !add("data", r.data)) {
    return false;
  }

  return appendChar(buffer, bufferSize, length, '}');
}

bool MOMQTT::buildStatusJson(const StatusData& s, char* buffer,
                             size_t bufferSize, size_t& length) {
  length = 0;
  if (!appendChar(buffer, bufferSize, length, '{')) return false;
  bool comma = false;

  const auto add = [&](std::string_view key, std::string_view value) {
    const bool ok = appendJsonString(buffer, bufferSize, length, key, value, comma);
    if (ok) comma = true;
    return ok;
  };

  if (!add("status", s.status) ||
      !add("timestamp", s.timestamp) ||
      !add("origin", s.origin) ||
      !add("origin_id", s.originId) ||
      !add("model", s.model) ||
      !add("firmware_version", s.firmwareVersion) ||
      !add("radio", s.radio) ||
      !add("client_version", s.clientVersion) ||
      !add("repeat", s.repeat)) {
    return false;
  }

  if (!appendChar(buffer, bufferSize, length, ',') ||
      !appendChar(buffer, bufferSize, length, '"') ||
      !appendEscaped(buffer, bufferSize, length, "stats") ||
      !appendChar(buffer, bufferSize, length, '"') ||
      !appendChar(buffer, bufferSize, length, ':') ||
      !appendChar(buffer, bufferSize, length, '{')) {
    return false;
  }

  bool statsComma = false;
  const auto addStat = [&](std::string_view key, std::string_view value) {
    if (value.empty()) return true;
    const bool ok =
        appendJsonString(buffer, bufferSize, length, key, value, statsComma);
    if (ok) statsComma = true;
    return ok;
  };

  if (!addStat("battery_mv", s.batteryMv) ||
      !addStat("uptime_secs", s.uptimeSecs) ||
      !addStat("errors", s.errors) ||
      !addStat("queue_len", s.queueLen) ||
      !addStat("noise_floor", s.noiseFloor) ||
      !addStat("tx_air_secs", s.txAirSecs) ||
      !addStat("rx_air_secs", s.rxAirSecs) ||
      !addStat("recv_errors", s.recvErrors) ||
      !addStat("packets_sent", s.packetsSent) ||
      !addStat("packets_received", s.packetsReceived) ||
      !appendChar(buffer, bufferSize, length, '}') ||
      !appendChar(buffer, bufferSize, length, '}')) {
    return false;
  }

  return true;
}

MOMQTT::BrokerRuntime& MOMQTT::runtime(
    std::array<BrokerRuntime, 2>& runtimes, BrokerId broker) noexcept {
  return runtimes[index(broker)];
}

const MOMQTT::BrokerRuntime& MOMQTT::runtime(
    const std::array<BrokerRuntime, 2>& runtimes,
    BrokerId broker) noexcept {
  return runtimes[index(broker)];
}

const MOBrokerPrefs& MOMQTT::prefs(const MOMQTTPrefs& prefs,
                                    BrokerId broker) noexcept {
  return broker == BrokerId::Mqtt1 ? prefs.mqtt1() : prefs.mqtt2();
}

size_t MOMQTT::index(BrokerId broker) noexcept {
  return static_cast<size_t>(broker);
}

uint32_t MOMQTT::retryDelayMs(uint32_t failures) noexcept {
  if (failures == 0) return kRetryBaseMs;
  constexpr uint32_t kMaxShift = 5;
  const uint32_t shift = std::min(failures - 1, kMaxShift);
  const uint64_t delay = static_cast<uint64_t>(kRetryBaseMs) << shift;
  return static_cast<uint32_t>(
      std::min<uint64_t>(delay, kRetryMaxMs));
}
