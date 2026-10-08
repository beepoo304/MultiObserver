from __future__ import annotations

import re

from .common import (
    CERT_BUNDLE_FLAG,
    MARKERS,
    MO_LIBRARY_DEP,
    PLATFORMIO_SECTION,
    WIFI_CLIENT_SECURE_IGNORE,
    InstallError,
)

def require_exactly_one(text: str, pattern: str, description: str) -> re.Match[str]:
    matches = list(re.finditer(pattern, text, re.MULTILINE))
    if len(matches) != 1:
        raise InstallError(
            f"{description}: expected exactly one match, found {len(matches)}."
        )
    return matches[0]

def _section_bounds(text: str, heading: str) -> tuple[int, int]:
    section_match = re.search(
        rf"^{re.escape(heading)}\s*$",
        text,
        re.MULTILINE,
    )
    if not section_match:
        raise InstallError(f"Cannot locate {heading} in platformio.ini.")

    next_section = re.search(
        r"^\[[^\]]+\]\s*$",
        text[section_match.end():],
        re.MULTILINE,
    )
    section_end = (
        section_match.end() + next_section.start()
        if next_section
        else len(text)
    )
    return section_match.start(), section_end


def _continuation_block(section: str, key: str) -> re.Match[str] | None:
    return re.search(
        rf"^{re.escape(key)}\s*=\s*\r?\n((?:^[ \t]+.*(?:\r?\n|$))*)",
        section,
        re.MULTILINE,
    )


def _require_block(section: str, key: str) -> re.Match[str]:
    match = _continuation_block(section, key)
    if match is None:
        raise InstallError(
            f"Cannot locate a conventional {key} continuation block "
            f"in {PLATFORMIO_SECTION}; refusing unsafe patch."
        )
    return match


def _block_contains(match: re.Match[str], item: str) -> bool:
    return any(line.strip() == item for line in match.group(1).splitlines())


def _append_block_item(section: str, key: str, item: str, newline: str) -> str:
    match = _require_block(section, key)
    if _block_contains(match, item):
        return section
    replacement = match.group(0).rstrip("\r\n") + newline + f"  {item}" + newline
    return section[:match.start()] + replacement + section[match.end():]


def _insert_block_before(
    section: str, anchor_key: str, key: str, items: tuple[str, ...], newline: str
) -> str:
    if re.search(rf"^{re.escape(key)}\s*=", section, re.MULTILINE):
        raise InstallError(f"Cannot safely parse {key} in {PLATFORMIO_SECTION}.")
    anchor = re.search(rf"^{re.escape(anchor_key)}\s*=", section, re.MULTILINE)
    if anchor is None:
        raise InstallError(f"Cannot locate {anchor_key} while adding {key}.")
    block = key + " =" + newline
    block += "".join(f"  {item}{newline}" for item in items)
    return section[:anchor.start()] + block + section[anchor.start():]


def _insert_block_after(
    section: str, anchor_key: str, key: str, items: tuple[str, ...], newline: str
) -> str:
    if re.search(rf"^{re.escape(key)}\s*=", section, re.MULTILINE):
        raise InstallError(f"Cannot safely parse {key} in {PLATFORMIO_SECTION}.")
    anchor = _require_block(section, anchor_key)
    block = key + " =" + newline
    block += "".join(f"  {item}{newline}" for item in items)
    return section[:anchor.end()] + block + section[anchor.end():]


def _ensure_block_item(
    section: str,
    key: str,
    item: str,
    newline: str,
    *,
    create_before: str | None = None,
    create_after: str | None = None,
    inherited_items: tuple[str, ...] = (),
) -> str:
    match = _continuation_block(section, key)
    if match is not None:
        return _append_block_item(section, key, item, newline)
    items = (*inherited_items, item)
    if create_before is not None:
        return _insert_block_before(section, create_before, key, items, newline)
    if create_after is not None:
        return _insert_block_after(section, create_after, key, items, newline)
    raise InstallError(f"Missing required {key} block in {PLATFORMIO_SECTION}.")


