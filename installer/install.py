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
    ]
    missing = [str(p) for p in required if not p.is_file()]
    if missing:
        raise InstallError("Missing MultiObserver source files:\n  " + "\n  ".join(missing))


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
        + "  mo_bridge.onRx(pkt, len, score, static_cast<int>(_radio->getLastRSSI()), -1);",
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

  MOMQTT::StatusData mo_status{
      .status = "online",
      .timestamp = {},
      .origin = getNodeName(),
      .originId = {},
      .model = board.getManufacturerName(),
      .firmwareVersion = FIRMWARE_VERSION,
      .radio = {},
      .clientVersion = "MultiObserver",
      .repeat = _prefs.disable_fwd ? "off" : "on",
      .batteryMv = board.getBattMilliVolts(),
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
  static unsigned long mo_status_last_ms = 0;
  if (mo_status_last_ms == 0 || now - mo_status_last_ms >= 5000) {
    mo_status_last_ms = now;
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


def prepare_filesystem(source_root: Path, name: str, public_key: str,
                       private_key: str, dry_run: bool) -> None:
    data_root = source_root / "installer" / "prepared_fs"
    identity_path = data_root / "identity" / "_main.id"
    prefs_path = data_root / "prefs.json"

    name_bytes = name.encode("utf-8")[:31]
    name_field = name_bytes + b"\x00" * (32 - len(name_bytes))
    raw_identity = bytes.fromhex(public_key) + bytes.fromhex(private_key) + name_field
    if len(raw_identity) != 128:
        raise InstallError(f"Generated _main.id has invalid size: {len(raw_identity)}.")

    if dry_run:
        print(f"  prepare {identity_path.relative_to(source_root)} (128 bytes)")
        print(f"  prepare {prefs_path.relative_to(source_root)}")
        return

    identity_path.parent.mkdir(parents=True, exist_ok=True)
    identity_path.write_bytes(raw_identity)
    prefs_path.write_text(
        json.dumps({"name": name}, separators=(",", ":")) + "\n",
        encoding="utf-8",
        newline="\n",
    )


def create_backup(root: Path) -> Path:
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    backup = root / ".multiobserver-backup" / stamp
    backup.mkdir(parents=True, exist_ok=False)

    for rel in MESH_FILES:
        src = root / rel
        dst = backup / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)

    return backup


def restore_backup(root: Path, backup: Path) -> None:
    if not backup.is_dir():
        raise InstallError(f"Backup directory not found: {backup}")

    for rel in MESH_FILES:
        src = backup / rel
        dst = root / rel
        if not src.is_file():
            raise InstallError(f"Backup is incomplete: {src}")
        shutil.copy2(src, dst)


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

    main = read_text(root / MESH_FILES[0])
    mymesh_h = read_text(root / MESH_FILES[1])
    mymesh_cpp = read_text(root / MESH_FILES[2])

    if MARKERS["main"] in main:
        return version, {}

    patched = {
        MESH_FILES[0]: patch_main(main),
        MESH_FILES[1]: patch_mymesh_h(mymesh_h),
        MESH_FILES[2]: patch_mymesh_cpp(mymesh_cpp),
    }
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

    print(f"MeshCore compatibility: {SUPPORTED[version]}")
    print(f"MeshCore root: {root}")
    print(f"MultiObserver source: {source_root}")

    if not patched:
        print("MultiObserver integration is already installed.")
        print("No MeshCore files will be modified.")
        if identity is not None:
            print("Preparing first-boot identity only.")
            prepare_filesystem(source_root, *identity, dry_run=args.dry_run)
            if not args.dry_run:
                prepared = source_root / "installer" / "prepared_fs"
                destination = root / "data"
                if destination.exists():
                    shutil.rmtree(destination)
                shutil.copytree(prepared, destination)
                print(f"Prepared first-boot filesystem: {destination}")
        return 0

    print("Planned MeshCore changes:")
    for rel in MESH_FILES:
        print(f"  patch  {rel}")
    print("Planned MultiObserver files:")
    print("  copy   lib/MultiObserver/")
    print("  copy   examples/simple_repeater/MOBridge.h")
    print("  copy   examples/simple_repeater/MOBridge.cpp")

    if identity is not None:
        print("Prepared first-boot identity:")
        print(f"  repeater name: {identity[0]}")
        print("  public key: configured")
        print("  private key: configured")
        prepare_filesystem(source_root, *identity, dry_run=args.dry_run)

    if args.dry_run:
        print("DRY-RUN: PASS")
        return 0

    backup = create_backup(root)
    try:
        copy_mo(source_root, root)
        for rel, text in patched.items():
            write_text(root / rel, text)
        if identity is not None:
            prepared = source_root / "installer" / "prepared_fs"
            destination = root / "data"
            if destination.exists():
                shutil.rmtree(destination)
            shutil.copytree(prepared, destination)
            print(f"Prepared first-boot filesystem: {destination}")
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
