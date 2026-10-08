#include "MOMQTT.h"

#include "MOWifi.h"

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>

#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
extern "C" esp_err_t esp_crt_bundle_attach(void* conf);
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <ctime>

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
  eventQueue_ = xQueueCreate(16, sizeof(PendingEvent));
  if (eventQueue_ == nullptr) {
    Serial.println("[MO][MQTT] event queue allocation failed");
    return;
  }
  for (BrokerId broker : {BrokerId::Mqtt1, BrokerId::Mqtt2}) {
    eventContexts_[index(broker)] = {this, broker, 0};
    runtime(brokers_, broker).state = State::Disconnected;
  }
  eventOverflow_ = 0;
  running_ = true;
  Serial.println("[MO][MQTT1] runtime start");
  Serial.println("[MO][MQTT2] runtime start");
}

void MOMQTT::end() {
  if (!running_) return;
  if (statusSnapshot_.valid) {
    publishStoredStatus(false);
  }
  destroyBroker(BrokerId::Mqtt1);
  destroyBroker(BrokerId::Mqtt2);
  vQueueDelete(eventQueue_);
  eventQueue_ = nullptr;
  running_ = false;
}

void MOMQTT::loop() {
  if (!running_) {
    begin();
    if (!running_) return;
  }
  drainEvents();
  const uint32_t now = millis();

  if (!isNetworkReady()) {
    for (BrokerId broker : {BrokerId::Mqtt1, BrokerId::Mqtt2}) {
      BrokerRuntime& state = runtime(brokers_, broker);
      if (state.client != nullptr) destroyBroker(broker);
      const State next =
          (isEnabled(prefs(prefs_, broker)) || state.forcedEnabled)
              ? networkWaitState()
              : State::Disabled;
      if (state.state != next) {
        Serial.printf("[MO][%s] wait t=%lu wifi=%s ntp=%s\n",
                      brokerName(broker),
                      static_cast<unsigned long>(now),
                      isWiFiReady() ? "up" : "down",
                      wifi_.hasTimeSync() ? "synced" : "wait");
      }
      state.state = next;
    }
    return;
  }

  // Keep peak TLS heap usage bounded: finish the current broker handshake
  // before starting the other one.
  for (BrokerId broker : {BrokerId::Mqtt1, BrokerId::Mqtt2}) {
    BrokerRuntime& state = runtime(brokers_, broker);

    if (!isEnabled(prefs(prefs_, broker)) && !state.forcedEnabled) {
      if (state.client != nullptr) destroyBroker(broker);
      state.state = State::Disabled;
      continue;
    }

    if (state.state == State::WaitingForWiFi ||
        state.state == State::WaitingForTime) {
      state.state = State::Disconnected;
      state.reconnectPending = true;
      state.nextConnectAttemptMs = now;
      Serial.printf("[MO][%s] network ready t=%lu connect=scheduled\n",
                    brokerName(broker), static_cast<unsigned long>(now));
    }

    if (state.client != nullptr && state.state == State::Connecting &&
        now - state.lastConnectAttemptMs >= kConnectTimeoutMs) {
      Serial.printf("[MO][%s] connect timeout t=%lu elapsed_ms=%lu\n",
                    brokerName(broker), static_cast<unsigned long>(now),
                    static_cast<unsigned long>(now - state.lastConnectAttemptMs));
      destroyBroker(broker);
      scheduleRetry(broker, now);
    }

    // DISCONNECTED/ERROR do not destroy the ESP-MQTT handle. Auto reconnect
    // is disabled, so retire it here (never inside its callback). Preserve
    // the scheduled deadline/failure count to retain bounded backoff.
    if (state.client != nullptr && state.reconnectPending &&
        state.state != State::Connected) {
      const uint32_t retryAt = state.nextConnectAttemptMs;
      destroyBroker(broker);
      state.state = State::Backoff;
      state.reconnectPending = true;
      state.nextConnectAttemptMs = retryAt;
    }
    const bool connectStarted = std::any_of(
        brokers_.begin(), brokers_.end(), [](const BrokerRuntime& other) {
          return other.client != nullptr && other.state == State::Connecting;
        });
    if (state.client == nullptr && !connectStarted) {
      ensureBroker(broker, now);
    }
  }
  if (statusSnapshot_.valid &&
      static_cast<uint32_t>(now - lastStatusPublishMs_) >= kStatusIntervalMs) {
    if (publishStoredStatus(true)) {
      lastStatusPublishMs_ = now;
    }
  }
}

