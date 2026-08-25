#pragma once

#include "MOConfig.h"
#include "MOMQTT.h"
#include "MOWifi.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

class MOCli {
 public:
  static constexpr size_t kReplyCapacity = 160;

  MOCli(MOConfig& config, MOWifi& wifi, MOMQTT& mqtt);

  MOCli(const MOCli&) = delete;
  MOCli& operator=(const MOCli&) = delete;

  bool handleCommand(uint32_t senderTimestamp, const char* command,
                     char* reply) noexcept;

 private:
  bool handleWifi(std::string_view command, char* reply) noexcept;
  bool handleMqtt(std::string_view command, char* reply) noexcept;
  bool handleCustom(std::string_view command, char* reply) noexcept;

  bool setWifiSsid(std::string_view value, char* reply) noexcept;
  bool setWifiPassword(std::string_view value, char* reply) noexcept;
  bool setMqttState(MOMQTT::BrokerId broker, MOOnOff state,
                    char* reply) noexcept;
  bool setMqttHost(MOMQTT::BrokerId broker, std::string_view value,
                   char* reply) noexcept;
  bool setMqttPort(MOMQTT::BrokerId broker, std::string_view value,
                   char* reply) noexcept;
  bool setMqttTransport(MOMQTT::BrokerId broker, std::string_view value,
                        char* reply) noexcept;
  bool setMqttUsername(MOMQTT::BrokerId broker, std::string_view value,
                       char* reply) noexcept;
  bool setMqttPassword(MOMQTT::BrokerId broker, std::string_view value,
                       char* reply) noexcept;
  bool setIata(std::string_view value, char* reply) noexcept;

  bool restartWifi(char* reply) noexcept;
  bool restartMqtt(MOMQTT::BrokerId broker, char* reply) noexcept;

  static bool splitSet(std::string_view command, std::string_view& key,
                       std::string_view& value) noexcept;
  static bool isValidIata(std::string_view value) noexcept;
  static bool parsePort(std::string_view value, uint16_t& port) noexcept;
  static bool copyReply(char* reply, std::string_view text) noexcept;
  static bool startsWith(std::string_view value, std::string_view prefix) noexcept;
  static std::string_view trim(std::string_view value) noexcept;
  static std::string_view mqttName(MOMQTT::BrokerId broker) noexcept;
  static std::string_view stateName(MOMQTT::State state) noexcept;
  static std::string_view transportName(MOTransport transport) noexcept;

  MOConfig& config_;
  MOWifi& wifi_;
  MOMQTT& mqtt_;
};