def patch_platformio(text: str) -> str:
    start, end = _section_bounds(text, PLATFORMIO_SECTION)
    section = text[start:end]
    newline = "\r\n" if "\r\n" in text else "\n"

    # Refuse unknown layouts instead of guessing where dependencies and flags live.
    _require_block(section, "lib_deps")
    _require_block(section, "build_flags")

    section = _ensure_block_item(
        section, "build_unflags", "-std=gnu++11", newline,
        create_before="build_flags",
    )
    section = _append_block_item(section, "build_flags", "-std=gnu++17", newline)
    section = _ensure_block_item(
        section,
        "extra_scripts",
        "pre:scripts/multiobserver_cpp17.py",
        newline,
        create_before="build_flags",
        inherited_items=("${esp32_base.extra_scripts}",),
    )
    section = _append_block_item(section, "build_flags", CERT_BUNDLE_FLAG, newline)
    section = _append_block_item(section, "lib_deps", MO_LIBRARY_DEP, newline)

    # ESP-IDF MQTT owns TLS; the unused Arduino wrapper breaks dependency
    # resolution on the verified ESP32 platform version.
    section = re.sub(
        r"^[ \t]+WiFi[ \t]*(?:\r?\n|$)", "", section, flags=re.MULTILINE
    )
    section = _ensure_block_item(
        section,
        "lib_ignore",
        WIFI_CLIENT_SECURE_IGNORE,
        newline,
        create_after="lib_deps",
    )
    return text[:start] + section + text[end:]

def patch_main(text: str) -> str:
    if MARKERS["main"] in text:
        return text

    text = text.replace(
        '#include "MyMesh.h"',
        '#include "MyMesh.h"\n#include "MOBridge.h"\n// ' + MARKERS["main"],
        1,
    )

    begin = "  the_mesh.begin(fs);"
    replacement = begin + "\n  mo_bridge.begin();"
    if text.count(begin) != 1:
        raise InstallError("Cannot patch main.cpp: the_mesh.begin(fs) is not unique.")
    text = text.replace(begin, replacement, 1)

    loop = "  the_mesh.loop();"
    if text.count(loop) != 1:
        raise InstallError("Cannot patch main.cpp: the_mesh.loop() is not unique.")
    text = text.replace(loop, loop + "\n  mo_bridge.loop();", 1)

    return text

def patch_tx_led_main(text: str) -> str:
    # GPIO35 is MeshCore's white packet LED and is already owned by MainBoard.
    # Remove guards installed by older MO releases instead of competing with
    # the board/radio lifecycle.
    legacy = """  mo_bridge.begin();

#ifdef P_LORA_TX_LED
  // Fail-safe for the Heltec V3 orange TX LED. MeshCore normally clears it
  // through onSendFinished(); this also guarantees a known state after boot.
  board.onAfterTransmit();
  Serial.printf("[MO][TXLED] off pin=%d reason=boot\\n", P_LORA_TX_LED);
#endif
  // MULTIOBSERVER: TX LED boot guard v1"""
    return text.replace(legacy, "  mo_bridge.begin();", 1)

def patch_mymesh_h(text: str) -> str:
    if MARKERS["mymesh_h"] in text:
        return text

    anchor = '#include "MyMesh.h"'
    if anchor in text:
        raise InstallError(
            'Unexpected self-include in MyMesh.h. Refusing unsafe patch.'
        )

    # MyMesh.h includes the common project headers; add the bridge include before
    # the class declaration without assuming a specific include ordering.
    class_anchor = "class MyMesh"
    if text.count(class_anchor) != 1:
        raise InstallError("Cannot locate unique MyMesh class declaration.")

    text = text.replace(
        class_anchor,
        '#include "MOBridge.h"\n\n// ' + MARKERS["mymesh_h"] + "\n\n" + class_anchor,
        1,
    )
    text += "\n\nextern MOBridge mo_bridge;\n"
    return text

