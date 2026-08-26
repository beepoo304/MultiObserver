#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import datetime as dt
import re
import shutil
import sys
from pathlib import Path


SUPPORTED = {
    "1.15": "MeshCore 1.15.x",
    "1.16": "MeshCore 1.16.x",
    "1.17": "MeshCore 1.17.x",
}

MARKERS = {
    "main": "MULTIOBSERVER: lifecycle integration v2",
    "mymesh_h": "MULTIOBSERVER: bridge declaration v2",
    "mymesh_cpp": "MULTIOBSERVER: mesh hooks v2",
}

MO_FILES = [
    "MO.h",
    "MO.cpp",
    "MOCli.h",
    "MOCli.cpp",
    "MOConfig.h",
    "MOConfig.cpp",
    "MOMQTT.h",
    "MOMQTT.cpp",
    "MOMQTTPrefs.h",
    "MOMQTTPrefs.cpp",
    "MOWifi.h",
    "MOWifi.cpp",
    "MOWifiPrefs.h",
    "MOWifiPrefs.cpp",
]

MESH_FILES = [
    Path("examples/simple_repeater/main.cpp"),
    Path("examples/simple_repeater/MyMesh.h"),
    Path("examples/simple_repeater/MyMesh.cpp"),
    Path("examples/simple_repeater/UITask.cpp"),
]

CONFIG_FILES = [
    Path("variants/heltec_v3/platformio.ini"),
]

PLATFORMIO_SECTION = "[env:Heltec_v3_repeater]"
MO_LIBRARY_DEP = "file://lib/MultiObserver"
WIFI_CLIENT_SECURE_IGNORE = "WiFiClientSecure"
CERT_BUNDLE_FLAG = "  -D CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=1"
CPP17_SCRIPT = Path("scripts/multiobserver_cpp17.py")
LIBRARY_MANIFEST = Path("lib/MultiObserver/library.json")
BACKUP_STATE_FILE = "multiobserver-backup.json"

PAYLOAD_PATHS = [
    Path("lib/MultiObserver"),
    Path("examples/simple_repeater/MOBridge.h"),
    Path("examples/simple_repeater/MOBridge.cpp"),
    CPP17_SCRIPT,
]

class InstallError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Install MultiObserver into a MeshCore simple_repeater tree."
    )
    p.add_argument("meshcore_root", type=Path)
    p.add_argument(
        "--source-root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="MultiObserver repository root.",
    )
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--repeater-name", help="Repeater name written to /prefs.json on first filesystem upload.")
    p.add_argument("--public-key", help="Observer public key, 64 hexadecimal characters.")
    p.add_argument("--private-key", help="Observer private key, 128 hexadecimal characters.")
    p.add_argument("--prepare-fs", action="store_true", help="Prepare first-boot filesystem data.")
    p.add_argument(
        "--rollback",
        type=Path,
        metavar="BACKUP_DIR",
        help="Restore files from a backup created by this installer.",
    )
    return p.parse_args()


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except Exception as exc:
        raise InstallError(f"Cannot read UTF-8 file: {path}") from exc


def write_text(path: Path, text: str) -> None:
    path.write_text(text, encoding="utf-8", newline="\n")


def detect_version(root: Path, mymesh_h: str) -> str:
    candidates = []

    for rel in (
        Path("examples/simple_repeater/MyMesh.h"),
        Path("examples/simple_repeater/MyMesh.cpp"),
    ):
        text = read_text(root / rel)
        candidates.extend(re.findall(r'FIRMWARE_VERSION\s+"v?(\d+\.\d+(?:\.\d+)?)"', text))

    if not candidates:
        raise InstallError("Cannot detect MeshCore firmware version.")

    major_minor = {v.rsplit(".", 1)[0] if v.count(".") == 2 else v for v in candidates}
    if len(major_minor) != 1:
        raise InstallError(f"Conflicting MeshCore versions detected: {sorted(major_minor)}")

    version = next(iter(major_minor))
    if version not in SUPPORTED:
        raise InstallError(
            f"Unsupported MeshCore version family: {version}. "
            f"Supported: {', '.join(SUPPORTED)}"
        )

    return version