void MOMQTT::connect(BrokerId broker) {
  if (!running_ ||
      (!isEnabled(prefs(prefs_, broker)) &&
       !runtime(brokers_, broker).forcedEnabled)) {
    return;
  }
  BrokerRuntime& state = runtime(brokers_, broker);
  if (!isNetworkReady()) {
    state.state = networkWaitState();
    Serial.printf("[MO][%s] connect deferred wifi=%s ntp=%s\n",
                  brokerName(broker), isWiFiReady() ? "up" : "down",
                  wifi_.hasTimeSync() ? "synced" : "wait");
    return;
  }
  state.reconnectPending = true;
  state.nextConnectAttemptMs = millis();
  state.state = State::Disconnected;
}

void MOMQTT::disconnect(BrokerId broker) {
  disconnectRuntime(broker, true);
}

void MOMQTT::disconnectRuntime(BrokerId broker, bool clearForced) {
  BrokerRuntime& state = runtime(brokers_, broker);
  if (state.client != nullptr) {
    if (statusSnapshot_.valid && state.state == State::Connected) {
      publishStoredStatus(false);
    }
    destroyBroker(broker);
  } else {
    clearRuntime(broker);
  }

  if (clearForced) {
    state.forcedEnabled = false;
  }

  if (isEnabled(prefs(prefs_, broker)) || state.forcedEnabled) {
    state.state = isNetworkReady() ? State::Disconnected : networkWaitState();
  } else {
    state.state = State::Disabled;
  }
}

void MOMQTT::reconnect(BrokerId broker) {
  disconnectRuntime(broker, false);
  connect(broker);
}

void MOMQTT::restart(BrokerId broker) {
  BrokerRuntime& state = runtime(brokers_, broker);
  state.forcedEnabled = true;
  disconnectRuntime(broker, false);
  connect(broker);
}

MOMQTT::BrokerStatus MOMQTT::status(BrokerId broker) const noexcept {
  const BrokerRuntime& state = runtime(brokers_, broker);
  return {.state = state.state,
          .reconnectFailures = state.reconnectFailures,
          .lastError = state.lastError,
          .lastPublishQueuedMs = state.lastPublishQueuedMs,
          .lastPublishConfirmedMs = state.lastPublishConfirmedMs,
          .connectedSinceMs = state.connectedSinceMs};
}

bool MOMQTT::connected(BrokerId broker) const noexcept {
  return runtime(brokers_, broker).state == State::Connected;
}

bool MOMQTT::configured(BrokerId broker) const noexcept {
  return isEnabled(prefs(prefs_, broker));
}