def patch_mymesh_cpp(text: str) -> str:
    if MARKERS["mymesh_cpp"] in text:
        return text

    # MyMesh.cpp already includes MyMesh.h, therefore the bridge declaration is
    # available through it.
    include_anchor = '#include "MyMesh.h"'
    if text.count(include_anchor) != 1:
        raise InstallError('Cannot find unique #include "MyMesh.h" in MyMesh.cpp.')

    text = text.replace(
        include_anchor,
        include_anchor + "\n#include <Utils.h>\n\n// " + MARKERS["mymesh_cpp"] + "\nMOBridge mo_bridge;",
        1,
    )

    begin_anchor = "  _cli.loadPrefs(_fs);"
    if text.count(begin_anchor) != 1:
        raise InstallError("Cannot locate unique MeshCore prefs load anchor.")

    identity_code = (
        begin_anchor
        + "\n"
        + "  char mo_observer_public_key[sizeof(self_id.pub_key) * 2 + 1]{};\n"
        + "  mesh::Utils::toHex(mo_observer_public_key, self_id.pub_key,\n"
        + "                     sizeof(self_id.pub_key));\n"
        + "  mo_bridge.setObserverIdentity(getNodeName(),\n"
        + "                                mo_observer_public_key);"
    )
    text = text.replace(begin_anchor, identity_code, 1)

    rx = "void MyMesh::logRx(mesh::Packet *pkt, int len, float score) {"
    if text.count(rx) != 1:
        raise InstallError("Cannot locate unique MyMesh::logRx implementation.")
    text = text.replace(
        rx,
        rx
        + "\n"
        + "  mo_bridge.onRx(pkt, len, score, static_cast<int>(_radio->getLastRSSI()),"
        + " static_cast<int>(_radio->getEstAirtimeFor(len)));",
        1,
    )

    tx = "void MyMesh::logTx(mesh::Packet *pkt, int len) {"
    if text.count(tx) != 1:
        raise InstallError("Cannot locate unique MyMesh::logTx implementation.")
    text = text.replace(
        tx,
        tx
        + "\n  mo_bridge.onTx(pkt, len, static_cast<int>(radio_driver.getLastRSSI()),"
        + " radio_driver.getLastSNR());",
        1,
    )

    status_anchor = "  uptime_millis += now - last_millis;"
    if text.count(status_anchor) != 1:
        raise InstallError("Cannot locate unique MeshCore uptime update.")
    status_code = """  uptime_millis += now - last_millis;

  // EastMesh reference behavior: do not toggle Heltec ADC_CTRL in every
  // mesh loop. GPIO37 controls the battery divider on V3.2 and continuous
  // sampling can disturb the no-battery charging indication.
  static unsigned long mo_status_last_ms = 0;
  static unsigned long mo_battery_last_ms = 0;
  static uint16_t mo_battery_mv = 0;
  if (mo_status_last_ms == 0 || now - mo_status_last_ms >= 5000) {
    mo_status_last_ms = now;
    if (mo_battery_last_ms == 0 || now - mo_battery_last_ms >= 60000) {
      mo_battery_last_ms = now;
      mo_battery_mv = board.getBattMilliVolts();
    }

    MOBridge::StatusSnapshot mo_status{
      .status = "online",
      .timestamp = {},
      .origin = getNodeName(),
      .originId = {},
      .model = board.getManufacturerName(),
      .firmwareVersion = FIRMWARE_VERSION,
      .radio = {},
      .clientVersion = "github.com/beepoo304/MultiObserver",
      .repeat = _prefs.disable_fwd ? "off" : "on",
      .batteryMv = mo_battery_mv,
      .uptimeSecs = static_cast<uint32_t>(uptime_millis / 1000),
      .errors = _err_flags,
      .queueLen = static_cast<uint32_t>(_mgr->getOutboundTotal()),
      .noiseFloor = static_cast<int32_t>(_radio->getNoiseFloor()),
      .txAirSecs = static_cast<uint32_t>(getTotalAirTime() / 1000),
      .rxAirSecs = static_cast<uint32_t>(getReceiveAirTime() / 1000),
      .recvErrors = static_cast<uint32_t>(radio_driver.getPacketsRecvErrors()),
      .packetsSent = static_cast<uint32_t>(radio_driver.getPacketsSent()),
      .packetsReceived = static_cast<uint32_t>(radio_driver.getPacketsRecv()),
    };
    char mo_radio[48]{};
    snprintf(mo_radio, sizeof(mo_radio), "%.6f,%.1f,%u,%u",
             static_cast<double>(_prefs.freq),
             static_cast<double>(_prefs.bw),
             static_cast<unsigned>(_prefs.sf),
             static_cast<unsigned>(_prefs.cr));
    mo_status.radio = mo_radio;
    mo_bridge.setStatusSnapshot(mo_status);
  }"""
    text = text.replace(status_anchor, status_code, 1)

    cli = "_cli.handleCommand(sender_timestamp, command, reply);"
    if text.count(cli) != 1:
        raise InstallError("Cannot locate unique MeshCore CLI fallback.")
    text = text.replace(
        cli,
        "if (mo_bridge.handleCommand(sender_timestamp, command, reply)) {\n"
        "      return;\n"
        "    }\n    "
        + cli,
        1,
    )

    return text