def require_exactly_one(text: str, pattern: str, description: str) -> re.Match[str]:
    matches = list(re.finditer(pattern, text, re.MULTILINE))
    if len(matches) != 1:
        raise InstallError(
            f"{description}: expected exactly one match, found {len(matches)}."
        )
    return matches[0]


def validate_meshcore(root: Path) -> str:
    paths = [root / p for p in MESH_FILES]
    missing = [str(p) for p in paths if not p.is_file()]
    if missing:
        raise InstallError("Missing simple_repeater files:\n  " + "\n  ".join(missing))

    mymesh_h = read_text(root / MESH_FILES[1])
    mymesh_cpp = read_text(root / MESH_FILES[2])
    main_cpp = read_text(root / MESH_FILES[0])

    version = detect_version(root, mymesh_h)

    # These are the stable integration contracts verified against the supplied
    # MeshCore 1.15.0, 1.16.0 and 1.17.1 source trees.
    require_exactly_one(
        mymesh_cpp,
        r"void\s+MyMesh::logRx\s*\(\s*mesh::Packet\s*\*\s*pkt\s*,\s*int\s+len\s*,\s*float\s+score\s*\)\s*\{",
        "MyMesh::logRx hook",
    )
    require_exactly_one(
        mymesh_cpp,
        r"void\s+MyMesh::logTx\s*\(\s*mesh::Packet\s*\*\s*pkt\s*,\s*int\s+len\s*\)\s*\{",
        "MyMesh::logTx hook",
    )
    require_exactly_one(
        mymesh_cpp,
        r"_cli\.handleCommand\s*\(\s*sender_timestamp\s*,\s*command\s*,\s*reply\s*\)",
        "MeshCore CLI fallback",
    )
    require_exactly_one(
        main_cpp,
        r"the_mesh\.begin\s*\(\s*fs\s*\)\s*;",
        "simple_repeater begin lifecycle",
    )
    require_exactly_one(
        main_cpp,
        r"the_mesh\.loop\s*\(\s*\)\s*;",
        "simple_repeater loop lifecycle",
    )
    if "the_mesh.self_id.pub_key" not in main_cpp:
        raise InstallError("MeshCore identity public key access not found.")
    if "getNodeName()" not in mymesh_h:
        raise InstallError("MeshCore node name access not found.")

    # Existing installations are accepted only if all markers are present.
    marker_state = [
        MARKERS["main"] in main_cpp,
        MARKERS["mymesh_h"] in mymesh_h,
        MARKERS["mymesh_cpp"] in mymesh_cpp,
    ]
    if any(marker_state) and not all(marker_state):
        raise InstallError(
            "Partial MultiObserver installation detected. "
            "Restore the previous backup before reinstalling."
        )

    return version


def validate_source(source_root: Path) -> None:
    source_lib = source_root / "lib" / "MultiObserver" / "src"
    required = [source_lib / f for f in MO_FILES]
    required += [
        source_root / "examples/simple_repeater/MOBridge.h",
        source_root / "examples/simple_repeater/MOBridge.cpp",
        source_root / LIBRARY_MANIFEST,
        source_root / "installer/multiobserver_cpp17.py",
    ]
    missing = [str(p) for p in required if not p.is_file()]
    if missing:
        raise InstallError("Missing MultiObserver source files:\n  " + "\n  ".join(missing))



def validate_platformio(root: Path) -> None:
    path = root / CONFIG_FILES[0]
    if not path.is_file():
        raise InstallError(f"Missing PlatformIO configuration: {path}")

    text = read_text(path)
    if PLATFORMIO_SECTION not in text:
        raise InstallError(
            f"Cannot locate {PLATFORMIO_SECTION} in {path}."
        )

    section_start = text.index(PLATFORMIO_SECTION)
    next_section = re.search(r"^\[[^\]]+\]\s*$", text[section_start + len(PLATFORMIO_SECTION):], re.MULTILINE)
    section_end = (
        section_start + len(PLATFORMIO_SECTION) + next_section.start()
        if next_section
        else len(text)
    )
    section = text[section_start:section_end]

    if "build_src_filter" not in section or "../examples/simple_repeater" not in section:
        raise InstallError(
            f"{PLATFORMIO_SECTION} does not include examples/simple_repeater in build_src_filter."
        )
    if not re.search(r"^lib_deps\s*=", section, re.MULTILINE):
        raise InstallError(
            f"{PLATFORMIO_SECTION} has no lib_deps block; refusing to invent its structure."
        )


