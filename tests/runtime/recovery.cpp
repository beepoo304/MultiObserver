// Runs the real MQTT/watchdog implementation against fake time, MQTT, Wi-Fi
// and storage. No broker, device, credentials or network access is needed.
#include <array>
#include <atomic>
#include <string>
#include <string_view>
#include <cassert>
#include <iostream>
#include <limits>
#include "Arduino.h"
#include "mqtt_client.h"
#include "freertos/queue.h"
#define private public
#include "MOMQTT.h"
#include "MOWatchdog.h"
#undef private
#include "MOLocalTime.h"
#include "esp_heap_caps.h"

uint32_t fakeMs = 1000;
bool linkUp = true, timeReady = true, markerWritable = true;
uint8_t bootMarkerMask = 0;
int wifiRestarts = 0;
std::vector<std::string> alerts;
MOWifi::MOWifi() = default;
bool MOWifi::connected() const noexcept { return linkUp; }
bool MOWifi::healthy() const noexcept { return linkUp; }
bool MOWifi::hasTimeSync() const noexcept { return timeReady; }
void MOWifi::restart() { ++wifiRestarts; }
const MOBrokerPrefs& MOMQTTPrefs::mqtt1() const noexcept { return mqtt1_; }
MOBrokerPrefs& MOMQTTPrefs::mqtt1() noexcept { return mqtt1_; }
const MOBrokerPrefs& MOMQTTPrefs::mqtt2() const noexcept { return mqtt2_; }
MOBrokerPrefs& MOMQTTPrefs::mqtt2() noexcept { return mqtt2_; }
const std::string& MOMQTTPrefs::iata() const noexcept { return iata_; }
bool MOEtap2Prefs::watchdogEnabled() const noexcept { return watchdogEnabled_; }
uint16_t MOEtap2Prefs::graceSeconds() const noexcept { return graceSeconds_; }
void MOEtap2Prefs::setWatchdogEnabled(bool v) noexcept { watchdogEnabled_ = v; }
uint8_t MOEtap2Prefs::consumeWatchdogReboot() const { auto v = bootMarkerMask; bootMarkerMask = 0; return v; }
bool MOEtap2Prefs::markWatchdogReboot(uint8_t mask) const { if (!markerWritable) return false; bootMarkerMask = mask; return true; }
bool MOLocalTime::now(struct tm& local) noexcept { local = {}; local.tm_year = 126; return timeReady; }

using Broker = MOMQTT::BrokerId;
using State = MOMQTT::State;
struct Fixture {
  MOMQTTPrefs prefs;
  MOWifi wifi;
  MOMQTT mqtt{prefs, wifi};
  MOEtap2Prefs wdPrefs;
  MOWatchdog wdg{wdPrefs, wifi, mqtt};
  Fixture(bool dual = false) {
    fakeMs = 1000; linkUp = timeReady = markerWritable = true; bootMarkerMask = 0;
    wifiRestarts = 0; alerts.clear();
    fakeHeap = 120 * 1024; fakeStartResult = ESP_OK;
    prefs.mqtt1().state = MOOnOff::On;
    prefs.mqtt1().host = "broker1.invalid";
    if (dual) { prefs.mqtt2().state = MOOnOff::On; prefs.mqtt2().host = "broker2.invalid"; }
    prefs.iata_ = "TST";
    mqtt.setObserverIdentity("test-observer");
    mqtt.begin();
    wdg.setAlertSink([](void*, const char* text) -> bool { alerts.emplace_back(text); return true; }, nullptr);
  }
  TestClient* client(size_t i = 0) { return mqtt.brokers_[i].client; }
  void connect(size_t i = 0) {
    mqtt.loop(); assert(client(i)); emit(client(i), MQTT_EVENT_CONNECTED); mqtt.loop();
  }
  void healthy(size_t i) {
    auto& s = mqtt.brokers_[i]; s.state = State::Connected;
    s.connectedSinceMs = fakeMs; s.lastPublishConfirmedMs = fakeMs;
  }
  void startWatchdog() {
    wdg.begin(); fakeMs += wdPrefs.graceSeconds() * 1000; wdg.loop(); wdg.loop();
  }
  void advance(uint32_t ms) { fakeMs += ms; wdg.loop(); }
};

