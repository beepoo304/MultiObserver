#pragma once

#include "MO.h"

#include <cstdint>

namespace mesh {
class Packet;
}

class MOBridge {
 public:
  void begin();
  void loop();
  void end();

  bool onRx(mesh::Packet* packet, int len, float score, int rssi, int duration) noexcept;
  bool onTx(mesh::Packet* packet, int len) noexcept;

  bool handleCommand(uint32_t senderTimestamp, const char* command,
                     char* reply) noexcept;

  [[nodiscard]] bool isStarted() const noexcept;

 private:
  MO observer_;
};
