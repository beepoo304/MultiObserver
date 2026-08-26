#include "MO.h"

#include <Arduino.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <utility>

MO::MO()
    : mqtt_(config_.mqtt(), wifi_),
      watchdog_(config_.etap2(), wifi_, mqtt_),
      cli_(config_, wifi_, mqtt_) {}

void MO::begin() {
  if (started_) {
    return;
  }

  Serial.println("[MO] begin");
  const bool configLoaded = config_.load();
  Serial.printf("[MO] config load: %s\n", configLoaded ? "OK" : "defaults");
  wifi_.begin(config_.wifi().ssid(), config_.wifi().password());
  mqtt_.begin();
  watchdog_.begin();

  started_ = true;
  Serial.println("[MO] ready");
}

void MO::loop() {
  if (!started_) {
    return;
  }

  wifi_.loop();
  mqtt_.loop();
  watchdog_.loop();

  while (rxCount_ != 0) {
    const RxEvent event = rxQueue_[rxTail_];
    rxTail_ = (rxTail_ + 1) % rxQueue_.size();
    --rxCount_;
    processRxEvent(event);
  }

  while (txCount_ != 0) {
    const TxEvent event = txQueue_[txTail_];
    txTail_ = (txTail_ + 1) % txQueue_.size();
    --txCount_;
    processTxEvent(event);
  }
}

void MO::end() {
  if (!started_) {
    return;
  }

  mqtt_.end();
  wifi_.disconnect();

  rxHead_ = 0;
  rxTail_ = 0;
  rxCount_ = 0;
  txHead_ = 0;
  txTail_ = 0;
  txCount_ = 0;
  started_ = false;
}

bool MO::isStarted() const noexcept {
  return started_;
}

bool MO::handleCommand(uint32_t senderTimestamp, const char* command,
                       char* reply) noexcept {
  if (!started_) {
    return false;
  }

  return cli_.handleCommand(senderTimestamp, command, reply);
}

bool MO::enqueueRx(const uint8_t* raw, size_t rawLength, uint8_t payloadType,
                   uint16_t payloadLength, uint8_t routeType,
                   uint8_t pathHashCount, uint8_t pathHashSize,
                   int8_t snrQuarter, int rssi, int score, int duration,
                   const uint8_t* packetHash, size_t packetHashLength,
                   uint32_t timestamp) noexcept {
  if (!started_ || raw == nullptr || rawLength == 0 ||
      rawLength > kMaxRawPacketSize || rxCount_ >= rxQueue_.size()) {
    return false;
  }

  if (packetHashLength > kMaxHashSize) {
    return false;
  }

  RxEvent& event = rxQueue_[rxHead_];
  event.rawLength = rawLength;
  std::memcpy(event.raw.data(), raw, rawLength);

  event.payloadType = payloadType;
  event.payloadLength = payloadLength;
  event.routeType = routeType;
  event.pathHashCount = pathHashCount;
  event.pathHashSize = pathHashSize;
  event.snrQuarter = snrQuarter;
  event.rssi = rssi;
  event.score = score;
  event.duration = duration;
  event.packetHashLength = packetHashLength;
  event.timestamp = timestamp;

  std::fill(event.packetHash.begin(), event.packetHash.end(), 0);
  if (packetHash != nullptr && packetHashLength != 0) {
    std::memcpy(event.packetHash.data(), packetHash, packetHashLength);
  }

  rxHead_ = (rxHead_ + 1) % rxQueue_.size();
  ++rxCount_;
  return true;
}

bool MO::enqueueTx(const uint8_t* raw, size_t rawLength,
                   uint8_t payloadType, uint16_t payloadLength,
                   uint8_t routeType, uint8_t pathHashCount,
                   uint8_t pathHashSize, int8_t snrQuarter, int rssi,
                   const uint8_t* packetHash, size_t packetHashLength,
                   uint32_t timestamp) noexcept {
  if (!started_ || raw == nullptr || rawLength == 0 ||
      rawLength > kMaxRawPacketSize || txCount_ >= txQueue_.size()) {
    return false;
  }

  if (packetHashLength > kMaxHashSize) {
    return false;
  }

  TxEvent& event = txQueue_[txHead_];
  event.rawLength = rawLength;
  std::memcpy(event.raw.data(), raw, rawLength);

  event.payloadType = payloadType;
  event.payloadLength = payloadLength;
  event.routeType = routeType;
  event.pathHashCount = pathHashCount;
  event.pathHashSize = pathHashSize;
  event.snrQuarter = snrQuarter;
  event.rssi = rssi;
  event.packetHashLength = packetHashLength;
  event.timestamp = timestamp;

  std::fill(event.packetHash.begin(), event.packetHash.end(), 0);
  if (packetHash != nullptr && packetHashLength != 0) {
    std::memcpy(event.packetHash.data(), packetHash, packetHashLength);
  }

  txHead_ = (txHead_ + 1) % txQueue_.size();
  ++txCount_;
  return true;
}