def patch_alert_channel_bridge(mymesh_h: str, mymesh_cpp: str) -> tuple[str, str]:
    marker = "MULTIOBSERVER: native alert queue bridge v1"
    declaration = (
        "  bool enqueueMultiObserverAlert(const uint8_t* secret, "
        "size_t secretLength, const char* text);"
    )
    if declaration not in mymesh_h:
        anchor = "  const char* getNodeName() { return _prefs.node_name; }"
        if mymesh_h.count(anchor) != 1:
            raise InstallError("Cannot locate MyMesh public API for alert bridge.")
        mymesh_h = mymesh_h.replace(anchor, anchor + "\n" + declaration, 1)

    if marker in mymesh_cpp:
        return mymesh_h, mymesh_cpp

    global_anchor = "MOBridge mo_bridge;"
    if mymesh_cpp.count(global_anchor) != 1:
        raise InstallError("Cannot locate unique MultiObserver bridge instance.")
    callback = """MOBridge mo_bridge;

namespace {
bool moEnqueueAlert(void* context, const uint8_t* secret,
                    size_t secretLength, const char* text) {
  return context != nullptr &&
         static_cast<MyMesh*>(context)->enqueueMultiObserverAlert(
             secret, secretLength, text);
}
}  // namespace
// MULTIOBSERVER: native alert queue bridge v1"""
    mymesh_cpp = mymesh_cpp.replace(global_anchor, callback, 1)

    begin_anchor = "void MyMesh::begin(FILESYSTEM *fs) {"
    if mymesh_cpp.count(begin_anchor) != 1:
        raise InstallError("Cannot locate unique MyMesh::begin for alert bridge.")
    implementation = """bool MyMesh::enqueueMultiObserverAlert(
    const uint8_t* secret, size_t secretLength, const char* text) {
  if (secret == nullptr || (secretLength != 16 && secretLength != 32) ||
      text == nullptr) {
    return false;
  }

  mesh::GroupChannel channel{};
  memcpy(channel.secret, secret, secretLength);
  mesh::Utils::sha256(channel.hash, sizeof(channel.hash), channel.secret,
                      secretLength);

  constexpr size_t kAlertTextMax = 130;
  uint8_t payload[5 + kAlertTextMax + 1]{};
  const uint32_t timestamp = getRTCClock()->getCurrentTime();
  memcpy(payload, &timestamp, sizeof(timestamp));
  payload[4] = 0;  // TXT_TYPE_PLAIN
  const int prefixLength = snprintf(reinterpret_cast<char*>(&payload[5]),
                                    sizeof(payload) - 5, "%s: ", getNodeName());
  if (prefixLength < 0 ||
      static_cast<size_t>(prefixLength) >= kAlertTextMax) return false;
  size_t textLength = strlen(text);
  const size_t textCapacity = kAlertTextMax - static_cast<size_t>(prefixLength);
  if (textLength > textCapacity) textLength = textCapacity;
  memcpy(&payload[5 + prefixLength], text, textLength);

  mesh::Packet* packet = createGroupDatagram(
      PAYLOAD_TYPE_GRP_TXT, channel, payload, 5 + prefixLength + textLength);
  if (packet == nullptr) return false;
  sendFloodScoped(default_scope, packet, 0, _prefs.path_hash_mode + 1);
  return true;
}

"""
    mymesh_cpp = mymesh_cpp.replace(begin_anchor, implementation + begin_anchor, 1)
    mymesh_cpp = mymesh_cpp.replace(
        begin_anchor,
        begin_anchor + "\n  mo_bridge.setAlertSender(&moEnqueueAlert, this);",
        1,
    )
    return mymesh_h, mymesh_cpp

