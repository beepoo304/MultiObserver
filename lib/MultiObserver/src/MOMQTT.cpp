#include "MOMQTT.h"

#include "MOWifi.h"

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <limits>
#include <string>

namespace {

const char* transportName(MOTransport transport) {
  return transport == MOTransport::Wss ? "wss" : "tcp";
}

bool isEnabled(const MOBrokerPrefs& prefs) {
  return prefs.state == MOOnOff::On && !prefs.host.empty() && prefs.port != 0;
}

}  // namespace

MOMQTT::MOMQTT(MOMQTTPrefs& prefs, MOWifi& wifi)
    : prefs_(prefs), wifi_(wifi) {}

MOMQTT::~MOMQTT() {
  end();
}

void MOMQTT::begin() {
  if (running_) {
    return;
  }

  brokers_.fill({});
  running_ = true;
}

void MOMQTT::end() {
  if (!running_) {
    return;
  }

  destroyBroker(BrokerId::Mqtt1);
  destroyBroker(BrokerId::Mqtt2);
  running_ = false;
}

void MOMQTT::loop() {
  if (!running_) {
    return;
  }

  const uint32_t now = millis();

  if (!isWiFiReady()) {
    for (BrokerId broker : {BrokerId::Mqtt1, BrokerId::Mqtt2}) {
      BrokerRuntime& state = runtime(brokers_, broker);
      if (state.client != nullptr) {
        destroyBroker(broker);
      }
      if (isEnabled(prefs(prefs_, broker))) {
        state.state = State::WaitingForWiFi;
      } else {
        state.state = State::Disabled;
      }
    }
    return;
  }

  bool connectStarted = false;

  for (BrokerId broker : {BrokerId::Mqtt1, BrokerId::Mqtt2}) {
    BrokerRuntime& state = runtime(brokers_, broker);

    if (!isEnabled(prefs(prefs_, broker))) {
      if (state.client != nullptr) {
        destroyBroker(broker);
      }
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
  if (!running_ || !isWiFiReady() || !isEnabled(prefs(prefs_, broker))) {
    return;
  }

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
  return {
      .state = state.state,
      .reconnectFailures = state.reconnectFailures,
      .lastError = state.lastError,
  };
}

bool MOMQTT::connected(BrokerId broker) const noexcept {
  return runtime(brokers_, broker).state == State::Connected;
}

bool MOMQTT::queuePublish(BrokerId broker, std::string_view topic,
                          std::string_view payload, bool retain) {
  const BrokerRuntime& state = runtime(brokers_, broker);

  if (state.client == nullptr || state.state != State::Connected ||
      topic.empty() || payload.empty()) {
    return false;
  }

  const int rc = esp_mqtt_client_enqueue(
      state.client, std::string(topic).c_str(), std::string(payload).c_str(),
      0, 1, retain ? 1 : 0, true);

  return rc >= 0;
}

void MOMQTT::onMqttEvent(void* handlerArg, esp_event_base_t eventBase,
                         int32_t eventId, void* eventData) {
  (void)eventBase;
  (void)eventId;

  if (handlerArg == nullptr || eventData == nullptr) {
    return;
  }

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

  if (state.client != nullptr) {
    return;
  }

  if (state.nextConnectAttemptMs != 0 &&
      static_cast<int32_t>(now - state.nextConnectAttemptMs) < 0) {
    return;
  }

  if (!state.reconnectPending && state.state != State::Disconnected &&
      state.state != State::Backoff && state.state != State::Error) {
    return;
  }

  if (!hasConnectHeadroom(broker)) {
    state.state = State::Backoff;
    state.reconnectPending = true;
    state.nextConnectAttemptMs = now + kRetryBaseMs;
    return;
  }

  state.reconnectPending = false;
  state.lastConnectAttemptMs = now;
  state.state = State::Connecting;

  if (!startBroker(broker, now)) {
    scheduleRetry(broker, now);
  }
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

  config.user_context = this;

  esp_err_t rc =
      esp_mqtt_client_register_event(client, MQTT_EVENT_ANY,
                                     &MOMQTT::onMqttEvent, this);
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
      broker == BrokerId::Mqtt1
          ? connected(BrokerId::Mqtt2)
          : connected(BrokerId::Mqtt1);

  if (!otherConnected) {
    return true;
  }

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

  if (!isEnabled(brokerPrefs)) {
    return false;
  }

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

MOMQTT::BrokerRuntime& MOMQTT::runtime(
    std::array<BrokerRuntime, 2>& runtimes, BrokerId broker) noexcept {
  return runtimes[index(broker)];
}

const MOMQTT::BrokerRuntime& MOMQTT::runtime(
    const std::array<BrokerRuntime, 2>& runtimes, BrokerId broker) noexcept {
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
  if (failures == 0) {
    return kRetryBaseMs;
  }

  constexpr uint32_t kMaxShift = 5;
  const uint32_t shift = std::min(failures - 1, kMaxShift);
  const uint64_t delay =
      static_cast<uint64_t>(kRetryBaseMs) << shift;
  return static_cast<uint32_t>(
      std::min<uint64_t>(delay, kRetryMaxMs));
}