def patch_platformio(text: str) -> str:
    section_match = re.search(
        rf"^{re.escape(PLATFORMIO_SECTION)}\s*$",
        text,
        re.MULTILINE,
    )
    if not section_match:
        raise InstallError(
            f"Cannot locate {PLATFORMIO_SECTION} in platformio.ini."
        )

    section_start = section_match.start()
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
    section = text[section_start:section_end]

    lib_deps_match = re.search(
        r"^lib_deps\s*=\s*\n((?:^[ \t]+.*\n)*)",
        section,
        re.MULTILINE,
    )
    if not lib_deps_match:
        raise InstallError(
            "Cannot locate a conventional lib_deps continuation block "
            f"in {PLATFORMIO_SECTION}; refusing unsafe patch."
        )

    build_flags_match = re.search(
        r"^build_flags\s*=\s*\n((?:^[ \t]+.*\n)*)",
        section,
        re.MULTILINE,
    )
    if not build_flags_match:
        raise InstallError(
            "Cannot locate a conventional build_flags continuation block "
            f"in {PLATFORMIO_SECTION}; refusing unsafe patch."
        )

    changed = False
    newline = "\r\n" if "\r\n" in text else "\n"
    patched_section = section

    # MultiObserver requires C++17. Remove the inherited C++11 selector when
    # present and explicitly append C++17 to this environment only.
    if not re.search(r"^build_unflags\s*=", patched_section, re.MULTILINE):
        build_flags_pos = re.search(r"^build_flags\s*=", patched_section, re.MULTILINE)
        if not build_flags_pos:
            raise InstallError(
                f"Cannot locate build_flags in {PLATFORMIO_SECTION}."
            )
        insert_at = build_flags_pos.start()
        patched_section = (
            patched_section[:insert_at]
            + "build_unflags =" + newline
            + "  -std=gnu++11" + newline
            + patched_section[insert_at:]
        )
        changed = True
    else:
        unflags_match = re.search(
            r"^build_unflags\s*=\s*\n((?:^[ \t]+.*\n)*)",
            patched_section,
            re.MULTILINE,
        )
        if not unflags_match:
            raise InstallError(
                f"Cannot parse build_unflags in {PLATFORMIO_SECTION}."
            )
        unflags_block = unflags_match.group(0)
        if "-std=gnu++11" not in unflags_block:
            replacement = unflags_block.rstrip("\r\n") + newline + "  -std=gnu++11" + newline
            patched_section = (
                patched_section[:unflags_match.start()]
                + replacement
                + patched_section[unflags_match.end():]
            )
            changed = True

    build_flags_match = re.search(
        r"^build_flags\s*=\s*\n((?:^[ \t]+.*\n)*)",
        patched_section,
        re.MULTILINE,
    )
    if not build_flags_match:
        raise InstallError(
            f"Cannot re-locate build_flags in {PLATFORMIO_SECTION}."
        )
    build_flags_block = build_flags_match.group(0)
    if "-std=gnu++17" not in build_flags_block:
        replacement = (
            build_flags_block.rstrip("\r\n")
            + newline
            + "  -std=gnu++17"
            + newline
        )
        patched_section = (
            patched_section[:build_flags_match.start()]
            + replacement
            + patched_section[build_flags_match.end():]
        )
        changed = True

    # Force the effective C++ standard for every C++ translation unit in
    # this environment. The Arduino ESP32 platform can inject its own
    # -std=gnu++11 after build_flags; the pre-build script removes any
    # existing -std selector and appends -std=gnu++17.
    if "pre:scripts/multiobserver_cpp17.py" not in patched_section:
        extra_match = re.search(
            r"^extra_scripts\s*=\s*\n((?:^[ \t]+.*\n)*)",
            patched_section,
            re.MULTILINE,
        )
        if extra_match:
            block = extra_match.group(0)
            replacement = block.rstrip("\r\n") + newline + "  pre:scripts/multiobserver_cpp17.py" + newline
            patched_section = (
                patched_section[:extra_match.start()]
                + replacement
                + patched_section[extra_match.end():]
            )
        else:
            build_flags_pos = re.search(r"^build_flags\s*=", patched_section, re.MULTILINE)
            if not build_flags_pos:
                raise InstallError("Cannot locate build_flags for extra_scripts patch.")
            insert_at = build_flags_pos.start()
            patched_section = (
                patched_section[:insert_at]
                + "extra_scripts =" + newline
                + "  ${esp32_base.extra_scripts}" + newline
                + "  pre:scripts/multiobserver_cpp17.py" + newline
                + patched_section[insert_at:]
            )
        changed = True

    # Match the working EastMesh ESP32 MQTT configuration.
    # MOMQTT uses esp_crt_bundle_attach for WSS TLS.
    if "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=1" not in patched_section:
        build_flags_match = re.search(
            r"^build_flags\s*=\s*\n((?:^[ \t]+.*\n)*)",
            patched_section,
            re.MULTILINE,
        )
        if not build_flags_match:
            raise InstallError(
                f"Cannot locate build_flags while enabling the ESP32 certificate bundle in {PLATFORMIO_SECTION}."
            )
        block = build_flags_match.group(0)
        replacement = block.rstrip("\r\n") + newline + CERT_BUNDLE_FLAG + newline
        patched_section = (
            patched_section[:build_flags_match.start()]
            + replacement
            + patched_section[build_flags_match.end():]
        )
        changed = True

    # Add the local MO dependency if it is not already present.
    if MO_LIBRARY_DEP not in patched_section:
        lib_deps_match = re.search(
            r"^lib_deps\s*=\s*\n((?:^[ \t]+.*\n)*)",
            patched_section,
            re.MULTILINE,
        )
        if not lib_deps_match:
            raise InstallError(
                "Cannot re-locate lib_deps after PlatformIO patch."
            )
        block = lib_deps_match.group(0)
        replacement = block.rstrip("\r\n") + newline + "  " + MO_LIBRARY_DEP + newline
        patched_section = (
            patched_section[:lib_deps_match.start()]
            + replacement
            + patched_section[lib_deps_match.end():]
        )
        changed = True

    # MultiObserver uses ESP-IDF MQTT directly and does not use the Arduino
    # WiFiClientSecure wrapper. PlatformIO's LDF can nevertheless pull the
    # wrapper into this environment through other framework libraries, where
    # it fails to resolve its private WiFi include path with ESP32 6.11.0.
    # Excluding only that unused wrapper is the configuration proven by the
    # Heltec V3 build; the framework WiFi library remains available to MOWifi.
    explicit_wifi_dep = re.compile(
        r"^[ \t]+WiFi[ \t]*(?:\r?\n|$)",
        re.MULTILINE,
    )
    if explicit_wifi_dep.search(patched_section):
        patched_section = explicit_wifi_dep.sub("", patched_section)
        changed = True

    lib_ignore_match = re.search(
        r"^lib_ignore\s*=\s*\n((?:^[ \t]+.*\n)*)",
        patched_section,
        re.MULTILINE,
    )
    if lib_ignore_match:
        ignore_block = lib_ignore_match.group(0)
        if not re.search(
            r"^[ \t]+" + re.escape(WIFI_CLIENT_SECURE_IGNORE) + r"\s*$",
            ignore_block,
            re.MULTILINE,
        ):
            replacement = (
                ignore_block.rstrip("\r\n")
                + newline
                + "  "
                + WIFI_CLIENT_SECURE_IGNORE
                + newline
            )
            patched_section = (
                patched_section[:lib_ignore_match.start()]
                + replacement
                + patched_section[lib_ignore_match.end():]
            )
            changed = True
    else:
        lib_deps_match = re.search(
            r"^lib_deps\s*=\s*\n((?:^[ \t]+.*\n)*)",
            patched_section,
            re.MULTILINE,
        )
        if not lib_deps_match:
            raise InstallError(
                "Cannot locate lib_deps while adding the WiFiClientSecure exclusion."
            )
        insertion = (
            "lib_ignore ="
            + newline
            + "  "
            + WIFI_CLIENT_SECURE_IGNORE
            + newline
        )
        patched_section = (
            patched_section[:lib_deps_match.end()]
            + insertion
            + patched_section[lib_deps_match.end():]
        )
        changed = True

    if not changed:
        return text

    return text[:section_start] + patched_section + text[section_end:]



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
      .clientVersion = "MultiObserver",
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
        .clientVersion = "MultiObserver",
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



