from __future__ import annotations

import argparse
import datetime as dt
import json
import shutil
from pathlib import Path

from .common import (
    BACKUP_STATE_FILE,
    CONFIG_FILES,
    CPP17_SCRIPT,
    MESH_FILES,
    PAYLOAD_PATHS,
    InstallError,
)

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

def install_cpp17_script(source_root: Path, root: Path) -> None:
    source = source_root / "installer" / "multiobserver_cpp17.py"
    destination = root / CPP17_SCRIPT

    if not source.is_file():
        raise InstallError(f"Missing installer payload: {source}")

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
