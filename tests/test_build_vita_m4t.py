#!/usr/bin/env python3
"""Small ISO-BMFF subtitle-track language checks."""

from pathlib import Path
import sys
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src" / "desktop"))
from subtitle_formats.build_vita_m4t import iso639_packed  # noqa: E402


class M4tLanguageTests(unittest.TestCase):
    def test_iso639_packing(self):
        self.assertEqual(iso639_packed("eng"), 0x15C7)
        self.assertEqual(iso639_packed("jpn"), 0x2A0E)

    def test_invalid_track_language(self):
        for value in ("ja", "JPN", "123", "日本語"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                iso639_packed(value)


if __name__ == "__main__":
    unittest.main()