def validate_identity_args(args: argparse.Namespace) -> tuple[str, str, str] | None:
    values = (args.repeater_name, args.public_key, args.private_key)
    if all(value is None for value in values):
        return None
    if any(value is None for value in values):
        raise InstallError(
            "--repeater-name, --public-key and --private-key must be supplied together."
        )

    name = args.repeater_name.strip()
    public_key = args.public_key.strip().upper()
    private_key = args.private_key.strip().upper()

    if not name or len(name.encode("utf-8")) > 31:
        raise InstallError("Repeater name must be 1..31 UTF-8 bytes.")
    if len(public_key) != 64 or not re.fullmatch(r"[0-9A-F]{64}", public_key):
        raise InstallError("Public key must contain exactly 64 hexadecimal characters.")
    if len(private_key) != 128 or not re.fullmatch(r"[0-9A-F]{128}", private_key):
        raise InstallError("Private key must contain exactly 128 hexadecimal characters.")

    return name, public_key, private_key


def prepare_filesystem(root: Path, name: str, public_key: str,
                       private_key: str, dry_run: bool) -> None:
    data_root = root / "data"
    identity_path = data_root / "identity" / "_main.id"
    prefs_path = data_root / "prefs.json"

    name_bytes = name.encode("utf-8")[:31]
    name_field = name_bytes + b"\x00" * (32 - len(name_bytes))
    raw_identity = bytes.fromhex(public_key) + bytes.fromhex(private_key) + name_field
    if len(raw_identity) != 128:
        raise InstallError(f"Generated _main.id has invalid size: {len(raw_identity)}.")

    if dry_run:
        print(f"  prepare {identity_path.relative_to(root)} (128 bytes)")
        print(f"  prepare {prefs_path.relative_to(root)}")
        return

    identity_path.parent.mkdir(parents=True, exist_ok=True)
    identity_path.write_bytes(raw_identity)
    prefs_path.parent.mkdir(parents=True, exist_ok=True)
    prefs_path.write_text(
        json.dumps({"name": name}, separators=(",", ":")) + "\n",
        encoding="utf-8",
        newline="\n",
    )


