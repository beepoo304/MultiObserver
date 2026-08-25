#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


SUPPORTED = {"1.15", "1.16", "1.17"}
MAIN_MARKER = "MULTIOBSERVER: lifecycle integration v2"
H_MARKER = "MULTIOBSERVER: bridge declaration v2"
CPP_MARKER = "MULTIOBSERVER: mesh hooks v2"

MO_FILES = [
    "MO.h", "MO.cpp",
    "MOCli.h", "MOCli.cpp",
    "MOConfig.h", "MOConfig.cpp",
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
        root / "examples/simple_repeater/MOBridge.h",
        root / "examples/simple_repeater/MOBridge.cpp",
    ] + [
        root / "lib/MultiObserver/src" / name for name in MO_FILES
    ]

    missing = [str(p) for p in required if not p.is_file()]
    if missing:
        raise VerifyError("Missing installed files:\n  " + "\n  ".join(missing))

    v = version(root)
    main_cpp = read(root / "examples/simple_repeater/main.cpp")
    mymesh_h = read(root / "examples/simple_repeater/MyMesh.h")
    mymesh_cpp = read(root / "examples/simple_repeater/MyMesh.cpp")

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
        "RX hook": mymesh_cpp.count("mo_bridge.onRx(pkt, len, score") == 1,
        "TX hook": mymesh_cpp.count("mo_bridge.onTx(pkt, len, static_cast<int>(radio_driver.getLastRSSI()),") == 1,
        "status hook": mymesh_cpp.count("mo_bridge.setStatusSnapshot(mo_status);") == 1,
        "CLI hook": mymesh_cpp.count(
            "if (mo_bridge.handleCommand(sender_timestamp, command, reply))"
        ) == 1,
        "MOMQTT status": "publishStatus" in (root / "lib/MultiObserver/src/MOMQTT.cpp").read_text(encoding="utf-8"),
        "MOMQTT LWT": "last_will" in (root / "lib/MultiObserver/src/MOMQTT.cpp").read_text(encoding="utf-8"),
        "RAW publish": "mqtt_.publishRaw(raw)" in (root / "lib/MultiObserver/src/MO.cpp").read_text(encoding="utf-8"),
        "startup logs": "[MO] MQTT1 start" in (root / "lib/MultiObserver/src/MOMQTT.cpp").read_text(encoding="utf-8"),
        "no literal newline escape": "\\n" not in (root / "lib/MultiObserver/src/MOMQTT.h").read_text(encoding="utf-8"),
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
