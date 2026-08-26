#include "MOCli.h"

#include <Arduino.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr size_t kMaxCommandLength = 159;

std::string_view afterFirstSpace(std::string_view value) {
  const size_t pos = value.find(' ');
  if (pos == std::string_view::npos) {
    return {};
  }
  return value.substr(pos + 1);
}

bool equals(std::string_view lhs, std::string_view rhs) {
  return lhs == rhs;
}

}  // namespace

MOCli::MOCli(MOConfig& config, MOWifi& wifi, MOMQTT& mqtt,
             MOAlertChannel& channel, MOWatchdog& watchdog)
    : config_(config), wifi_(wifi), mqtt_(mqtt), channel_(channel),
      watchdog_(watchdog) {}

bool MOCli::handleCommand(uint32_t senderTimestamp, const char* command,
                          char* reply) noexcept {
  (void)senderTimestamp;

  if (command == nullptr || reply == nullptr) {
    return false;
  }

  reply[0] = '\0';

  const size_t length = strnlen(command, kMaxCommandLength + 1);
  if (length == 0 || length > kMaxCommandLength) {
    return false;
  }

  const std::string_view input(command, length);
  if (startsWith(input, "set wifi.") || startsWith(input, "get wifi.") ||
      input == "restart.wifi") {
    return handleWifi(input, reply);
  }

  if (startsWith(input, "set mqtt1.") || startsWith(input, "get mqtt1.") ||
      input == "restart.mqtt1" || startsWith(input, "set mqtt2.") ||
      startsWith(input, "get mqtt2.") || input == "restart.mqtt2") {
    return handleMqtt(input, reply);
  }

  if (input == "get mqtt.iata" || startsWith(input, "set mqtt.iata ")) {
    return handleCustom(input, reply);
  }

  if (startsWith(input, "get wdg.") || startsWith(input, "set wdg.") ||
      input == "restart.wdg") {
    return handleWatchdog(input, reply);
  }

  if (startsWith(input, "get channel.") || startsWith(input, "set channel.") ||
      input == "test.channel") {
    return handleChannel(input, reply);
  }

  return false;
}

bool MOCli::handleWatchdog(std::string_view command, char* reply) noexcept {
  if (command == "get wdg.status") {
    char buffer[kReplyCapacity]{};
    watchdog_.formatStatus(buffer, sizeof(buffer));
    return copyReply(reply, buffer);
  }
  if (command == "get wdg.grace") {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "WDG GRACE %u",
                  static_cast<unsigned>(config_.etap2().graceSeconds()));
    return copyReply(reply, buffer);
  }
  if (command == "get wdg.log") {
    char buffer[kReplyCapacity]{};
    return watchdog_.formatLog(buffer, sizeof(buffer)) && copyReply(reply, buffer);
  }
  if (command == "restart.wdg") {
    watchdog_.restartGrace();
    return copyReply(reply, "OK");
  }

  std::string_view key;
  std::string_view value;
  if (!splitSet(command, key, value)) return false;
  if ((key == "wdg.on" || key == "wdg.off") && value.empty()) {
    const bool enabled = key == "wdg.on";
    watchdog_.setEnabled(enabled);
    if (!config_.etap2().save()) return copyReply(reply, "ERR");
    return copyReply(reply, "OK");
  }
  if (key == "wdg.grace") {
    uint16_t seconds = 0;
    if (!parsePort(value, seconds) ||
        !config_.etap2().setGraceSeconds(seconds) ||
        !config_.etap2().save()) return copyReply(reply, "ERR");
    watchdog_.restartGrace();
    return copyReply(reply, "OK");
  }
  if (key == "wdg.log" && value == "1") {
    return copyReply(reply, watchdog_.clearLog() ? "CLEAR" : "ERR");
  }
  return false;
}

