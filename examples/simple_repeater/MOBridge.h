#pragma once

#include "MO.h"

#include <cstdint>

namespace mesh {
class Packet;
}

class MOBridge {
 public:
  using AlertSender = MO::AlertSender;
  void begin();
  void loop();
  void end();

  bool onRx(mesh::Packet* packet, int len, float score, int rssi, int duration) noexcept;
  bool onTx(mesh::Packet* packet, int len) noexcept;
  bool onTx(mesh::Packet* packet, int len, int rssi, float snr) noexcept;

  struct StatusSnapshot {
    const char* status{nullptr};
    const char* timestamp{nullptr};
    const char* origin{nullptr};
    const char* originId{nullptr};
    const char* model{nullptr};
    const char* firmwareVersion{nullptr};
    const char* radio{nullptr};
    const char* clientVersion{nullptr};
    const char* repeat{nullptr};
    uint32_t batteryMv{0};
    uint32_t uptimeSecs{0};
    uint32_t errors{0};
    uint32_t queueLen{0};
    int32_t noiseFloor{0};
    uint32_t txAirSecs{0};
    uint32_t rxAirSecs{0};
    uint32_t recvErrors{0};
    uint32_t packetsSent{0};
    uint32_t packetsReceived{0};
  };

  void setObserverIdentity(const char* name,
                          const char* publicKeyHex) noexcept;
  void setAlertSender(AlertSender sender, void* context) noexcept;

  void setStatusSnapshot(const StatusSnapshot& status) noexcept;

  bool handleCommand(uint32_t senderTimestamp, const char* command,
                     char* reply) noexcept;

  [[nodiscard]] bool isStarted() const noexcept;

 private:
  void pulsePacketLed() noexcept;

  MO observer_;
  uint32_t packetLedOffAt_{0};
};
