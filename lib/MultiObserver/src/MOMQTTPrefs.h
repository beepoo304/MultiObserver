#pragma once

#include <cstdint>
#include <string>

enum class MOOnOff : uint8_t {
  Off = 0,
  On = 1,
};

struct MOBrokerPrefs {
  MOOnOff state{MOOnOff::Off};
  std::string host;
  uint16_t port{1883};
  std::string username;
  std::string password;
  std::string clientId;
};

class MOMQTTPrefs {
 public:
  static constexpr const char* kStoragePath = "/multiobserver/mqtt.cfg";
  static constexpr uint32_t kFormatVersion = 1;

  void defaults();
  bool load();
  bool save() const;

  [[nodiscard]] const MOBrokerPrefs& mqtt1() const noexcept;
  [[nodiscard]] MOBrokerPrefs& mqtt1() noexcept;
  [[nodiscard]] const MOBrokerPrefs& mqtt2() const noexcept;
  [[nodiscard]] MOBrokerPrefs& mqtt2() noexcept;

  [[nodiscard]] const std::string& iata() const noexcept;
  void setIata(const std::string& value);

 private:
  MOBrokerPrefs mqtt1_;
  MOBrokerPrefs mqtt2_;
  std::string iata_;
};