bool MOCli::handleChannel(std::string_view command, char* reply) noexcept {
  if (command == "get channel.status") {
    const char* status = !config_.etap2().channelEnabled()
                             ? "CHANNEL OFF"
                             : (channel_.ready() ? "CHANNEL READY"
                                                 : "CHANNEL KEY/SENDER MISSING");
    return copyReply(reply, status);
  }
  if (command == "get channel.key") {
    return copyReply(reply, config_.etap2().channelKey().empty()
                                ? "CHANNEL KEY CLEAR"
                                : config_.etap2().channelKey());
  }
  if (command == "test.channel") {
    char status[96]{};
    watchdog_.formatStatus(status, sizeof(status));
    return copyReply(reply, channel_.test(status) ? "QUEUED" : "ERR");
  }

  std::string_view key;
  std::string_view value;
  if (!splitSet(command, key, value)) return false;
  if ((key == "channel.on" || key == "channel.off") && value.empty()) {
    config_.etap2().setChannelEnabled(key == "channel.on");
    return copyReply(reply, config_.etap2().save() ? "OK" : "ERR");
  }
  if (key == "channel.key") {
    if (!config_.etap2().setChannelKey(value) || !config_.etap2().save()) {
      return copyReply(reply, "ERR");
    }
    return copyReply(reply, "OK");
  }
  return false;
}

bool MOCli::handleWifi(std::string_view command, char* reply) noexcept {
  if (command == "restart.wifi") {
    return restartWifi(reply);
  }

  if (command == "get wifi.ssid") {
    const auto& ssid = config_.wifi().ssid();
    if (ssid.empty()) {
      return copyReply(reply, "SSID ");
    }

    char buffer[kReplyCapacity];
    const int written =
        std::snprintf(buffer, sizeof(buffer), "SSID %s", ssid.c_str());
    if (written < 0 || static_cast<size_t>(written) >= sizeof(buffer)) {
      return copyReply(reply, "ERR");
    }
    return copyReply(reply, buffer);
  }

  if (command == "get wifi.status") {
    char buffer[kReplyCapacity]{};
    wifi_.formatStatus(buffer, sizeof(buffer));
    return copyReply(reply, buffer);
  }

  std::string_view key;
  std::string_view value;
  if (!splitSet(command, key, value)) {
    return false;
  }

  if (key == "wifi.ssid") {
    return setWifiSsid(value, reply);
  }

  if (key == "wifi.pwd") {
    return setWifiPassword(value, reply);
  }

  return false;
}

bool MOCli::handleMqtt(std::string_view command, char* reply) noexcept {
  if (command == "restart.mqtt1") {
    return restartMqtt(MOMQTT::BrokerId::Mqtt1, reply);
  }

  if (command == "restart.mqtt2") {
    return restartMqtt(MOMQTT::BrokerId::Mqtt2, reply);
  }

  const bool mqtt1 = startsWith(command, "set mqtt1.") ||
                     startsWith(command, "get mqtt1.");
  const MOMQTT::BrokerId broker =
      mqtt1 ? MOMQTT::BrokerId::Mqtt1 : MOMQTT::BrokerId::Mqtt2;

  const std::string_view prefix = mqtt1 ? "mqtt1." : "mqtt2.";

  if (command == "get mqtt1.status" || command == "get mqtt2.status") {
    const auto runtimeStatus = mqtt_.status(broker);
    const auto& prefs = broker == MOMQTT::BrokerId::Mqtt1
                            ? config_.mqtt().mqtt1()
                            : config_.mqtt().mqtt2();

    char buffer[kReplyCapacity];
    const char* configured =
        prefs.state == MOOnOff::On ? "ON" : "OFF";
    const int written = std::snprintf(
        buffer, sizeof(buffer), "%s %s %s failures=%lu err=0x%x",
        configured, stateName(runtimeStatus.state).data(), prefs.host.c_str(),
        static_cast<unsigned long>(runtimeStatus.reconnectFailures),
        static_cast<unsigned>(runtimeStatus.lastError));
    if (written < 0 || static_cast<size_t>(written) >= sizeof(buffer)) {
      return copyReply(reply, "ERR");
    }
    return copyReply(reply, buffer);
  }

  std::string_view key;
  std::string_view value;
  if (!splitSet(command, key, value)) {
    return false;
  }

  if (!startsWith(key, prefix)) {
    return false;
  }

  key.remove_prefix(prefix.size());

  if (key == "on" && value.empty()) {
    return setMqttState(broker, MOOnOff::On, reply);
  }

  if (key == "off" && value.empty()) {
    return setMqttState(broker, MOOnOff::Off, reply);
  }

  if (key == "host") {
    return setMqttHost(broker, value, reply);
  }

  if (key == "port") {
    return setMqttPort(broker, value, reply);
  }

  if (key == "transport") {
    return setMqttTransport(broker, value, reply);
  }

  if (key == "username") {
    return setMqttUsername(broker, value, reply);
  }

  if (key == "pwd") {
    return setMqttPassword(broker, value, reply);
  }

  return false;
}

