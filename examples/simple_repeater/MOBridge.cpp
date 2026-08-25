#include "MOBridge.h"

#include <Packet.h>

#include <algorithm>
#include <ctime>

void MOBridge::begin() {
  observer_.begin();
}

void MOBridge::loop() {
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

  const int scoreValue = score >= 0.0f
                             ? static_cast<int>(score)
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
    const MOMQTT::StatusData& status) noexcept {
  observer_.setStatusSnapshot(status);
}

bool MOBridge::handleCommand(uint32_t senderTimestamp, const char* command,
                             char* reply) noexcept {
  return observer_.handleCommand(senderTimestamp, command, reply);
}

bool MOBridge::isStarted() const noexcept {
  return observer_.isStarted();
}