void MO::setObserverIdentity(std::string origin, std::string originId) {
  observerOrigin_.clear();
  observerId_.clear();

  if (origin.empty() || originId.size() != 64) {
    return;
  }

  for (char c : originId) {
    const bool isHex =
        (c >= '0' && c <= '9') ||
        (c >= 'a' && c <= 'f') ||
        (c >= 'A' && c <= 'F');
    if (!isHex) {
      return;
    }
  }

  observerOrigin_ = std::move(origin);
  observerId_ = std::move(originId);

  for (char& c : observerId_) {
    if (c >= 'a' && c <= 'f') {
      c = static_cast<char>(c - ('a' - 'A'));
    }
  }

  mqtt_.setObserverIdentity(observerId_);
}

void MO::setStatusSnapshot(const MOMQTT::StatusData& status) {
  MOMQTT::StatusData snapshot = status;
  if (snapshot.origin.empty()) {
    snapshot.origin = observerOrigin_;
  }
  if (snapshot.originId.empty()) {
    snapshot.originId = observerId_;
  }
  mqtt_.setStatusSnapshot(snapshot);
}

void MO::processRxEvent(const RxEvent& event) {
  char rawHex[(kMaxRawPacketSize * 2) + 1]{};
  char hashHex[(kMaxHashSize * 2) + 1]{};
  char length[12]{};
  char payloadLength[12]{};
  char packetType[8]{};
  char snr[16]{};
  char rssi[16]{};
  char score[16]{};
  char duration[16]{};
  char path[32]{};
  char timestamp[32]{};
  char timeOnly[16]{};
  char dateOnly[16]{};

  appendHex(event.raw.data(), event.rawLength, rawHex, sizeof(rawHex));
  appendHex(event.packetHash.data(), event.packetHashLength, hashHex,
            sizeof(hashHex));

  std::snprintf(length, sizeof(length), "%u",
                static_cast<unsigned>(event.rawLength));
  std::snprintf(payloadLength, sizeof(payloadLength), "%u",
                static_cast<unsigned>(event.payloadLength));
  std::snprintf(packetType, sizeof(packetType), "%u",
                static_cast<unsigned>(event.payloadType));
  std::snprintf(snr, sizeof(snr), "%.1f",
                static_cast<double>(event.snrQuarter) / 4.0);
  std::snprintf(rssi, sizeof(rssi), "%d", event.rssi);
  if (event.score >= 0) {
    std::snprintf(score, sizeof(score), "%d", event.score);
  }
  if (event.duration >= 0) {
    std::snprintf(duration, sizeof(duration), "%d", event.duration);
  }
  formatTimestamp(event.timestamp, timestamp, sizeof(timestamp));
  const time_t timeValue = static_cast<time_t>(event.timestamp);
  struct tm utc{};
  if (timeValue != 0 && gmtime_r(&timeValue, &utc) != nullptr) {
    std::strftime(timeOnly, sizeof(timeOnly), "%H:%M:%S", &utc);
    std::strftime(dateOnly, sizeof(dateOnly), "%d/%m/%Y", &utc);
  }

  const char* route = "F";
  if (event.routeType == 2 || event.routeType == 3) {
    route = "D";
    std::snprintf(path, sizeof(path), "path_%ux%u_%ub",
                  static_cast<unsigned>(event.pathHashCount),
                  static_cast<unsigned>(event.pathHashSize),
                  static_cast<unsigned>(
                      event.pathHashCount * event.pathHashSize));
  }

  std::string_view pathView = route[0] == 'D' ? std::string_view(path) : std::string_view{};
  std::string scoreString;
  if (event.score >= 0) {
    scoreString = std::to_string(event.score);
  }

  MOMQTT::PacketData packet{
      .origin = observerOrigin_,
      .originId = observerId_,
      .timestamp = timestamp,
      .time = timeOnly,
      .date = dateOnly,
      .direction = "rx",
      .length = length,
      .packetType = packetType,
      .route = route,
      .payloadLength = payloadLength,
      .raw = rawHex,
      .snr = snr,
      .rssi = rssi,
      .hash = hashHex,
      .score = scoreString,
      .duration = event.duration >= 0 ? std::string_view(duration) : std::string_view{},
      .path = pathView,
  };

  Serial.printf(
      "[MO][PACKET] dir=rx type=%u payload_len=%u rssi=%d snr=%.1f score=%d duration=%d\n",
      static_cast<unsigned>(event.payloadType),
      static_cast<unsigned>(event.payloadLength), event.rssi,
      static_cast<double>(event.snrQuarter) / 4.0, event.score,
      event.duration);

  mqtt_.publishPacket(packet);

  const MOMQTT::RawData raw{
      .origin = observerOrigin_,
      .originId = observerId_,
      .timestamp = timestamp,
      .data = rawHex,
  };
  mqtt_.publishRaw(raw);
}

