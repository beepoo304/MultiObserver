from __future__ import annotations

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
    "MO.h", "MO.cpp", "MOCli.h", "MOCli.cpp", "MOConfig.h", "MOConfig.cpp",
    "MOMQTT.h", "MOMQTT.cpp", "MOMQTTPrefs.h", "MOMQTTPrefs.cpp",
    "MOWifi.h", "MOWifi.cpp", "MOWifiPrefs.h", "MOWifiPrefs.cpp",
]

MESH_FILES = [
    Path("examples/simple_repeater/main.cpp"),
    Path("examples/simple_repeater/MyMesh.h"),
    Path("examples/simple_repeater/MyMesh.cpp"),
    Path("examples/simple_repeater/UITask.cpp"),
]
CONFIG_FILES = [Path("variants/heltec_v3/platformio.ini")]

PLATFORMIO_SECTION = "[env:Heltec_v3_repeater]"
MO_LIBRARY_DEP = "file://lib/MultiObserver"
WIFI_CLIENT_SECURE_IGNORE = "WiFiClientSecure"
CERT_BUNDLE_FLAG = "-D CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=1"
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


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except Exception as exc:
        raise InstallError(f"Cannot read UTF-8 file: {path}") from exc

def write_text(path: Path, text: str) -> None:
    path.write_text(text, encoding="utf-8", newline="\n")