bool MOMQTT::publishingHealthy(BrokerId broker) const noexcept {
  const BrokerRuntime& state = runtime(brokers_, broker);
  if (state.state != State::Connected || state.connectedSinceMs == 0) return false;
  const uint32_t now = millis();
  if (state.lastPublishConfirmedMs == 0) {
    // The first retained status is periodic. Give it one full interval plus a
    // small delivery margin before declaring a connected session stale.
    return now - state.connectedSinceMs <= kStatusIntervalMs + 60'000;
  }
  return now - state.lastPublishConfirmedMs <= (kStatusIntervalMs * 2U) + 60'000;
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
  setStatusSnapshot(status);
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

void MOMQTT::setStatusSnapshot(const StatusData& status) {
  statusSnapshot_.status.assign(status.status.data(), status.status.size());
  statusSnapshot_.origin.assign(status.origin.data(), status.origin.size());
  statusSnapshot_.originId.assign(status.originId.data(), status.originId.size());
  statusSnapshot_.model.assign(status.model.data(), status.model.size());
  statusSnapshot_.firmwareVersion.assign(status.firmwareVersion.data(),
                                         status.firmwareVersion.size());
  statusSnapshot_.radio.assign(status.radio.data(), status.radio.size());
  statusSnapshot_.clientVersion.assign(status.clientVersion.data(),
                                       status.clientVersion.size());
  statusSnapshot_.repeat.assign(status.repeat.data(), status.repeat.size());
  statusSnapshot_.batteryMv = status.batteryMv;
  statusSnapshot_.uptimeSecs = status.uptimeSecs;
  statusSnapshot_.errors = status.errors;
  statusSnapshot_.queueLen = status.queueLen;
  statusSnapshot_.noiseFloor = status.noiseFloor;
  statusSnapshot_.txAirSecs = status.txAirSecs;
  statusSnapshot_.rxAirSecs = status.rxAirSecs;
  statusSnapshot_.recvErrors = status.recvErrors;
  statusSnapshot_.packetsSent = status.packetsSent;
  statusSnapshot_.packetsReceived = status.packetsReceived;
  statusSnapshot_.valid = true;
}

bool MOMQTT::publishStoredStatus(bool online) {
  if (!statusSnapshot_.valid) {
    return false;
  }

  char timestamp[32]{};
  const time_t now = time(nullptr);
  const tm* utc = gmtime(&now);
  if (utc == nullptr ||
      std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", utc) == 0) {
    return false;
  }

  const std::string_view status = online ? "online" : "offline";
  const StatusData data{
      .status = status,
      .timestamp = timestamp,
      .origin = statusSnapshot_.origin,
      .originId = statusSnapshot_.originId,
      .model = statusSnapshot_.model,
      .firmwareVersion = statusSnapshot_.firmwareVersion,
      .radio = statusSnapshot_.radio,
      .clientVersion = statusSnapshot_.clientVersion,
      .repeat = statusSnapshot_.repeat,
      .batteryMv = statusSnapshot_.batteryMv,
      .uptimeSecs = statusSnapshot_.uptimeSecs,
      .errors = statusSnapshot_.errors,
      .queueLen = statusSnapshot_.queueLen,
      .noiseFloor = statusSnapshot_.noiseFloor,
      .txAirSecs = statusSnapshot_.txAirSecs,
      .rxAirSecs = statusSnapshot_.rxAirSecs,
      .recvErrors = statusSnapshot_.recvErrors,
      .packetsSent = statusSnapshot_.packetsSent,
      .packetsReceived = statusSnapshot_.packetsReceived,
  };
  return publishStatus(data, true);
}

bool MOMQTT::queuePublish(BrokerId broker, std::string_view topic,
                          std::string_view payload, bool retain) {
  BrokerRuntime& state = runtime(brokers_, broker);
  if (state.client == nullptr || state.state != State::Connected ||
      topic.empty() || payload.empty()) return false;

  std::string topicCopy(topic);
  std::string payloadCopy(payload);
  const int rc = esp_mqtt_client_enqueue(
      state.client, topicCopy.c_str(), payloadCopy.c_str(), 0, 1,
      retain ? 1 : 0, true);
  if (rc >= 0) {
    state.lastPublishQueuedMs = millis();
    return true;
  }
  return false;
}

void MOMQTT::onMqttEvent(void* handlerArg, esp_event_base_t eventBase,
                         int32_t eventId, void* eventData) {
  (void)eventBase;
  (void)eventId;
  if (handlerArg == nullptr || eventData == nullptr) return;

  auto* context = static_cast<EventContext*>(handlerArg);
  auto* self = context->owner;
  auto* event = static_cast<esp_mqtt_event_handle_t>(eventData);
  if (event->event_id != MQTT_EVENT_CONNECTED &&
      event->event_id != MQTT_EVENT_DISCONNECTED &&
      event->event_id != MQTT_EVENT_ERROR &&
      event->event_id != MQTT_EVENT_PUBLISHED &&
      event->event_id != MQTT_EVENT_BEFORE_CONNECT) return;
  PendingEvent copy{};
  copy.broker = context->broker;
  copy.generation = context->generation;
  copy.timestampMs = millis();
  copy.id = event->event_id;
  copy.hasError = event->event_id == MQTT_EVENT_ERROR && event->error_handle;
  if (copy.hasError) copy.error = *event->error_handle;
  if (xQueueSend(self->eventQueue_, &copy, 0) != pdTRUE) {
    self->eventOverflow_.fetch_or(1U << index(copy.broker));
  }
}

void MOMQTT::drainEvents() {
  const uint32_t overflow = eventOverflow_.exchange(0);
  for (BrokerId broker : {BrokerId::Mqtt1, BrokerId::Mqtt2}) {
    if ((overflow & (1U << index(broker))) == 0) continue;
    Serial.printf("[MO][%s] event overflow: recover client\n", brokerName(broker));
    destroyBroker(broker);
    scheduleRetry(broker, millis());
  }
  PendingEvent event{};
  while (xQueueReceive(eventQueue_, &event, 0) == pdTRUE) {
    if (event.generation != eventContexts_[index(event.broker)].generation ||
        runtime(brokers_, event.broker).client == nullptr) continue;
    handleMqttEvent(event);
  }
}

void MOMQTT::handleMqttEvent(const PendingEvent& event) {
  const BrokerId broker = event.broker;
  BrokerRuntime& state = runtime(brokers_, broker);
  const uint32_t now = event.timestampMs;
  const uint32_t connectedFor =
      state.connectedSinceMs == 0 ? 0 : now - state.connectedSinceMs;
  switch (event.id) {
    case MQTT_EVENT_CONNECTED:
      state.state = State::Connected;
      state.reconnectFailures = 0;
      state.lastError = ESP_OK;
      state.reconnectPending = false;
      state.nextConnectAttemptMs = 0;
      state.connectedSinceMs = now;
      Serial.printf(
          "[MO][%s] connected t=%lu free_heap=%u largest_heap=%u\n",
          brokerName(broker), static_cast<unsigned long>(now),
          static_cast<unsigned>(
              heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
          static_cast<unsigned>(heap_caps_get_largest_free_block(
              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
      if (statusSnapshot_.valid) {
        publishStoredStatus(true);
        lastStatusPublishMs_ = millis();
      }
      break;
    case MQTT_EVENT_PUBLISHED:
      state.lastPublishConfirmedMs = now;
      break;
    case MQTT_EVENT_DISCONNECTED:
      Serial.printf(
          "[MO][%s] disconnected t=%lu wifi_code=%d rssi=%d connected_ms=%lu\n",
          brokerName(broker), static_cast<unsigned long>(now),
          static_cast<int>(WiFi.status()), WiFi.RSSI(),
          static_cast<unsigned long>(connectedFor));
      state.connectedSinceMs = 0;
      state.state = State::Backoff;
      scheduleRetry(broker, now);
      break;
    case MQTT_EVENT_ERROR:
      state.lastError = event.hasError
                            ? event.error.esp_tls_last_esp_err
                            : ESP_FAIL;
      if (event.hasError) {
        Serial.printf(
            "[MO][%s] error t=%lu type=%d tls_esp=0x%x tls_stack=0x%x "
            "cert_flags=0x%x sock_errno=%d conn_refused=%d wifi_code=%d "
            "rssi=%d connected_ms=%lu\n",
            brokerName(broker), static_cast<unsigned long>(now),
            event.error.error_type,
            event.error.esp_tls_last_esp_err,
            event.error.esp_tls_stack_err,
            event.error.esp_tls_cert_verify_flags,
            event.error.esp_transport_sock_errno,
            event.error.connect_return_code,
            static_cast<int>(WiFi.status()), WiFi.RSSI(),
            static_cast<unsigned long>(connectedFor));
      } else {
        Serial.printf("[MO][%s] error t=%lu no_error_handle\n",
                      brokerName(broker), static_cast<unsigned long>(now));
      }
      state.connectedSinceMs = 0;
      state.state = State::Error;
      scheduleRetry(broker, now);
      break;
    case MQTT_EVENT_BEFORE_CONNECT:
      Serial.printf("[MO][%s] before connect t=%lu\n", brokerName(broker),
                    static_cast<unsigned long>(now));
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
    Serial.printf("[MO][%s] deferred: insufficient heap retry_ms=%lu\n",
                  brokerName(broker),
                  static_cast<unsigned long>(kRetryBaseMs));
    return;
  }

  state.reconnectPending = false;
  state.lastConnectAttemptMs = now;
  state.state = State::Connecting;
  Serial.printf("[MO][%s] connecting t=%lu attempt=%lu\n",
                brokerName(broker), static_cast<unsigned long>(now),
                static_cast<unsigned long>(state.reconnectFailures + 1));
  if (!startBroker(broker, now)) scheduleRetry(broker, now);
}

bool MOMQTT::startBroker(BrokerId broker, uint32_t now) {
  BrokerRuntime& state = runtime(brokers_, broker);
  esp_mqtt_client_config_t config{};
  std::string uri;

  if (!buildClientConfig(broker, config, uri)) {
    state.lastError = ESP_ERR_INVALID_ARG;
    Serial.printf("[MO][%s] invalid broker configuration\n", brokerName(broker));
    return false;
  }

  const MOBrokerPrefs& brokerPrefs = prefs(prefs_, broker);
  Serial.printf(
      "[MO][%s] init host=%s port=%u transport=%s free_heap=%u largest_heap=%u\n",
      brokerName(broker), brokerPrefs.host.c_str(),
      static_cast<unsigned>(brokerPrefs.port),
      transportName(brokerPrefs.transport),
      static_cast<unsigned>(
          heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
      static_cast<unsigned>(heap_caps_get_largest_free_block(
          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));

  char statusTopic[192]{};
  char offlinePayload[kStatusJsonBufferSize]{};
  size_t offlineLength = 0;
  if (!buildTopic("status", statusTopic, sizeof(statusTopic))) {
    state.lastError = ESP_ERR_INVALID_ARG;
    return false;
  }

  if (statusSnapshot_.valid) {
    char timestamp[32]{};
    const time_t nowTime = time(nullptr);
    const tm* utc = gmtime(&nowTime);
    if (utc == nullptr ||
        std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", utc) == 0) {
      state.lastError = ESP_ERR_INVALID_ARG;
      return false;
    }

    const StatusData offline{
        .status = "offline",
        .timestamp = timestamp,
        .origin = statusSnapshot_.origin,
        .originId = statusSnapshot_.originId,
        .model = statusSnapshot_.model,
        .firmwareVersion = statusSnapshot_.firmwareVersion,
        .radio = statusSnapshot_.radio,
        .clientVersion = statusSnapshot_.clientVersion,
        .repeat = statusSnapshot_.repeat,
        .batteryMv = statusSnapshot_.batteryMv,
        .uptimeSecs = statusSnapshot_.uptimeSecs,
        .errors = statusSnapshot_.errors,
        .queueLen = statusSnapshot_.queueLen,
        .noiseFloor = statusSnapshot_.noiseFloor,
        .txAirSecs = statusSnapshot_.txAirSecs,
        .rxAirSecs = statusSnapshot_.rxAirSecs,
        .recvErrors = statusSnapshot_.recvErrors,
        .packetsSent = statusSnapshot_.packetsSent,
        .packetsReceived = statusSnapshot_.packetsReceived,
    };
    if (!buildStatusJson(offline, offlinePayload, sizeof(offlinePayload),
                          offlineLength)) {
      state.lastError = ESP_ERR_NO_MEM;
      return false;
    }
  } else {
    const char fallback[] = "{\"status\":\"offline\"}";
    std::memcpy(offlinePayload, fallback, sizeof(fallback));
    offlineLength = sizeof(fallback) - 1;
  }

#if ESP_IDF_VERSION_MAJOR >= 5
  config.session.last_will.topic = statusTopic;
  config.session.last_will.msg = offlinePayload;
  config.session.last_will.msg_len = offlineLength;
  config.session.last_will.qos = 1;
  config.session.last_will.retain = 1;
#else
  config.lwt_topic = statusTopic;
  config.lwt_msg = offlinePayload;
  config.lwt_qos = 1;
  config.lwt_retain = 1;
#endif

  esp_mqtt_client_handle_t client = esp_mqtt_client_init(&config);
  if (client == nullptr) {
    state.lastError = ESP_ERR_NO_MEM;
    Serial.printf("[MO][%s] init failed: no memory\n", brokerName(broker));
    return false;
  }

  esp_err_t rc = esp_mqtt_client_register_event(
      client, MQTT_EVENT_ANY, &MOMQTT::onMqttEvent, &eventContexts_[index(broker)]);
  if (rc != ESP_OK) {
    esp_mqtt_client_destroy(client);
    state.lastError = rc;
    Serial.printf("[MO][%s] event registration failed rc=0x%x\n",
                  brokerName(broker), static_cast<unsigned>(rc));
    return false;
  }

  state.client = client;
  state.started = true;
  state.state = State::Connecting;
  state.lastConnectAttemptMs = now;

  rc = esp_mqtt_client_start(client);
  if (rc != ESP_OK) {
    state.lastError = rc;
    Serial.printf("[MO][%s] start failed rc=0x%x\n", brokerName(broker),
                  static_cast<unsigned>(rc));
    destroyBroker(broker);
    return false;
  }
  Serial.printf("[MO][%s] start requested t=%lu\n", brokerName(broker),
                static_cast<unsigned long>(now));
  return true;
}

void MOMQTT::destroyBroker(BrokerId broker) {
  BrokerRuntime& state = runtime(brokers_, broker);
  if (state.client != nullptr) {
    esp_mqtt_client_stop(state.client);
    esp_mqtt_client_destroy(state.client);
  }
  // stop/destroy join the MQTT task before its callback context is changed.
  // Events already queued by that client must not affect its replacement.
  ++eventContexts_[index(broker)].generation;
  clearRuntime(broker);
}

void MOMQTT::scheduleRetry(BrokerId broker, uint32_t now) {
  BrokerRuntime& state = runtime(brokers_, broker);
  // ESP-MQTT can emit ERROR and DISCONNECTED for the same failed attempt.
  // Count and schedule that attempt only once.
  if (state.reconnectPending) return;
  state.reconnectFailures =
      std::min<uint32_t>(state.reconnectFailures + 1, 31);
  state.reconnectPending = true;
  state.state = State::Backoff;
  const uint32_t delay = retryDelayMs(state.reconnectFailures);
  state.nextConnectAttemptMs = now + delay;
  Serial.printf(
      "[MO][%s] backoff t=%lu retry_ms=%lu failures=%lu last_error=0x%x\n",
      brokerName(broker), static_cast<unsigned long>(now),
      static_cast<unsigned long>(delay),
      static_cast<unsigned long>(state.reconnectFailures),
      static_cast<unsigned>(state.lastError));
}

void MOMQTT::clearRuntime(BrokerId broker) {
  BrokerRuntime& state = runtime(brokers_, broker);
  state.client = nullptr;
  state.started = false;
  state.reconnectPending = false;
  state.nextConnectAttemptMs = 0;
  state.connectedSinceMs = 0;
  state.lastPublishQueuedMs = 0;
  state.lastPublishConfirmedMs = 0;
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

bool MOMQTT::isNetworkReady() const {
  return wifi_.connected() && wifi_.hasTimeSync();
}

MOMQTT::State MOMQTT::networkWaitState() const {
  return wifi_.connected() ? State::WaitingForTime : State::WaitingForWiFi;
}

bool MOMQTT::buildClientConfig(BrokerId broker,
                               esp_mqtt_client_config_t& config,
                               std::string& uri) const {
  const MOBrokerPrefs& brokerPrefs = prefs(prefs_, broker);
  if (!isEnabled(brokerPrefs) &&
      !runtime(brokers_, broker).forcedEnabled) {
    return false;
  }
  if (brokerPrefs.host.empty() || brokerPrefs.port == 0) return false;

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

  const auto addString = [&](std::string_view key, std::string_view value) {
    const bool ok =
        appendJsonString(buffer, bufferSize, length, key, value, comma);
    if (ok) comma = true;
    return ok;
  };

  const auto addNumber = [&](std::string_view key, auto value) {
    char number[24]{};
    const int written = std::snprintf(number, sizeof(number), "%ld",
                                      static_cast<long>(value));
    if (written < 0 || static_cast<size_t>(written) >= sizeof(number)) {
      return false;
    }
    const bool ok = appendJsonLiteral(buffer, bufferSize, length, key, number,
                                      comma);
    if (ok) comma = true;
    return ok;
  };

  if (!addString("status", s.status) ||
      !addString("timestamp", s.timestamp) ||
      !addString("origin", s.origin) ||
      !addString("origin_id", s.originId) ||
      !addString("model", s.model) ||
      !addString("firmware_version", s.firmwareVersion) ||
      !addString("radio", s.radio) ||
      !addString("client_version", s.clientVersion) ||
      !addString("repeat", s.repeat) ||
      !appendChar(buffer, bufferSize, length, ',') ||
      !appendChar(buffer, bufferSize, length, '"') ||
      !appendEscaped(buffer, bufferSize, length, "stats") ||
      !appendChar(buffer, bufferSize, length, '"') ||
      !appendChar(buffer, bufferSize, length, ':') ||
      !appendChar(buffer, bufferSize, length, '{')) {
    return false;
  }

  bool statsComma = false;
  const auto addStat = [&](std::string_view key, auto value) {
    char number[24]{};
    const int written = std::snprintf(number, sizeof(number), "%ld",
                                      static_cast<long>(value));
    if (written < 0 || static_cast<size_t>(written) >= sizeof(number)) {
      return false;
    }
    const bool ok = appendJsonLiteral(buffer, bufferSize, length, key, number,
                                      statsComma);
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

const char* MOMQTT::brokerName(BrokerId broker) noexcept {
  return broker == BrokerId::Mqtt1 ? "MQTT1" : "MQTT2";
}