void MO::processTxEvent(const TxEvent& event) {
  char rawHex[(kMaxRawPacketSize * 2) + 1]{};
  char hashHex[(kMaxHashSize * 2) + 1]{};
  char length[12]{};
  char payloadLength[12]{};
  char packetType[8]{};
  char snr[16]{};
  char rssi[16]{};
  char timestamp[32]{};
  char timeOnly[16]{};
  char dateOnly[16]{};
  char path[32]{};

  appendHex(event.raw.data(), event.rawLength, rawHex, sizeof(rawHex));
  appendHex(event.packetHash.data(), event.packetHashLength, hashHex,
            sizeof(hashHex));

  std::snprintf(length, sizeof(length), "%u",
                static_cast<unsigned>(event.rawLength));
  std::snprintf(payloadLength, sizeof(payloadLength), "%u",
                static_cast<unsigned>(event.payloadLength));
  std::snprintf(packetType, sizeof(packetType), "%u",
                static_cast<unsigned>(event.payloadType));
  std::snprintf(snr, sizeof(snr), "%.1f",
                static_cast<double>(event.snrQuarter) / 4.0);
  std::snprintf(rssi, sizeof(rssi), "%d", event.rssi);
  formatTimestamp(event.timestamp, timestamp, sizeof(timestamp));
  const time_t timeValue = static_cast<time_t>(event.timestamp);
  struct tm utc{};
  if (timeValue != 0 && gmtime_r(&timeValue, &utc) != nullptr) {
    std::strftime(timeOnly, sizeof(timeOnly), "%H:%M:%S", &utc);
    std::strftime(dateOnly, sizeof(dateOnly), "%d/%m/%Y", &utc);
  }

  const char* route = "F";
  if (event.routeType == 2 || event.routeType == 3) {
    route = "D";
    std::snprintf(path, sizeof(path), "path_%ux%u_%ub",
                  static_cast<unsigned>(event.pathHashCount),
                  static_cast<unsigned>(event.pathHashSize),
                  static_cast<unsigned>(
                      event.pathHashCount * event.pathHashSize));
  }

  const std::string_view pathView =
      route[0] == 'D' ? std::string_view(path) : std::string_view{};

  MOMQTT::PacketData packet{
      .origin = observerOrigin_,
      .originId = observerId_,
      .timestamp = timestamp,
      .time = timeOnly,
      .date = dateOnly,
      .direction = "tx",
      .length = length,
      .packetType = packetType,
      .route = route,
      .payloadLength = payloadLength,
      .raw = rawHex,
      .snr = snr,
      .rssi = rssi,
      .hash = hashHex,
      .score = {},
      .duration = {},
      .path = pathView,
  };

  Serial.printf(
      "[MO][PACKET] dir=tx type=%u payload_len=%u rssi=%d snr=%.1f\n",
      static_cast<unsigned>(event.payloadType),
      static_cast<unsigned>(event.payloadLength), event.rssi,
      static_cast<double>(event.snrQuarter) / 4.0);

  mqtt_.publishPacket(packet);

  const MOMQTT::RawData raw{
      .origin = observerOrigin_,
      .originId = observerId_,
      .timestamp = timestamp,
      .data = rawHex,
  };
  mqtt_.publishRaw(raw);
}

void MO::appendHex(const uint8_t* data, size_t length, char* output,
                   size_t outputCapacity) noexcept {
  if (output == nullptr || outputCapacity == 0) {
    return;
  }

  size_t used = 0;
  for (size_t i = 0; i < length && used + 2 < outputCapacity; ++i) {
    const int written = std::snprintf(
        output + used, outputCapacity - used, "%02X", data[i]);
    if (written != 2) {
      break;
    }
    used += 2;
  }

  output[used] = '\0';
}

void MO::formatTimestamp(uint32_t timestamp, char* output,
                         size_t outputCapacity) noexcept {
  if (output == nullptr || outputCapacity == 0) {
    return;
  }

  output[0] = '\0';

  if (timestamp == 0) {
    return;
  }

  const time_t timeValue = static_cast<time_t>(timestamp);
  struct tm utc{};
  if (gmtime_r(&timeValue, &utc) == nullptr) {
    return;
  }

  std::strftime(output, outputCapacity, "%Y-%m-%dT%H:%M:%S.000000", &utc);
}
