#include "MOBridge.h"

#include <Packet.h>
#include <Arduino.h>

#include <algorithm>
#include <ctime>

void MOBridge::begin() {
  observer_.begin();
}

void MOBridge::loop() {
#if defined(HELTEC_LORA_V3) && defined(P_LORA_TX_LED)
  if (packetLedOffAt_ != 0 &&
      static_cast<int32_t>(millis() - packetLedOffAt_) >= 0) {
    digitalWrite(P_LORA_TX_LED, LOW);
    packetLedOffAt_ = 0;
  }
#endif
  observer_.loop();
}

void MOBridge::end() {
  observer_.end();
}

bool MOBridge::onRx(mesh::Packet* packet, int len, float score, int rssi, int duration) noexcept {
  if (packet == nullptr || len <= 0) {
    return false;
  }

  uint8_t raw[MO::kMaxRawPacketSize]{};
  const int rawLength = packet->writeTo(raw);
  if (rawLength <= 0 ||
      static_cast<size_t>(rawLength) > sizeof(raw)) {
    return false;
  }

  uint8_t packetHash[MO::kMaxHashSize]{};
  packet->calculatePacketHash(packetHash);

  pulsePacketLed();

  const int scoreValue = score >= 0.0f
                             ? static_cast<int>(score * 1000.0f)
                             : -1;

  return observer_.enqueueRx(
      raw, static_cast<size_t>(rawLength), packet->getPayloadType(),
      packet->payload_len, packet->getRouteType(),
      packet->getPathHashCount(), packet->getPathHashSize(),
      static_cast<int8_t>(std::clamp(packet->getSNR() * 4.0F, -128.0F, 127.0F)), rssi, scoreValue, duration, packetHash,
      sizeof(packetHash), static_cast<uint32_t>(time(nullptr)));
}

bool MOBridge::onTx(mesh::Packet* packet, int len) noexcept {
  return onTx(packet, len, 0, 0.0F);
}

bool MOBridge::onTx(mesh::Packet* packet, int len, int rssi,
                    float snr) noexcept {
  if (packet == nullptr || len <= 0) {
    return false;
  }

  uint8_t raw[MO::kMaxRawPacketSize]{};
  const int rawLength = packet->writeTo(raw);
  if (rawLength <= 0 ||
      static_cast<size_t>(rawLength) > sizeof(raw)) {
    return false;
  }

  uint8_t packetHash[MO::kMaxHashSize]{};
  packet->calculatePacketHash(packetHash);

  // logTx() is called after the radio has completed sending. Extend the
  // physical indication so fast SF6 packets remain visible to a human.
  pulsePacketLed();

  const int8_t snrQuarter = static_cast<int8_t>(
      std::clamp(snr * 4.0F, -128.0F, 127.0F));

  return observer_.enqueueTx(
      raw, static_cast<size_t>(rawLength), packet->getPayloadType(),
      packet->payload_len, packet->getRouteType(),
      packet->getPathHashCount(), packet->getPathHashSize(), snrQuarter, rssi,
      packetHash, sizeof(packetHash), static_cast<uint32_t>(time(nullptr)));
}

void MOBridge::setObserverIdentity(const char* name,
                                    const char* publicKeyHex) noexcept {
  if (name == nullptr || publicKeyHex == nullptr) {
    observer_.setObserverIdentity({}, {});
    return;
  }

  observer_.setObserverIdentity(name, publicKeyHex);
}

void MOBridge::setStatusSnapshot(
    const StatusSnapshot& status) noexcept {
  MOMQTT::StatusData mqtt_status{
      .status = status.status != nullptr ? status.status : "",
      .timestamp = status.timestamp != nullptr ? status.timestamp : "",
      .origin = status.origin != nullptr ? status.origin : "",
      .originId = status.originId != nullptr ? status.originId : "",
      .model = status.model != nullptr ? status.model : "",
      .firmwareVersion = status.firmwareVersion != nullptr
                             ? status.firmwareVersion
                             : "",
      .radio = status.radio != nullptr ? status.radio : "",
      .clientVersion = status.clientVersion != nullptr
                           ? status.clientVersion
                           : "",
      .repeat = status.repeat != nullptr ? status.repeat : "",
      .batteryMv = status.batteryMv,
      .uptimeSecs = status.uptimeSecs,
      .errors = status.errors,
      .queueLen = status.queueLen,
      .noiseFloor = status.noiseFloor,
      .txAirSecs = status.txAirSecs,
      .rxAirSecs = status.rxAirSecs,
      .recvErrors = status.recvErrors,
      .packetsSent = status.packetsSent,
      .packetsReceived = status.packetsReceived,
  };
  observer_.setStatusSnapshot(mqtt_status);
}

bool MOBridge::handleCommand(uint32_t senderTimestamp, const char* command,
                             char* reply) noexcept {
  return observer_.handleCommand(senderTimestamp, command, reply);
}

bool MOBridge::isStarted() const noexcept {
  return observer_.isStarted();
}

void MOBridge::pulsePacketLed() noexcept {
#if defined(HELTEC_LORA_V3) && defined(P_LORA_TX_LED)
  constexpr uint32_t kPacketLedPulseMs = 60;
  pinMode(P_LORA_TX_LED, OUTPUT);
  digitalWrite(P_LORA_TX_LED, HIGH);
  packetLedOffAt_ = millis() + kPacketLedPulseMs;
#endif
}
