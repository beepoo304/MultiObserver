from __future__ import annotations

import re
from pathlib import Path

from .common import (
    CONFIG_FILES,
    LIBRARY_MANIFEST,
    MARKERS,
    MESH_FILES,
    MO_FILES,
    PLATFORMIO_SECTION,
    SUPPORTED,
    InstallError,
    read_text,
)
from .patches import require_exactly_one

def detect_version(root: Path) -> str:
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

def validate_meshcore(root: Path) -> str:
    paths = [root / p for p in MESH_FILES]
    missing = [str(p) for p in paths if not p.is_file()]
    if missing:
        raise InstallError("Missing simple_repeater files:\n  " + "\n  ".join(missing))

    mymesh_h = read_text(root / MESH_FILES[1])
    mymesh_cpp = read_text(root / MESH_FILES[2])
    main_cpp = read_text(root / MESH_FILES[0])

    version = detect_version(root)

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
