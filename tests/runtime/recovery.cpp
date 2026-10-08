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
bool linkUp = true, timeReady = true, bootMarker = false, markerWritable = true;
int wifiRestarts = 0;
std::vector<std::string> logEntries;
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
bool MOEtap2Prefs::consumeWatchdogReboot() const { bool v = bootMarker; bootMarker = false; return v; }
bool MOEtap2Prefs::markWatchdogReboot() const { bootMarker = markerWritable; return markerWritable; }
bool MOEtap2Prefs::appendLog(std::string_view, std::string_view entry) const { logEntries.emplace_back(entry); return true; }
bool MOEtap2Prefs::readLastLogLines(std::string_view, size_t, std::string&) const { return true; }
bool MOEtap2Prefs::clearLog() const { logEntries.clear(); return true; }
bool MOLocalTime::now(struct tm& local) noexcept { local = {}; local.tm_year = 126; return timeReady; }

using Broker = MOMQTT::BrokerId;
using State = MOMQTT::State;
using Service = MOWatchdog::Service;
struct Fixture {
  MOMQTTPrefs prefs;
  MOWifi wifi;
  MOMQTT mqtt{prefs, wifi};
  MOEtap2Prefs wdPrefs;
  MOWatchdog wdg{wdPrefs, wifi, mqtt};
  Fixture(bool dual = false) {
    fakeMs = 1000; linkUp = timeReady = markerWritable = true; bootMarker = false;
    fakeHeap = 120 * 1024; fakeStartResult = ESP_OK;
    prefs.mqtt1().state = MOOnOff::On;
    prefs.mqtt1().host = "broker1.invalid";
    if (dual) { prefs.mqtt2().state = MOOnOff::On; prefs.mqtt2().host = "broker2.invalid"; }
    prefs.iata_ = "TST";
    mqtt.setObserverIdentity("test-observer");
    mqtt.begin();
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
};

void reconnectAfterDisconnect() {
  Fixture f; f.connect(); auto* old = f.client(); int before = starts;
  emit(old, MQTT_EVENT_DISCONNECTED);
  assert(f.mqtt.connected(Broker::Mqtt1)); // callback does not mutate state
  f.mqtt.loop(); assert(old->destroyed && !f.client());
  fakeMs += 9999; f.mqtt.loop(); assert(starts == before);
  ++fakeMs; f.mqtt.loop(); assert(starts == before + 1 && f.client() != old);
  emit(f.client(), MQTT_EVENT_CONNECTED); f.mqtt.loop();
  assert(f.mqtt.connected(Broker::Mqtt1));
}
void failedAttemptAndStaleEvents() {
  Fixture f; f.mqtt.loop(); auto* old = f.client();
  emit(old, MQTT_EVENT_ERROR); emit(old, MQTT_EVENT_DISCONNECTED); f.mqtt.loop();
  assert(f.mqtt.status(Broker::Mqtt1).reconnectFailures == 1);
  fakeMs += 10000; f.mqtt.loop(); auto* next = f.client();
  emit(next, MQTT_EVENT_DISCONNECTED); // queued event from current generation
  f.mqtt.restart(Broker::Mqtt1); f.mqtt.loop();
  assert(f.client() && f.mqtt.status(Broker::Mqtt1).state == State::Connecting);
  assert(f.mqtt.status(Broker::Mqtt1).reconnectFailures == 1);
}
void timeoutDualAndDisabled() {
  Fixture f(true); f.connect(0); f.connect(1);
  auto* second = f.client(1); emit(f.client(), MQTT_EVENT_DISCONNECTED); f.mqtt.loop();
  fakeMs += 10000; f.mqtt.loop(); assert(f.client(1) == second && !second->destroyed);
  // An attempt with no callback must also recover.
  fakeMs += 10000; auto* timed = f.client(); f.mqtt.loop(); assert(timed->destroyed);
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
  fakeHeap = 120 * 1024; fakeMs += 10000; f.mqtt.loop(); assert(f.client(1));
}
void callbackPublishingAndOverflow() {
  Fixture f; f.mqtt.setStatusSnapshot({}); int before = publishes;
  f.mqtt.loop(); emit(f.client(), MQTT_EVENT_CONNECTED);
  assert(publishes == before); f.mqtt.loop(); assert(publishes > before);
  for (int i = 0; i < 20; ++i) emit(f.client(), MQTT_EVENT_PUBLISHED);
  auto* old = f.client(); f.mqtt.loop(); assert(old->destroyed && !f.client());
  fakeMs += 10000; f.mqtt.loop(); assert(f.client());
}
void rolloverAndBackoff() {
  Fixture f; fakeMs = UINT32_MAX - 5000; f.connect();
  emit(f.client(), MQTT_EVENT_DISCONNECTED); f.mqtt.loop();
  fakeMs += 9999; f.mqtt.loop(); assert(!f.client());
  ++fakeMs; f.mqtt.loop(); assert(f.client());
  assert(MOMQTT::retryDelayMs(1) == 10000);
  assert(MOMQTT::retryDelayMs(31) == 300000);
}
void simultaneousRebootOwners() {
  Fixture f(true); f.startWatchdog();
  // Walk the real escalation: 3 + 5 + 15 + 30 minutes, then final delay.
  for (uint32_t mins : {3, 5, 15, 30}) { fakeMs += mins * 60000; f.wdg.loop(); }
  assert(f.wdg.rebootScheduled_);
  f.healthy(0); fakeMs += 15000; f.wdg.loop();
  assert(f.wdg.rebootScheduled_); // MQTT2 must retain the reboot request
  fakeMs += 165000;
  bool rebooted = false;
  try { f.wdg.loop(); } catch (RebootRequested&) { rebooted = true; }
  assert(rebooted);
}
void recoveredServiceCancelsReboot() {
  Fixture f; f.startWatchdog(); f.wdg.scheduleReboot(Service::Mqtt1, fakeMs);
  f.healthy(0); fakeMs += 180000; f.wdg.loop();
  assert(!f.wdg.rebootScheduled_);
}
void storageFailureCannotVetoReboot() {
  Fixture f; f.startWatchdog(); markerWritable = false;
  f.wdg.scheduleReboot(Service::Mqtt1, fakeMs); fakeMs += 180000;
  bool rebooted = false;
  try { f.wdg.loop(); } catch (RebootRequested&) { rebooted = true; }
  assert(rebooted && !bootMarker);
}
void silentModeIsBounded() {
  Fixture f(true); bootMarker = true; f.startWatchdog();
  assert(f.wdg.phase_ == MOWatchdog::Phase::Silent);
  // Another service fails while one broker remains unavailable.
  linkUp = false; fakeMs += 15 * 60000; f.wdg.loop();
  assert(f.wdg.phase_ == MOWatchdog::Phase::Normal);
  f.wdg.loop(); int before = wifiRestarts; fakeMs += 180000; f.wdg.loop();
  assert(wifiRestarts == before + 1);
}
void watchdogAcknowledgementAndDisable() {
  Fixture f; f.connect();
  emit(f.client(), MQTT_EVENT_PUBLISHED); f.mqtt.loop();
  assert(f.mqtt.publishingHealthy(Broker::Mqtt1));
  fakeMs += 11 * 60000 + 1; assert(!f.mqtt.publishingHealthy(Broker::Mqtt1));
  f.startWatchdog(); f.wdg.scheduleReboot(Service::Mqtt1, fakeMs);
  f.wdg.setEnabled(false); fakeMs += 180000; f.wdg.loop();
  assert(!f.wdg.rebootScheduled_);
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
  run("post-reboot SILENT is bounded", silentModeIsBounded);
  run("publish ACK health and disabling watchdog", watchdogAcknowledgementAndDisable);
  for (auto* c : allClients) delete c;
  std::cout << count << " runtime scenarios passed\n";
}
