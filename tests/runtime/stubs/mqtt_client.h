#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
using esp_err_t = int;
using esp_event_base_t = const char*;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_ARG = 2, ESP_ERR_NO_MEM = 3;
constexpr int MQTT_EVENT_ANY = -1, MQTT_EVENT_CONNECTED = 1, MQTT_EVENT_DISCONNECTED = 2,
              MQTT_EVENT_ERROR = 3, MQTT_EVENT_PUBLISHED = 4, MQTT_EVENT_BEFORE_CONNECT = 5;
constexpr int MQTT_TRANSPORT_OVER_TCP = 1;
struct esp_mqtt_error_codes_t {
  int esp_tls_last_esp_err = 0, esp_tls_stack_err = 0, esp_tls_cert_verify_flags = 0,
      error_type = 0, esp_transport_sock_errno = 0, connect_return_code = 0;
};
struct TestClient;
using esp_mqtt_client_handle_t = TestClient*;
struct esp_mqtt_event_t {
  int event_id;
  esp_mqtt_client_handle_t client;
  esp_mqtt_error_codes_t* error_handle;
};
using esp_mqtt_event_handle_t = esp_mqtt_event_t*;
struct esp_mqtt_client_config_t {
  const char *username{}, *password{}, *client_id{}, *uri{}, *host{}, *lwt_topic{}, *lwt_msg{};
  int keepalive{}, reconnect_timeout_ms{}, network_timeout_ms{}, buffer_size{}, out_buffer_size{},
      port{}, transport{}, lwt_qos{}, lwt_retain{};
  bool disable_auto_reconnect{};
  int (*crt_bundle_attach)(void*){};
};
using TestHandler = void (*)(void*, esp_event_base_t, int32_t, void*);
struct TestClient { TestHandler handler{}; void* context{}; bool destroyed{}; };
inline std::vector<TestClient*> allClients;
inline int starts = 0, stops = 0, destroys = 0, publishes = 0;
inline int fakeStartResult = ESP_OK;
inline bool insideCallback = false;
inline esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t*) {
  auto* c = new TestClient; allClients.push_back(c); return c;
}
inline int esp_mqtt_client_register_event(TestClient* c, int, TestHandler h, void* ctx) {
  c->handler = h; c->context = ctx; return ESP_OK;
}
inline int esp_mqtt_client_start(TestClient*) { ++starts; return fakeStartResult; }
inline int esp_mqtt_client_stop(TestClient*) { if (insideCallback) throw "stop from callback"; ++stops; return ESP_OK; }
inline int esp_mqtt_client_destroy(TestClient* c) {
  if (insideCallback) throw "destroy from callback";
  c->destroyed = true; ++destroys; return ESP_OK;
}
inline int esp_mqtt_client_enqueue(TestClient*, const char*, const char*, int, int, int, bool) {
  if (insideCallback) throw "publish from callback";
  return ++publishes;
}
inline void emit(TestClient* c, int id) {
  esp_mqtt_error_codes_t error{};
  esp_mqtt_event_t event{id, c, &error};
  insideCallback = true;
  c->handler(c->context, nullptr, id, &event);
  insideCallback = false;
}
