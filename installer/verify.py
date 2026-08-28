#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


SUPPORTED = {"1.15", "1.16", "1.17"}
MAIN_MARKER = "MULTIOBSERVER: lifecycle integration v2"
H_MARKER = "MULTIOBSERVER: bridge declaration v2"
CPP_MARKER = "MULTIOBSERVER: mesh hooks v2"
CPP17_SCRIPT = "scripts/multiobserver_cpp17.py"
LIBRARY_MANIFEST = "lib/MultiObserver/library.json"

HELTEC_V3_117_ENVS = {
    "Heltec_v3_repeater",
    "Heltec_v3_repeater_bridge_rs232",
    "Heltec_v3_repeater_bridge_espnow",
    "Heltec_v3_room_server",
    "Heltec_v3_terminal_chat",
    "Heltec_v3_companion_radio_usb",
    "Heltec_v3_companion_radio_ble",
    "Heltec_v3_companion_radio_wifi",
    "Heltec_v3_sensor",
    "Heltec_WSL3_repeater",
    "Heltec_WSL3_repeater_bridge_rs232",
    "Heltec_WSL3_repeater_bridge_espnow",
    "Heltec_WSL3_room_server",
    "Heltec_WSL3_companion_radio_ble",
    "Heltec_WSL3_companion_radio_usb",
    "Heltec_WSL3_companion_radio_wifi",
    "Heltec_WSL3_sensor",
    "Heltec_v3_kiss_modem",
}

MO_FILES = [
    "MO.h", "MO.cpp",
    "MOCli.h", "MOCli.cpp",
    "MOConfig.h", "MOConfig.cpp",
    "MOEtap2Prefs.h", "MOEtap2Prefs.cpp",
    "MOLocalTime.h", "MOLocalTime.cpp",
    "MOWatchdog.h", "MOWatchdog.cpp",
    "MOAlertChannel.h", "MOAlertChannel.cpp",
    "MOMQTT.h", "MOMQTT.cpp",
    "MOMQTTPrefs.h", "MOMQTTPrefs.cpp",
    "MOWifi.h", "MOWifi.cpp",
    "MOWifiPrefs.h", "MOWifiPrefs.cpp",
]

class VerifyError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Verify a MultiObserver installation in MeshCore."
    )
    p.add_argument("meshcore_root", type=Path)
    return p.parse_args()


def read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except Exception as exc:
        raise VerifyError(f"Cannot read: {path}") from exc


def version(root: Path) -> str:
    text = read(root / "examples/simple_repeater/MyMesh.h")
    m = re.search(r'FIRMWARE_VERSION\s+"v?(\d+\.\d+(?:\.\d+)?)"', text)
    if not m:
        raise VerifyError("Cannot detect MeshCore firmware version.")
    v = m.group(1)
    mm = v.rsplit(".", 1)[0] if v.count(".") == 2 else v
    if mm not in SUPPORTED:
        raise VerifyError(f"Unsupported MeshCore version family: {mm}")
    return mm


