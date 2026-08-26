from __future__ import annotations

import argparse
import unittest

from installer.mo_installer.common import InstallError
from installer.mo_installer.storage import validate_identity_args


class IdentityValidationTests(unittest.TestCase):
    def test_accepts_complete_valid_identity(self) -> None:
        args = argparse.Namespace(
            repeater_name="Test repeater",
            public_key="A" * 64,
            private_key="B" * 128,
        )
        self.assertEqual(
            validate_identity_args(args),
            ("Test repeater", "A" * 64, "B" * 128),
        )

    def test_rejects_incomplete_identity(self) -> None:
        args = argparse.Namespace(
            repeater_name="Test repeater",
            public_key="A" * 64,
            private_key=None,
        )
        with self.assertRaises(InstallError):
            validate_identity_args(args)


if __name__ == "__main__":
    unittest.main()
