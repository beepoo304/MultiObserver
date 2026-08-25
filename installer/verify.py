#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


MARKER = "MULTIOBSERVER: lifecycle integration v1"


class VerifyError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Verify MultiObserver lifecycle integration in MeshCore."
    )
    parser.add_argument("meshcore_root", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = args.meshcore_root.resolve()

    main_cpp = root / "examples" / "simple_repeater" / "main.cpp"
    bridge_h = root / "examples" / "simple_repeater" / "MOBridge.h"
    bridge_cpp = root / "examples" / "simple_repeater" / "MOBridge.cpp"
    mo_h = root / "lib" / "MultiObserver" / "src" / "MO.h"
    mo_cpp = root / "lib" / "MultiObserver" / "src" / "MO.cpp"

    required = [main_cpp, bridge_h, bridge_cpp, mo_h, mo_cpp]
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise VerifyError("Missing installed files:\n  " + "\n  ".join(missing))

    text = main_cpp.read_text(encoding="utf-8")

    checks = {
        "marker": MARKER in text,
        "include": '#include "MOBridge.h"' in text,
        "instance": re.search(r"\bMOBridge\s+mo_bridge\s*;", text) is not None,
        "begin": "mo_bridge.begin();" in text,
        "loop": "mo_bridge.loop();" in text,
    }

    failed = [name for name, ok in checks.items() if not ok]
    if failed:
        raise VerifyError("Verification failed: " + ", ".join(failed))

    if text.count('#include "MOBridge.h"') != 1:
        raise VerifyError("MOBridge include count is not exactly 1.")

    if text.count("MOBridge mo_bridge;") != 1:
        raise VerifyError("MOBridge instance count is not exactly 1.")

    print("MultiObserver Step 01 verification: PASS")
    for name in checks:
        print(f"  {name}: OK")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except VerifyError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(2)
