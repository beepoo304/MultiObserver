#pragma once

#include <string>

class MOWifiPrefs {
 public:
  static constexpr const char* kStoragePath = "/multiobserver/wifi.cfg";
  static constexpr uint32_t kFormatVersion = 1;

  void defaults();
  bool load();
  bool save() const;

  [[nodiscard]] const std::string& ssid() const noexcept;
  [[nodiscard]] const std::string& password() const noexcept;

  void setSsid(const std::string& value);
  void setPassword(const std::string& value);

 private:
  std::string ssid_;
  std::string password_;
};
