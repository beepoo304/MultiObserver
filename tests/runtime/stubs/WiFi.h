#pragma once
struct TestWiFi {
  int status() const { return 3; }
  int RSSI() const { return -60; }
};
inline TestWiFi WiFi;
