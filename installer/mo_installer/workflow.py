from __future__ import annotations

from pathlib import Path

from .common import (
    CONFIG_FILES,
    MARKERS,
    MESH_FILES,
    PLATFORMIO_SECTION,
    InstallError,
    read_text,
)
from .patches import (
    patch_main,
    patch_mymesh_cpp,
    patch_mymesh_h,
    patch_platformio,
    patch_rx_metadata,
    patch_status_sampling,
    patch_tx_led_main,
    patch_tx_led_mymesh,
    patch_ui_wifi_ip,
)
from .validation import validate_meshcore, validate_platformio, validate_source

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