bool MOCli::handleCustom(std::string_view command, char* reply) noexcept {
  if (command == "get mqtt.iata") {
    return copyReply(reply, config_.mqtt().iata());
  }

  std::string_view key;
  std::string_view value;
  if (!splitSet(command, key, value) || key != "mqtt.iata") {
    return false;
  }

  return setIata(value, reply);
}

bool MOCli::setWifiSsid(std::string_view value, char* reply) noexcept {
  if (value.empty()) {
    return copyReply(reply, "ERR");
  }

  config_.wifi().setSsid(std::string(value));
  if (!config_.wifi().save()) {
    return copyReply(reply, "ERR");
  }

  wifi_.setCredentials(config_.wifi().ssid(), config_.wifi().password());
  wifi_.reconnect();
  return copyReply(reply, "OK");
}

bool MOCli::setWifiPassword(std::string_view value, char* reply) noexcept {
  if (value.empty()) {
    return copyReply(reply, "ERR");
  }

  config_.wifi().setPassword(std::string(value));
  if (!config_.wifi().save()) {
    return copyReply(reply, "ERR");
  }

  wifi_.setCredentials(config_.wifi().ssid(), config_.wifi().password());
  wifi_.reconnect();
  return copyReply(reply, "OK");
}

bool MOCli::setMqttState(MOMQTT::BrokerId broker, MOOnOff state,
                         char* reply) noexcept {
  auto& prefs = broker == MOMQTT::BrokerId::Mqtt1
                    ? config_.mqtt().mqtt1()
                    : config_.mqtt().mqtt2();

  prefs.state = state;
  if (!config_.mqtt().save()) {
    return copyReply(reply, "ERR");
  }

  if (state == MOOnOff::On) {
    mqtt_.restart(broker);
  } else {
    mqtt_.disconnect(broker);
  }

  return copyReply(reply, "OK");
}

bool MOCli::setMqttHost(MOMQTT::BrokerId broker, std::string_view value,
                        char* reply) noexcept {
  if (value.empty()) {
    return copyReply(reply, "ERR");
  }

  auto& prefs = broker == MOMQTT::BrokerId::Mqtt1
                    ? config_.mqtt().mqtt1()
                    : config_.mqtt().mqtt2();
  prefs.host.assign(value.data(), value.size());

  if (!config_.mqtt().save()) {
    return copyReply(reply, "ERR");
  }

  mqtt_.restart(broker);
  return copyReply(reply, "OK");
}

bool MOCli::setMqttPort(MOMQTT::BrokerId broker, std::string_view value,
                        char* reply) noexcept {
  uint16_t port = 0;
  if (!parsePort(value, port)) {
    return copyReply(reply, "ERR");
  }

  auto& prefs = broker == MOMQTT::BrokerId::Mqtt1
                    ? config_.mqtt().mqtt1()
                    : config_.mqtt().mqtt2();
  prefs.port = port;

  if (!config_.mqtt().save()) {
    return copyReply(reply, "ERR");
  }

  mqtt_.restart(broker);
  return copyReply(reply, "OK");
}

bool MOCli::setMqttTransport(MOMQTT::BrokerId broker, std::string_view value,
                             char* reply) noexcept {
  MOTransport transport{};
  if (value == "tcp") {
    transport = MOTransport::Tcp;
  } else if (value == "wss") {
    transport = MOTransport::Wss;
  } else {
    return copyReply(reply, "ERR");
  }

  auto& prefs = broker == MOMQTT::BrokerId::Mqtt1
                    ? config_.mqtt().mqtt1()
                    : config_.mqtt().mqtt2();
  prefs.transport = transport;

  if (!config_.mqtt().save()) {
    return copyReply(reply, "ERR");
  }

  mqtt_.restart(broker);
  return copyReply(reply, "OK");
}

bool MOCli::setMqttUsername(MOMQTT::BrokerId broker, std::string_view value,
                            char* reply) noexcept {
  if (value.empty()) {
    return copyReply(reply, "ERR");
  }

  auto& prefs = broker == MOMQTT::BrokerId::Mqtt1
                    ? config_.mqtt().mqtt1()
                    : config_.mqtt().mqtt2();
  prefs.username.assign(value.data(), value.size());

  if (!config_.mqtt().save()) {
    return copyReply(reply, "ERR");
  }

  mqtt_.restart(broker);
  return copyReply(reply, "OK");
}

