from __future__ import annotations

import argparse
import sys
from pathlib import Path

from .common import (
    CONFIG_FILES,
    CPP17_SCRIPT,
    MESH_FILES,
    PLATFORMIO_SECTION,
    SUPPORTED,
    InstallError,
    write_text,
)
from .storage import (
    copy_mo,
    create_backup,
    install_cpp17_script,
    payload_is_current,
    prepare_filesystem,
    restore_backup,
    validate_identity_args,
)
from .workflow import build_plan

def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Install MultiObserver into a MeshCore simple_repeater tree."
    )
    p.add_argument("meshcore_root", type=Path)
    p.add_argument(
        "--source-root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="MultiObserver repository root.",
    )
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--repeater-name", help="Repeater name written to /prefs.json on first filesystem upload.")
    p.add_argument("--public-key", help="Observer public key, 64 hexadecimal characters.")
    p.add_argument("--private-key", help="Observer private key, 128 hexadecimal characters.")
    p.add_argument(
        "--rollback",
        type=Path,
        metavar="BACKUP_DIR",
        help="Restore files from a backup created by this installer.",
    )
    return p.parse_args()

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
        install_cpp17_script(source_root, root)
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


def entrypoint() -> None:
    try:
        raise SystemExit(main())
    except InstallError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(2) from exc