def patch_remote_cli_bridge(mymesh_h: str, mymesh_cpp: str) -> tuple[str, str]:
    marker = "MULTIOBSERVER: remote CLI bridge v1"
    channel_search = (
        "  int searchChannelsByHash(const uint8_t* hash, "
        "mesh::GroupChannel dest[], int maxMatches) override;"
    )
    channel_receive = (
        "  void onGroupDataRecv(mesh::Packet* packet, uint8_t type, "
        "const mesh::GroupChannel& channel, uint8_t* data, size_t len) override;"
    )
    if channel_search not in mymesh_h:
        anchor = "  int searchPeersByHash(const uint8_t* hash) override;"
        if mymesh_h.count(anchor) != 1:
            raise InstallError("Cannot locate MeshCore channel-search hook.")
        mymesh_h = mymesh_h.replace(
            anchor, anchor + "\n" + channel_search + "\n" + channel_receive, 1
        )

    if marker in mymesh_cpp:
        return mymesh_h, mymesh_cpp

    namespace_anchor = "}  // namespace\n// MULTIOBSERVER: native alert queue bridge v1"
    if mymesh_cpp.count(namespace_anchor) != 1:
        raise InstallError("Cannot locate MultiObserver callback namespace.")
    executor = """void moExecuteRemoteCli(void* context, uint32_t senderTimestamp,
                        char* command, char* reply) {
  if (context != nullptr) {
    static_cast<MyMesh*>(context)->handleCommand(senderTimestamp, command, reply);
  }
}
"""
    mymesh_cpp = mymesh_cpp.replace(
        namespace_anchor, executor + namespace_anchor, 1
    )

    begin_hook = "  mo_bridge.setAlertSender(&moEnqueueAlert, this);"
    if mymesh_cpp.count(begin_hook) != 1:
        raise InstallError("Cannot locate MultiObserver alert sender hook.")
    mymesh_cpp = mymesh_cpp.replace(
        begin_hook,
        begin_hook +
        "\n  mo_bridge.setRemoteCliExecutor(&moExecuteRemoteCli, this);",
        1,
    )

    implementation_anchor = "bool MyMesh::enqueueMultiObserverAlert("
    if mymesh_cpp.count(implementation_anchor) != 1:
        raise InstallError("Cannot locate MultiObserver channel sender.")
    implementation = """int MyMesh::searchChannelsByHash(
    const uint8_t* hash, mesh::GroupChannel dest[], int maxMatches) {
  if (hash == nullptr || dest == nullptr || maxMatches < 1) return 0;

  uint8_t secret[32]{};
  size_t secretLength = 0;
  if (!mo_bridge.copyRemoteCliChannelSecret(secret, sizeof(secret),
                                             secretLength)) return 0;

  mesh::GroupChannel channel{};
  memcpy(channel.secret, secret, secretLength);
  mesh::Utils::sha256(channel.hash, sizeof(channel.hash), channel.secret,
                      secretLength);
  if (channel.hash[0] != hash[0]) return 0;
  dest[0] = channel;
  return 1;
}

void MyMesh::onGroupDataRecv(mesh::Packet*, uint8_t type,
                             const mesh::GroupChannel&, uint8_t* data,
                             size_t len) {
  if (type != PAYLOAD_TYPE_GRP_TXT || data == nullptr || len < 5 ||
      (data[4] >> 2) != 0) return;

  uint32_t senderTimestamp = 0;
  memcpy(&senderTimestamp, data, sizeof(senderTimestamp));
  data[len] = 0;
  mo_bridge.handleRemoteCli(senderTimestamp, getNodeName(),
                            reinterpret_cast<const char*>(&data[5]));
}

// MULTIOBSERVER: remote CLI bridge v1
"""
    mymesh_cpp = mymesh_cpp.replace(
        implementation_anchor, implementation + implementation_anchor, 1
    )
    return mymesh_h, mymesh_cpp

def patch_tx_led_mymesh(text: str) -> str:
    # MeshCore's radio wrapper already calls onAfterTransmit() on success and
    # failure. Strip the redundant MO guards from v1/v2 installations.
    text = re.sub(
        r"void MyMesh::logTx\(mesh::Packet \*pkt, int len\) \{\n"
        r"#ifdef P_LORA_TX_LED\n.*?"
        r"// MULTIOBSERVER: TX LED completion guard v[12]\n"
        r"  mo_bridge\.onTx\(pkt, len, static_cast<int>\(radio_driver\.getLastRSSI\(\)\), radio_driver\.getLastSNR\(\)\);",
        "void MyMesh::logTx(mesh::Packet *pkt, int len) {\n"
        "  mo_bridge.onTx(pkt, len, static_cast<int>(radio_driver.getLastRSSI()), radio_driver.getLastSNR());",
        text,
        count=1,
        flags=re.DOTALL,
    )
    text = re.sub(
        r"void MyMesh::logTxFail\(mesh::Packet \*pkt, int len\) \{\n"
        r"#ifdef P_LORA_TX_LED\n.*?#endif",
        "void MyMesh::logTxFail(mesh::Packet *pkt, int len) {",
        text,
        count=1,
        flags=re.DOTALL,
    )
    return text