void reconnectAfterDisconnect() {
  Fixture f; f.connect(); auto* old = f.client(); int before = starts;
  emit(old, MQTT_EVENT_DISCONNECTED);
  assert(f.mqtt.connected(Broker::Mqtt1)); // callback does not mutate state
  f.mqtt.loop(); assert(old->destroyed && !f.client());
  fakeMs += 179999; f.mqtt.loop(); assert(starts == before);
  ++fakeMs; f.mqtt.loop(); assert(starts == before + 1 && f.client() != old);
  emit(f.client(), MQTT_EVENT_CONNECTED); f.mqtt.loop();
  assert(f.mqtt.connected(Broker::Mqtt1));
}
void failedAttemptAndStaleEvents() {
  Fixture f; f.mqtt.loop(); auto* old = f.client();
  emit(old, MQTT_EVENT_ERROR); emit(old, MQTT_EVENT_DISCONNECTED); f.mqtt.loop();
  assert(f.mqtt.status(Broker::Mqtt1).reconnectFailures == 1);
  fakeMs += 180000; f.mqtt.loop(); auto* next = f.client();
  emit(next, MQTT_EVENT_DISCONNECTED); // queued event from current generation
  f.mqtt.restart(Broker::Mqtt1); f.mqtt.loop();
  assert(f.client() && f.mqtt.status(Broker::Mqtt1).state == State::Connecting);
  assert(f.mqtt.status(Broker::Mqtt1).reconnectFailures == 1);
}
void timeoutDualAndDisabled() {
  Fixture f(true); f.connect(0); f.connect(1);
  auto* second = f.client(1); emit(f.client(), MQTT_EVENT_DISCONNECTED); f.mqtt.loop();
  fakeMs += 179999; f.mqtt.loop(); assert(!f.client(0) && f.client(1) == second && !second->destroyed);
  ++fakeMs; f.mqtt.loop(); auto* timed = f.client(0); assert(timed && f.client(1) == second);
  // A connection attempt with no callback times out and enters backoff.
  fakeMs += 10'001; f.mqtt.loop(); assert(timed->destroyed && !f.client(0));
  f.prefs.mqtt1().state = MOOnOff::Off; fakeMs += 300000; f.mqtt.loop();
  assert(f.mqtt.status(Broker::Mqtt1).state == State::Disabled && !f.client());
}
void networkAndHeapRecovery() {
  Fixture f(true); f.connect();
  linkUp = false; f.mqtt.loop(); assert(!f.client() && !f.client(1));
  linkUp = true; timeReady = false; f.mqtt.loop(); assert(!f.client());
  timeReady = true; f.connect();
  // MQTT2 is allowed only after MQTT1 finishes its handshake.
  if (f.client(1)) f.mqtt.destroyBroker(Broker::Mqtt2);
  f.mqtt.brokers_[1].state = State::Disconnected;
  fakeHeap = 40000; f.mqtt.loop(); assert(!f.client(1));
  fakeHeap = 120 * 1024; fakeMs += 180000; f.mqtt.loop(); assert(f.client(1));
}
void callbackPublishingAndOverflow() {
  Fixture f; f.mqtt.setStatusSnapshot({}); int before = publishes;
  f.mqtt.loop(); emit(f.client(), MQTT_EVENT_CONNECTED);
  assert(publishes == before); f.mqtt.loop(); assert(publishes > before);
  for (int i = 0; i < 20; ++i) emit(f.client(), MQTT_EVENT_PUBLISHED);
  auto* old = f.client(); f.mqtt.loop(); assert(old->destroyed && !f.client());
  fakeMs += 180000; f.mqtt.loop(); assert(f.client());
}
void rolloverAndBackoff() {
  Fixture f; f.startWatchdog();
  fakeMs = UINT32_MAX - 10'000; f.wdg.phase_ = MOWatchdog::Phase::Normal;
  f.wdg.nextServiceCheckMs_ = fakeMs; f.wdg.mqtt1_.nextActionMs = fakeMs + 10'000; f.wdg.loop();
  fakeMs += 20'000; f.wdg.loop();
  assert(f.wdg.mqtt1_.attempts == 2);
}
void simultaneousRebootOwners() {
  Fixture f(true); f.startWatchdog();
  assert(f.wdg.mqtt1_.attempts == 1 && f.wdg.mqtt2_.attempts == 1);
  f.advance(3 * 60'000); assert(f.wdg.mqtt1_.attempts == 2 && f.wdg.mqtt2_.attempts == 2);
  f.advance(3 * 60'000); assert(f.wdg.mqtt1_.attempts == 3 && f.wdg.mqtt2_.attempts == 3);
  f.advance(3 * 60'000); assert(f.wdg.rebootBrokerMask_ == 3);
  bool rebooted = false; try { f.advance(30'000); } catch (RebootRequested&) { rebooted = true; }
  assert(rebooted && bootMarkerMask == 3);
}
void recoveredServiceCancelsReboot() {
  Fixture f; f.startWatchdog();
  f.advance(3 * 60'000); f.advance(3 * 60'000); f.advance(3 * 60'000);
  assert(f.wdg.rebootBrokerMask_ == 1);
  f.healthy(0); f.advance(30'000);
  assert(f.wdg.rebootBrokerMask_ == 0 && alerts.back() == "mqtt1.restore");
}
void storageFailureCannotVetoReboot() {
  Fixture f; f.startWatchdog();
  f.advance(3 * 60'000); f.advance(3 * 60'000); f.advance(3 * 60'000);
  markerWritable = false; bool rebooted = false;
  try { f.advance(30'000); } catch (RebootRequested&) { rebooted = true; }
  assert(!rebooted && f.wdg.rebootBrokerMask_ == 0 && f.wdg.mqtt1_.postReboot);
}
void postRebootRetriesDoNotLoop() {
  Fixture f; bootMarkerMask = 1; f.startWatchdog();
  assert(f.wdg.mqtt1_.postReboot);
  assert(!alerts.empty() && alerts.back() == "mqtt1.down");
  f.advance(5 * 60'000); assert(f.wdg.mqtt1_.postReboot && f.wdg.rebootBrokerMask_ == 0);
  assert(alerts.back() == "mqtt1.down");
}
void wifiRestartAndNoReboot() {
  Fixture f; f.startWatchdog(); linkUp = false;
  f.advance(5'000); f.advance(30'000); assert(wifiRestarts == 1 && f.wdg.rebootBrokerMask_ == 0);
  f.advance(3 * 60'000); assert(wifiRestarts == 1);
  linkUp = true; f.advance(5'000); linkUp = false;
  f.advance(5'000); f.advance(30'000);
  assert(wifiRestarts == 2);
}
void statusLabelsAndDisable() {
  Fixture f; f.startWatchdog(); char text[128]; f.wdg.formatStatus(text, sizeof(text));
  assert(std::string(text).find("phase=NORMAL wifi=UP mqtt1=DOWN mqtt2=OFF reboot=NO") != std::string::npos);
  f.wdg.setEnabled(false); f.advance(10 * 60'000); assert(f.wdg.rebootBrokerMask_ == 0);
}

int main() {
  std::cout << std::unitbuf;
  int count = 0;
  auto run = [&](const char* name, auto test) { std::cout << "RUN " << name << '\n'; test(); ++count; std::cout << "PASS " << name << '\n'; };
  run("disconnect retries at deadline", reconnectAfterDisconnect);
  run("ERROR/DISCONNECTED dedup and old event isolation", failedAttemptAndStaleEvents);
  run("two brokers, timeout and disabled profile", timeoutDualAndDisabled);
  run("Wi-Fi, NTP and heap recovery", networkAndHeapRecovery);
  run("callback isolation and bounded queue overflow recovery", callbackPublishingAndOverflow);
  run("millis rollover and bounded backoff", rolloverAndBackoff);
  run("two failed brokers retain independent reboot causes", simultaneousRebootOwners);
  run("recovery cancels no-longer-needed reboot", recoveredServiceCancelsReboot);
  run("storage failure cannot suppress reboot", storageFailureCannotVetoReboot);
  run("post-reboot retries do not reboot-loop", postRebootRetriesDoNotLoop);
  run("Wi-Fi restart after 30s never reboots ESP", wifiRestartAndNoReboot);
  run("status labels and WDG disable", statusLabelsAndDisable);
  for (auto* c : allClients) delete c;
  std::cout << count << " runtime scenarios passed\n";
}
