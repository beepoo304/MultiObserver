#pragma once

#include <cstdint>
#include <string>

enum class MOOnOff : uint8_t {
  Off = 0,
  On = 1,
};

enum class MOTransport : uint8_t {
  Tcp = 0,
  Wss = 1,
};

struct MOBrokerPrefs {
  MOOnOff state{MOOnOff::Off};
  std::string host;
  uint16_t port{1883};
  MOTransport transport{MOTransport::Tcp};
  std::string username;
  std::string password;
};

class MOMQTTPrefs {
 public:
  void defaults();
  bool load();
  bool save() const;

  [[nodiscard]] const MOBrokerPrefs& mqtt1() const noexcept;
  [[nodiscard]] MOBrokerPrefs& mqtt1() noexcept;

  [[nodiscard]] const MOBrokerPrefs& mqtt2() const noexcept;
  [[nodiscard]] MOBrokerPrefs& mqtt2() noexcept;

  [[nodiscard]] const std::string& iata() const noexcept;
  void setIata(std::string value);

 private:
  MOBrokerPrefs mqtt1_;
  MOBrokerPrefs mqtt2_;
  std::string iata_;
};