def patch_status_sampling(text: str) -> str:
    text = text.replace(
        '.clientVersion = "MultiObserver",',
        '.clientVersion = "github.com/beepoo304/MultiObserver",',
    )
    if "static unsigned long mo_battery_last_ms = 0;" in text:
        return text

    pattern = re.compile(
        r"  MOBridge::StatusSnapshot mo_status\{.*?"
        r"    mo_bridge\.setStatusSnapshot\(mo_status\);\n"
        r"  \}",
        re.DOTALL,
    )
    replacement = """  // EastMesh reference behavior: do not toggle Heltec ADC_CTRL in every
  // mesh loop. GPIO37 controls the battery divider on V3.2 and continuous
  // sampling can disturb the no-battery charging indication.
  static unsigned long mo_status_last_ms = 0;
  static unsigned long mo_battery_last_ms = 0;
  static uint16_t mo_battery_mv = 0;
  if (mo_status_last_ms == 0 || now - mo_status_last_ms >= 5000) {
    mo_status_last_ms = now;
    if (mo_battery_last_ms == 0 || now - mo_battery_last_ms >= 60000) {
      mo_battery_last_ms = now;
      mo_battery_mv = board.getBattMilliVolts();
    }

    MOBridge::StatusSnapshot mo_status{
        .status = "online",
        .timestamp = {},
        .origin = getNodeName(),
        .originId = {},
        .model = board.getManufacturerName(),
        .firmwareVersion = FIRMWARE_VERSION,
        .radio = {},
        .clientVersion = "github.com/beepoo304/MultiObserver",
        .repeat = _prefs.disable_fwd ? "off" : "on",
        .batteryMv = mo_battery_mv,
        .uptimeSecs = static_cast<uint32_t>(uptime_millis / 1000),
        .errors = _err_flags,
        .queueLen = static_cast<uint32_t>(_mgr->getOutboundTotal()),
        .noiseFloor = static_cast<int32_t>(_radio->getNoiseFloor()),
        .txAirSecs = static_cast<uint32_t>(getTotalAirTime() / 1000),
        .rxAirSecs = static_cast<uint32_t>(getReceiveAirTime() / 1000),
        .recvErrors = static_cast<uint32_t>(radio_driver.getPacketsRecvErrors()),
        .packetsSent = static_cast<uint32_t>(radio_driver.getPacketsSent()),
        .packetsReceived = static_cast<uint32_t>(radio_driver.getPacketsRecv()),
    };
    char mo_radio[48]{};
    snprintf(mo_radio, sizeof(mo_radio), "%.6f,%.1f,%u,%u",
             static_cast<double>(_prefs.freq),
             static_cast<double>(_prefs.bw),
             static_cast<unsigned>(_prefs.sf),
             static_cast<unsigned>(_prefs.cr));
    mo_status.radio = mo_radio;
    mo_bridge.setStatusSnapshot(mo_status);
  }"""
    text, count = pattern.subn(replacement, text, count=1)
    if count != 1:
        raise InstallError("Cannot migrate MultiObserver status sampling hook.")
    return text

def patch_rx_metadata(text: str) -> str:
    legacy = (
        "  mo_bridge.onRx(pkt, len, score, static_cast<int>(_radio->getLastRSSI()), -1);"
    )
    current = (
        "  mo_bridge.onRx(pkt, len, score, static_cast<int>(_radio->getLastRSSI()),"
        " static_cast<int>(_radio->getEstAirtimeFor(len)));"
    )
    if current in text:
        return text
    if text.count(legacy) != 1:
        raise InstallError("Cannot migrate MultiObserver RX metadata hook.")
    return text.replace(legacy, current, 1)

def patch_ui_wifi_ip(text: str) -> str:
    marker = "MULTIOBSERVER: EastMesh WiFi IP display v1"
    if marker in text:
        return text

    include_anchor = "#include <Arduino.h>"
    if text.count(include_anchor) != 1:
        raise InstallError("Cannot locate Arduino include in UITask.cpp.")
    text = text.replace(
        include_anchor,
        include_anchor
        + "\n#if defined(ESP_PLATFORM)\n#include <WiFi.h>\n#endif\n// "
        + marker,
        1,
    )

    screen_anchor = """    sprintf(tmp, "BW: %03.2f CR: %d", _node_prefs->bw, _node_prefs->cr);
    _display->print(tmp);"""
    if text.count(screen_anchor) != 1:
        raise InstallError("Cannot locate radio details screen in UITask.cpp.")
    screen = screen_anchor + """

#if defined(ESP_PLATFORM)
    _display->setCursor(0, 40);
    if (WiFi.status() == WL_CONNECTED) {
      IPAddress ip = WiFi.localIP();
      snprintf(tmp, sizeof(tmp), "IP: %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    } else {
      snprintf(tmp, sizeof(tmp), "IP: -");
    }
    _display->print(tmp);
#endif"""
    return text.replace(screen_anchor, screen, 1)