def main() -> int:
    root = parse_args().meshcore_root.resolve()

    required = [
        root / "examples/simple_repeater/main.cpp",
        root / "examples/simple_repeater/MyMesh.h",
        root / "examples/simple_repeater/MyMesh.cpp",
        root / "examples/simple_repeater/UITask.cpp",
        root / "examples/simple_repeater/MOBridge.h",
        root / "examples/simple_repeater/MOBridge.cpp",
        root / LIBRARY_MANIFEST,
        root / CPP17_SCRIPT,
    ] + [
        root / "lib/MultiObserver/src" / name for name in MO_FILES
    ]

    missing = [str(p) for p in required if not p.is_file()]
    if missing:
        raise VerifyError("Missing installed files:\n  " + "\n  ".join(missing))

    v = version(root)
    platformio_path = root / "variants/heltec_v3/platformio.ini"
    if not platformio_path.is_file():
        raise VerifyError(f"Missing PlatformIO configuration: {platformio_path}")
    platformio = read(platformio_path)

    main_cpp = read(root / "examples/simple_repeater/main.cpp")
    mymesh_h = read(root / "examples/simple_repeater/MyMesh.h")
    mymesh_cpp = read(root / "examples/simple_repeater/MyMesh.cpp")
    ui_cpp = read(root / "examples/simple_repeater/UITask.cpp")

    section_match = re.search(
        r"^\[env:Heltec_v3_repeater\]\s*$",
        platformio,
        re.MULTILINE,
    )
    if not section_match:
        raise VerifyError("Missing [env:Heltec_v3_repeater] in variants/heltec_v3/platformio.ini.")

    section_start = section_match.start()
    next_section = re.search(
        r"^\[[^\]]+\]\s*$",
        platformio[section_match.end():],
        re.MULTILINE,
    )
    section_end = (
        section_match.end() + next_section.start()
        if next_section
        else len(platformio)
    )
    repeater_section = platformio[section_start:section_end]

    try:
        manifest = json.loads(read(root / LIBRARY_MANIFEST))
    except json.JSONDecodeError as exc:
        raise VerifyError(f"Invalid MultiObserver library manifest: {exc}") from exc

    heltec_envs = set(re.findall(r"^\[env:([^\]]+)\]", platformio, re.MULTILINE))
    heltec_layout_ok = v != "1.17" or HELTEC_V3_117_ENVS.issubset(heltec_envs)

    checks = {
        "version": v in SUPPORTED,
        "main marker": main_cpp.count(MAIN_MARKER) == 1,
        "MyMesh.h marker": mymesh_h.count(H_MARKER) == 1,
        "MyMesh.cpp marker": mymesh_cpp.count(CPP_MARKER) == 1,
        "MOBridge include": main_cpp.count('#include "MOBridge.h"') == 1,
        "MOBridge declaration": mymesh_h.count('extern MOBridge mo_bridge;') == 1,
        "MOBridge definition": mymesh_cpp.count('MOBridge mo_bridge;') == 1,
        "identity conversion": "mesh::Utils::toHex" in mymesh_cpp,
        "identity name": "getNodeName()" in mymesh_cpp,
        "identity key": "self_id.pub_key" in mymesh_cpp,
        "bridge begin": main_cpp.count("mo_bridge.begin();") == 1,
        "bridge loop": main_cpp.count("mo_bridge.loop();") == 1,
        "MeshCore owns packet LED": "MULTIOBSERVER: TX LED" not in main_cpp
        and "[MO][TXLED]" not in main_cpp,
        "RX hook": mymesh_cpp.count("mo_bridge.onRx(pkt, len, score") == 1,
        "RX airtime metadata": "_radio->getEstAirtimeFor(len)" in mymesh_cpp,
        "TX hook": mymesh_cpp.count("mo_bridge.onTx(pkt, len, static_cast<int>(radio_driver.getLastRSSI()),") == 1,
        "no duplicate packet LED hook": "MULTIOBSERVER: TX LED" not in mymesh_cpp
        and "[MO][TXLED]" not in mymesh_cpp
        and "board.onAfterTransmit();" not in mymesh_cpp,
        "visible Heltec packet pulse": "pulsePacketLed();" in read(root / "examples/simple_repeater/MOBridge.cpp")
        and "kPacketLedPulseMs = 60" in read(root / "examples/simple_repeater/MOBridge.cpp"),
        "EastMesh WiFi IP display": "MULTIOBSERVER: EastMesh WiFi IP display v1" in ui_cpp
        and "WiFi.localIP()" in ui_cpp
        and '"IP: %u.%u.%u.%u"' in ui_cpp,
        "status bridge API": "MOBridge::StatusSnapshot" in mymesh_cpp,
        "status hook": mymesh_cpp.count("mo_bridge.setStatusSnapshot(mo_status);") == 1,
        "battery sampling cache": mymesh_cpp.count("mo_battery_last_ms") >= 3
        and "now - mo_battery_last_ms >= 60000" in mymesh_cpp
        and mymesh_cpp.count("mo_battery_mv = board.getBattMilliVolts();") == 1
        and ".batteryMv = mo_battery_mv" in mymesh_cpp,
        "no MOMQTT status leak": "MOMQTT::StatusData" not in mymesh_cpp,
        "CLI hook": mymesh_cpp.count(
            "if (mo_bridge.handleCommand(sender_timestamp, command, reply))"
        ) == 1,
        "native alert queue bridge": "MULTIOBSERVER: native alert queue bridge v1" in mymesh_cpp
        and "createGroupDatagram(" in mymesh_cpp
        and "sendFloodScoped(default_scope, packet" in mymesh_cpp
        and "mo_bridge.setAlertSender(&moEnqueueAlert, this);" in mymesh_cpp,
        "no MultiObserver alert queue": "alertQueue" not in mymesh_cpp
        and "retryAlert" not in mymesh_cpp,
        "remote CLI bridge": "MULTIOBSERVER: remote CLI bridge v1" in mymesh_cpp
        and "searchChannelsByHash" in mymesh_cpp
        and "onGroupDataRecv" in mymesh_cpp
        and "mo_bridge.setRemoteCliExecutor(&moExecuteRemoteCli, this);" in mymesh_cpp,
        "remote CLI native handler":
        "handleCommand(senderTimestamp, command, reply)" in mymesh_cpp,
        "watchdog CLI": all(command in read(root / "lib/MultiObserver/src/MOCli.cpp") for command in (
            "get wdg.status", "wdg.on", "wdg.off", "get wdg.grace",
            "wdg.grace", "restart.wdg", "get wdg.log", "wdg.log",
        )),
        "alert channel CLI": all(command in read(root / "lib/MultiObserver/src/MOCli.cpp") for command in (
            "get channel.status", "channel.on", "channel.off",
            "get channel.key", "channel.key", "test.channel",
        )),
        "remote CLI settings": all(command in read(root / "lib/MultiObserver/src/MOCli.cpp") for command in (
            "get rcli.status", "rcli.on", "rcli.off",
        )),
        "MOMQTT status": "publishStatus" in (root / "lib/MultiObserver/src/MOMQTT.cpp").read_text(encoding="utf-8"),
        "MOMQTT LWT": "last_will" in (root / "lib/MultiObserver/src/MOMQTT.cpp").read_text(encoding="utf-8"),
        "RAW publish": "mqtt_.publishRaw(raw)" in (root / "lib/MultiObserver/src/MO.cpp").read_text(encoding="utf-8"),
        "packet monitor logs": "[MO][PACKET] dir=rx" in (root / "lib/MultiObserver/src/MO.cpp").read_text(encoding="utf-8")
        and "[MO][PACKET] dir=tx" in (root / "lib/MultiObserver/src/MO.cpp").read_text(encoding="utf-8"),
        "startup logs": "[MO][MQTT1] runtime start" in (root / "lib/MultiObserver/src/MOMQTT.cpp").read_text(encoding="utf-8"),
        "no literal newline escape": "\\n" not in (root / "lib/MultiObserver/src/MOMQTT.h").read_text(encoding="utf-8"),
        "library manifest": manifest.get("name") == "MultiObserver",
        "Heltec environment layout": heltec_layout_ok,
        "PlatformIO MO dependency": "file://lib/MultiObserver" in repeater_section,
        "PlatformIO WiFiClientSecure ignore": re.search(
            r"^lib_ignore\s*=\s*\n(?:^[ \t]+.*\n)*?[ \t]+WiFiClientSecure\s*$",
            repeater_section,
            re.MULTILINE,
        ) is not None,
        "PlatformIO certificate bundle": "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=1" in repeater_section,
        "PlatformIO C++17 script": "pre:scripts/multiobserver_cpp17.py" in repeater_section,
        "PlatformIO C++17": "-std=gnu++17" in repeater_section,
        "PlatformIO C++11 unflag": "-std=gnu++11" in repeater_section.split("build_unflags", 1)[-1],
    }

    failed = [name for name, ok in checks.items() if not ok]
    if failed:
        raise VerifyError("Verification failed: " + ", ".join(failed))

    print(f"MultiObserver verification: PASS ({v}.x)")
    for name in checks:
        print(f"  {name}: OK")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except VerifyError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(2)
