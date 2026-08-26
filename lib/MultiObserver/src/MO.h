#pragma once

#include "MOConfig.h"
#include "MOCli.h"
#include "MOMQTT.h"
#include "MOWatchdog.h"
#include "MOWifi.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

class MO {
 public:
  static constexpr size_t kRxQueueCapacity = 8;
  static constexpr size_t kTxQueueCapacity = 4;
  static constexpr size_t kMaxRawPacketSize = 256;
  static constexpr size_t kMaxHashSize = 8;

  MO();
  MO(const MO&) = delete;
  MO& operator=(const MO&) = delete;

  void begin();
  void loop();
  void end();

  [[nodiscard]] bool isStarted() const noexcept;

  bool handleCommand(uint32_t senderTimestamp, const char* command,
                     char* reply) noexcept;

  bool enqueueRx(const uint8_t* raw, size_t rawLength, uint8_t payloadType,
                 uint16_t payloadLength, uint8_t routeType,
                 uint8_t pathHashCount, uint8_t pathHashSize,
                 int8_t snrQuarter, int rssi, int score, int duration,
                 const uint8_t* packetHash, size_t packetHashLength,
                 uint32_t timestamp) noexcept;

  bool enqueueTx(const uint8_t* raw, size_t rawLength, uint8_t payloadType,
                 uint16_t payloadLength, uint8_t routeType,
                 uint8_t pathHashCount, uint8_t pathHashSize,
                 int8_t snrQuarter, int rssi,
                 const uint8_t* packetHash, size_t packetHashLength,
                 uint32_t timestamp) noexcept;

  void setObserverIdentity(std::string origin, std::string originId);
  void setStatusSnapshot(const MOMQTT::StatusData& status);

 private:
  struct RxEvent {
    std::array<uint8_t, kMaxRawPacketSize> raw{};
    size_t rawLength{0};
    uint8_t payloadType{0};
    uint16_t payloadLength{0};
    uint8_t routeType{0};
    uint8_t pathHashCount{0};
    uint8_t pathHashSize{0};
    int8_t snrQuarter{0};
    int rssi{0};
    int score{-1};
    int duration{-1};
    std::array<uint8_t, kMaxHashSize> packetHash{};
    size_t packetHashLength{0};
    uint32_t timestamp{0};
  };

  struct TxEvent {
    std::array<uint8_t, kMaxRawPacketSize> raw{};
    size_t rawLength{0};
    uint8_t payloadType{0};
    uint16_t payloadLength{0};
    uint8_t routeType{0};
    uint8_t pathHashCount{0};
    uint8_t pathHashSize{0};
    int8_t snrQuarter{0};
    int rssi{0};
    std::array<uint8_t, kMaxHashSize> packetHash{};
    size_t packetHashLength{0};
    uint32_t timestamp{0};
  };

  void processRxEvent(const RxEvent& event);
  void processTxEvent(const TxEvent& event);
  static void appendHex(const uint8_t* data, size_t length, char* output,
                        size_t outputCapacity) noexcept;
  static void formatTimestamp(uint32_t timestamp, char* output,
                              size_t outputCapacity) noexcept;

  MOConfig config_;
  MOWifi wifi_;
  MOMQTT mqtt_;
  MOWatchdog watchdog_;
  MOCli cli_;

  std::array<RxEvent, kRxQueueCapacity> rxQueue_{};
  size_t rxHead_{0};
  size_t rxTail_{0};
  size_t rxCount_{0};

  std::array<TxEvent, kTxQueueCapacity> txQueue_{};
  size_t txHead_{0};
  size_t txTail_{0};
  size_t txCount_{0};

  std::string observerOrigin_;
  std::string observerId_;

  bool started_{false};
};
