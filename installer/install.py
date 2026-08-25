#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import shutil
import sys
from pathlib import Path


PATCH_MARKER = "MULTIOBSERVER: lifecycle integration v1"
SUPPORTED_MAJOR_MINOR = {"1.15", "1.16", "1.17"}


class InstallError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Install MultiObserver lifecycle integration into MeshCore simple_repeater."
    )
    parser.add_argument(
        "meshcore_root",
        type=Path,
        help="Path to the target MeshCore repository.",
    )
    parser.add_argument(
        "--source-root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="Path to the MultiObserver repository (default: repository root).",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Validate and print the planned changes without modifying files.",
    )
    return parser.parse_args()


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        raise InstallError(f"Cannot read UTF-8 source file: {path}") from exc


def detect_version(meshcore_root: Path, main_text: str) -> str:
    # MeshCore 1.15.x, 1.16.x and 1.17.x do not need version-specific
    # patch logic here. The installer deliberately fingerprints the lifecycle
    # structure instead of trusting a version string that may not be present
    # in a source archive or checkout.
    return "structurally-compatible"


def validate_layout(meshcore_root: Path) -> tuple[Path, Path]:
    main_cpp = meshcore_root / "examples" / "simple_repeater" / "main.cpp"
    if not main_cpp.is_file():
        raise InstallError(f"MeshCore simple_repeater main.cpp not found: {main_cpp}")

    mymesh_h = meshcore_root / "examples" / "simple_repeater" / "MyMesh.h"
    if not mymesh_h.is_file():
        raise InstallError(f"MeshCore simple_repeater MyMesh.h not found: {mymesh_h}")

    return main_cpp, mymesh_h


def build_patch(text: str) -> str:
    if PATCH_MARKER in text:
        return text

    include_anchor = '#include "MyMesh.h"'
    if include_anchor not in text:
        raise InstallError('Cannot find #include "MyMesh.h" in simple_repeater/main.cpp')

    instance_anchor = re.search(
        r'^(MyMesh\s+the_mesh\s*\([^;]+;\s*)$',
        text,
        re.MULTILINE,
    )
    if not instance_anchor:
        raise InstallError("Cannot find MyMesh the_mesh instance in simple_repeater/main.cpp")

    begin_anchor = "  the_mesh.begin(fs);"
    if begin_anchor not in text:
        raise InstallError("Cannot find the_mesh.begin(fs) lifecycle anchor")

    loop_anchor = "  the_mesh.loop();"
    if loop_anchor not in text:
        raise InstallError("Cannot find the_mesh.loop() lifecycle anchor")

    patched = text.replace(
        include_anchor,
        f'{include_anchor}\n#include "MOBridge.h"\n// {PATCH_MARKER}',
        1,
    )

    instance_match = re.search(
        r'^(MyMesh\s+the_mesh\s*\([^;]+;\s*)$',
        patched,
        re.MULTILINE,
    )
    assert instance_match is not None

    instance_line = instance_match.group(1)
    replacement = f"{instance_line}\nMOBridge mo_bridge;"
    patched = patched.replace(instance_line, replacement, 1)

    patched = patched.replace(
        begin_anchor,
        f"{begin_anchor}\n  mo_bridge.begin();",
        1,
    )
    patched = patched.replace(
        loop_anchor,
        f"{loop_anchor}\n  mo_bridge.loop();",
        1,
    )

    return patched


def copy_tree(source_root: Path, meshcore_root: Path, dry_run: bool) -> list[Path]:
    source_lib = source_root / "lib" / "MultiObserver"
    source_bridge = source_root / "examples" / "simple_repeater" / "MOBridge.h"
    source_bridge_cpp = source_root / "examples" / "simple_repeater" / "MOBridge.cpp"

    required = [
        source_lib / "src" / "MO.h",
        source_lib / "src" / "MO.cpp",
        source_bridge,
        source_bridge_cpp,
    ]

    for path in required:
        if not path.is_file():
            raise InstallError(f"Required MultiObserver file is missing: {path}")

    destinations = [
        meshcore_root / "lib" / "MultiObserver",
        meshcore_root / "examples" / "simple_repeater" / "MOBridge.h",
        meshcore_root / "examples" / "simple_repeater" / "MOBridge.cpp",
    ]

    if dry_run:
        return destinations

    lib_destination = meshcore_root / "lib" / "MultiObserver"
    if lib_destination.exists():
        shutil.rmtree(lib_destination)
    shutil.copytree(source_lib, lib_destination)

    shutil.copy2(
        source_bridge,
        meshcore_root / "examples" / "simple_repeater" / "MOBridge.h",
    )
    shutil.copy2(
        source_bridge_cpp,
        meshcore_root / "examples" / "simple_repeater" / "MOBridge.cpp",
    )

    return destinations


def main() -> int:
    args = parse_args()
    meshcore_root = args.meshcore_root.resolve()
    source_root = args.source_root.resolve()

    main_cpp, _ = validate_layout(meshcore_root)
    original = read_text(main_cpp)
    version = detect_version(meshcore_root, original)


    patched = build_patch(original)

    if args.dry_run:
        print(f"MeshCore compatibility: {version}")
        print(f"Target: {meshcore_root}")
        print("Dry-run: PASS")
        print("Would install:")
        for destination in copy_tree(source_root, meshcore_root, True):
            print(f"  {destination}")
        if patched != original:
            print(f"Would patch: {main_cpp}")
            print("  + #include \"MOBridge.h\"")
            print("  + MOBridge mo_bridge;")
            print("  + mo_bridge.begin();")
            print("  + mo_bridge.loop();")
        else:
            print("main.cpp already contains MultiObserver lifecycle integration.")
        return 0

    backup = main_cpp.with_suffix(".cpp.mo-preinstall")
    if patched != original and not backup.exists():
        shutil.copy2(main_cpp, backup)

    copy_tree(source_root, meshcore_root, False)

    if patched != original:
        main_cpp.write_text(patched, encoding="utf-8", newline="\n")

    print(f"MeshCore compatibility: {version}")
    print("MultiObserver lifecycle integration installed.")
    print(f"Backup: {backup if backup.exists() else 'not required'}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except InstallError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(2)