def remove_path(path: Path) -> None:
    if path.is_dir():
        shutil.rmtree(path)
    elif path.exists():
        path.unlink()


def copy_path(source: Path, destination: Path) -> None:
    if source.is_dir():
        shutil.copytree(source, destination)
    else:
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)


def payload_pairs(source_root: Path, root: Path) -> list[tuple[Path, Path]]:
    return [
        (source_root / "lib/MultiObserver", root / "lib/MultiObserver"),
        (
            source_root / "examples/simple_repeater/MOBridge.h",
            root / "examples/simple_repeater/MOBridge.h",
        ),
        (
            source_root / "examples/simple_repeater/MOBridge.cpp",
            root / "examples/simple_repeater/MOBridge.cpp",
        ),
        (source_root / "installer/multiobserver_cpp17.py", root / CPP17_SCRIPT),
    ]


def trees_equal(source: Path, destination: Path) -> bool:
    if source.is_file():
        return destination.is_file() and source.read_bytes() == destination.read_bytes()
    if not source.is_dir() or not destination.is_dir():
        return False

    source_files = {
        path.relative_to(source)
        for path in source.rglob("*")
        if path.is_file()
    }
    destination_files = {
        path.relative_to(destination)
        for path in destination.rglob("*")
        if path.is_file()
    }
    if source_files != destination_files:
        return False
    return all(
        (source / rel).read_bytes() == (destination / rel).read_bytes()
        for rel in source_files
    )


