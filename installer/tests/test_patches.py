from __future__ import annotations

import unittest

from installer.mo_installer.common import InstallError
from installer.mo_installer.patches import patch_platformio


TARGET = "[env:Heltec_v3_repeater]"


def platformio_fixture(newline: str = "\n") -> str:
    lines = [
        "[env:base]",
        "build_flags =",
        "  -DBASE=1",
        "",
        TARGET,
        "lib_deps =",
        "  ArduinoJson",
        "  WiFi",
        "build_flags =",
        "  ${esp32_base.build_flags}",
        "",
        "[env:after]",
        "build_flags =",
        "  -DAFTER=1",
        "",
    ]
    return newline.join(lines)


class PlatformioPatchTests(unittest.TestCase):
    def test_adds_only_required_target_settings(self) -> None:
        source = platformio_fixture()
        patched = patch_platformio(source)

        before, target_and_after = patched.split(TARGET, 1)
        target, after = target_and_after.split("[env:after]", 1)
        self.assertEqual(before, source.split(TARGET, 1)[0])
        self.assertEqual(after, source.split("[env:after]", 1)[1])
        self.assertNotIn("\n  WiFi\n", target)
        for item in (
            "file://lib/MultiObserver",
            "WiFiClientSecure",
            "-std=gnu++11",
            "-std=gnu++17",
            "pre:scripts/multiobserver_cpp17.py",
            "-D CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=1",
        ):
            self.assertEqual(target.count(item), 1, item)

    def test_is_idempotent(self) -> None:
        once = patch_platformio(platformio_fixture())
        self.assertEqual(patch_platformio(once), once)

    def test_preserves_crlf(self) -> None:
        patched = patch_platformio(platformio_fixture("\r\n"))
        self.assertNotIn("\n", patched.replace("\r\n", ""))

    def test_rejects_unknown_block_layout(self) -> None:
        malformed = platformio_fixture().replace(
            "lib_deps =\n  ArduinoJson\n  WiFi\n", "lib_deps = ArduinoJson\n"
        )
        with self.assertRaises(InstallError):
            patch_platformio(malformed)


if __name__ == "__main__":
    unittest.main()
