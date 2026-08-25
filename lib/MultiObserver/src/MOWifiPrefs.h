#pragma once

#include <string>

class MOWifiPrefs {
 public:
  void defaults();
  bool load();
  bool save() const;

  [[nodiscard]] const std::string& ssid() const noexcept;
  [[nodiscard]] const std::string& password() const noexcept;

  void setSsid(std::string value);
  void setPassword(std::string value);

 private:
  std::string ssid_;
  std::string password_;
};