def payload_is_current(source_root: Path, root: Path) -> bool:
    return all(
        trees_equal(source, destination)
        for source, destination in payload_pairs(source_root, root)
    )


def create_backup(root: Path, include_data: bool) -> Path:
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    backup = root / ".multiobserver-backup" / stamp
    backup.mkdir(parents=True, exist_ok=False)

    for rel in MESH_FILES + CONFIG_FILES:
        src = root / rel
        dst = backup / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)

    optional_paths = list(PAYLOAD_PATHS)
    if include_data:
        optional_paths.append(Path("data"))

    state: dict[str, bool] = {}
    for rel in optional_paths:
        src = root / rel
        exists = src.exists()
        state[rel.as_posix()] = exists
        if exists:
            copy_path(src, backup / rel)

    (backup / BACKUP_STATE_FILE).write_text(
        json.dumps({"optional_paths": state}, indent=2) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    return backup


def restore_backup(root: Path, backup: Path) -> None:
    if not backup.is_dir():
        raise InstallError(f"Backup directory not found: {backup}")

    for rel in MESH_FILES + CONFIG_FILES:
        src = backup / rel
        dst = root / rel
        if not src.is_file():
            raise InstallError(f"Backup is incomplete: {src}")
        shutil.copy2(src, dst)

    state_path = backup / BACKUP_STATE_FILE
    if not state_path.is_file():
        # Backups created by older installer revisions contain only the core
        # integration files. Preserve their legacy rollback behavior.
        return

    try:
        state = json.loads(state_path.read_text(encoding="utf-8"))
        optional = state["optional_paths"]
    except Exception as exc:
        raise InstallError(f"Invalid backup state file: {state_path}") from exc

    allowed = {rel.as_posix() for rel in PAYLOAD_PATHS + [Path("data")]}
    if not isinstance(optional, dict) or not set(optional).issubset(allowed):
        raise InstallError(f"Backup state contains unexpected paths: {state_path}")

    for rel_text, existed_before in optional.items():
        rel = Path(rel_text)
        destination = root / rel
        remove_path(destination)
        if existed_before:
            source = backup / rel
            if not source.exists():
                raise InstallError(f"Backup is incomplete: {source}")
            copy_path(source, destination)


def install_cpp17_script(source_root: Path, root: Path, dry_run: bool) -> None:
    source = source_root / "installer" / "multiobserver_cpp17.py"
    destination = root / CPP17_SCRIPT

    if not source.is_file():
        raise InstallError(f"Missing installer payload: {source}")

    if dry_run:
        print(f"  copy   {CPP17_SCRIPT}")
        return

    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def copy_mo(source_root: Path, root: Path) -> None:
    source_lib = source_root / "lib" / "MultiObserver"
    destination_lib = root / "lib" / "MultiObserver"

    if destination_lib.exists():
        shutil.rmtree(destination_lib)
    shutil.copytree(source_lib, destination_lib)

    for name in ("MOBridge.h", "MOBridge.cpp"):
        shutil.copy2(
            source_root / "examples" / "simple_repeater" / name,
            root / "examples" / "simple_repeater" / name,
        )


def build_plan(root: Path, source_root: Path) -> tuple[str, dict[Path, str]]:
    version = validate_meshcore(root)
    validate_source(source_root)
    validate_platformio(root)

    patched: dict[Path, str] = {}

    main = read_text(root / MESH_FILES[0])
    mymesh_h = read_text(root / MESH_FILES[1])
    mymesh_cpp = read_text(root / MESH_FILES[2])
    ui_cpp = read_text(root / MESH_FILES[3])

    installed_markers = (
        MARKERS["main"] in main,
        MARKERS["mymesh_h"] in mymesh_h,
        MARKERS["mymesh_cpp"] in mymesh_cpp,
    )
    if any(installed_markers) and not all(installed_markers):
        raise InstallError("Partial MultiObserver integration detected; use rollback before reinstalling.")

    if not all(installed_markers):
        main = patch_main(main)
        mymesh_h = patch_mymesh_h(mymesh_h)
        mymesh_cpp = patch_mymesh_cpp(mymesh_cpp)

    mymesh_cpp = patch_status_sampling(mymesh_cpp)
    mymesh_cpp = patch_rx_metadata(mymesh_cpp)
    main = patch_tx_led_main(main)
    mymesh_cpp = patch_tx_led_mymesh(mymesh_cpp)
    ui_cpp = patch_ui_wifi_ip(ui_cpp)

    current_sources = {
        MESH_FILES[0]: read_text(root / MESH_FILES[0]),
        MESH_FILES[1]: read_text(root / MESH_FILES[1]),
        MESH_FILES[2]: read_text(root / MESH_FILES[2]),
        MESH_FILES[3]: read_text(root / MESH_FILES[3]),
    }
    desired_sources = {
        MESH_FILES[0]: main,
        MESH_FILES[1]: mymesh_h,
        MESH_FILES[2]: mymesh_cpp,
        MESH_FILES[3]: ui_cpp,
    }
    for rel, desired in desired_sources.items():
        if desired != current_sources[rel]:
            patched[rel] = desired

    platformio_path = root / CONFIG_FILES[0]
    platformio = read_text(platformio_path)
    patched_platformio = patch_platformio(platformio)
    # Validate the resulting configuration, not the clean upstream file.
    # The certificate-bundle flag is intentionally introduced by patch_platformio.
    if "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=1" not in patched_platformio:
        raise InstallError(
            f"{PLATFORMIO_SECTION} patch did not enable CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=1."
        )
    if patched_platformio != platformio:
        patched[CONFIG_FILES[0]] = patched_platformio

    return version, patched


def main() -> int:
    args = parse_args()
    root = args.meshcore_root.resolve()
    source_root = args.source_root.resolve()
    identity = validate_identity_args(args)

    if args.rollback:
        restore_backup(root, args.rollback.resolve())
        print(f"Rollback completed from: {args.rollback.resolve()}")
        return 0

    if not root.is_dir():
        raise InstallError(f"MeshCore root does not exist: {root}")

    version, patched = build_plan(root, source_root)
    payload_current = payload_is_current(source_root, root)

    print(f"MeshCore compatibility: {SUPPORTED[version]}")
    print(f"MeshCore root: {root}")
    print(f"MultiObserver source: {source_root}")

    if not patched and payload_current and identity is None:
        print("MultiObserver integration is already installed.")
        print("MultiObserver payload is current; no files will be modified.")
        return 0

    if patched:
        print("Planned MeshCore changes:")
        for rel in MESH_FILES + CONFIG_FILES:
            if rel in patched:
                print(f"  patch  {rel}")
    print("Planned MultiObserver files:")
    print("  copy   lib/MultiObserver/")
    print("  copy   examples/simple_repeater/MOBridge.h")
    print("  copy   examples/simple_repeater/MOBridge.cpp")
    print(f"  copy   {CPP17_SCRIPT}")
    if CONFIG_FILES[0] in patched:
        print(
            f"  patch  {CONFIG_FILES[0]} "
            f"(MultiObserver + WiFiClientSecure exclusion + C++17 for {PLATFORMIO_SECTION})"
        )

    if identity is not None:
        print("Prepared first-boot identity:")
        print(f"  repeater name: {identity[0]}")
        print("  public key: configured")
        print("  private key: configured")
        prepare_filesystem(root, *identity, dry_run=True)

    if args.dry_run:
        print("DRY-RUN: PASS")
        return 0

    backup = create_backup(root, include_data=identity is not None)
    try:
        copy_mo(source_root, root)
        install_cpp17_script(source_root, root, dry_run=False)
        for rel, text in patched.items():
            write_text(root / rel, text)
        if identity is not None:
            prepare_filesystem(root, *identity, dry_run=False)
            print(f"Prepared first-boot filesystem: {root / 'data'}")
        print(f"Installation completed. Backup: {backup}")
    except Exception:
        try:
            restore_backup(root, backup)
        except Exception as rollback_error:
            raise InstallError(
                f"Installation failed and automatic rollback also failed: {rollback_error}"
            )
        raise InstallError("Installation failed. MeshCore changes were rolled back.")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except InstallError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(2)