bool MOCli::setMqttPassword(MOMQTT::BrokerId broker, std::string_view value,
                            char* reply) noexcept {
  if (value.empty()) {
    return copyReply(reply, "ERR");
  }

  auto& prefs = broker == MOMQTT::BrokerId::Mqtt1
                    ? config_.mqtt().mqtt1()
                    : config_.mqtt().mqtt2();
  prefs.password.assign(value.data(), value.size());

  if (!config_.mqtt().save()) {
    return copyReply(reply, "ERR");
  }

  mqtt_.restart(broker);
  return copyReply(reply, "OK");
}

bool MOCli::setIata(std::string_view value, char* reply) noexcept {
  if (!isValidIata(value)) {
    return copyReply(reply, "ERR");
  }

  config_.mqtt().setIata(std::string(value));
  if (!config_.mqtt().save()) {
    return copyReply(reply, "ERR");
  }

  return copyReply(reply, "OK");
}

bool MOCli::restartWifi(char* reply) noexcept {
  wifi_.restart();
  return copyReply(reply, "OK");
}

bool MOCli::restartMqtt(MOMQTT::BrokerId broker, char* reply) noexcept {
  mqtt_.restart(broker);
  return copyReply(reply, "OK");
}

bool MOCli::splitSet(std::string_view command, std::string_view& key,
                     std::string_view& value) noexcept {
  if (!startsWith(command, "set ")) {
    return false;
  }

  const std::string_view body = command.substr(4);
  const size_t separator = body.find(' ');
  if (separator == std::string_view::npos) {
    key = body;
    value = {};
    return true;
  }

  key = body.substr(0, separator);
  value = trim(body.substr(separator + 1));
  return !key.empty();
}

bool MOCli::isValidIata(std::string_view value) noexcept {
  if (value.size() != 3) {
    return false;
  }

  for (const char c : value) {
    if (std::toupper(static_cast<unsigned char>(c)) !=
        static_cast<unsigned char>(c) ||
        !std::isalpha(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

bool MOCli::parsePort(std::string_view value, uint16_t& port) noexcept {
  if (value.empty() || value.size() > 5) {
    return false;
  }

  uint32_t parsed = 0;
  for (const char c : value) {
    if (c < '0' || c > '9') {
      return false;
    }
    parsed = parsed * 10U + static_cast<uint32_t>(c - '0');
    if (parsed > 65535U) {
      return false;
    }
  }

  if (parsed == 0) {
    return false;
  }

  port = static_cast<uint16_t>(parsed);
  return true;
}

bool MOCli::copyReply(char* reply, std::string_view text) noexcept {
  if (reply == nullptr || text.size() >= kReplyCapacity) {
    return false;
  }

  std::memcpy(reply, text.data(), text.size());
  reply[text.size()] = '\0';
  return true;
}

bool MOCli::startsWith(std::string_view value,
                       std::string_view prefix) noexcept {
  return value.size() >= prefix.size() &&
         value.substr(0, prefix.size()) == prefix;
}

std::string_view MOCli::trim(std::string_view value) noexcept {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front()))) {
    value.remove_prefix(1);
  }

  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.remove_suffix(1);
  }

  return value;
}

std::string_view MOCli::mqttName(MOMQTT::BrokerId broker) noexcept {
  return broker == MOMQTT::BrokerId::Mqtt1 ? "mqtt1" : "mqtt2";
}

std::string_view MOCli::stateName(MOMQTT::State state) noexcept {
  switch (state) {
    case MOMQTT::State::Disabled:
      return "DISABLED";
    case MOMQTT::State::WaitingForWiFi:
      return "WAITING_WIFI";
    case MOMQTT::State::WaitingForTime:
      return "WAITING_TIME";
    case MOMQTT::State::Disconnected:
      return "DISCONNECTED";
    case MOMQTT::State::Connecting:
      return "CONNECTING";
    case MOMQTT::State::Connected:
      return "CONNECTED";
    case MOMQTT::State::Backoff:
      return "BACKOFF";
    case MOMQTT::State::Error:
      return "ERROR";
  }
  return "UNKNOWN";
}

std::string_view MOCli::transportName(MOTransport transport) noexcept {
  return transport == MOTransport::Wss ? "wss" : "tcp";
}
