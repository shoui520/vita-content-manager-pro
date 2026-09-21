#!/usr/bin/env python3
"""Checks for Vita M4T cue geometry derived from Sony's sample."""

from pathlib import Path
import sys
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src" / "desktop"))
from subtitle_formats.srt_to_vita_ttml import TTS, TT, XML, build_ttml, caption_lines  # noqa: E402


class VitaTtmlGeometryTests(unittest.TestCase):
    def test_single_line_is_centered_by_origin(self):
        root = build_ttml([("00:00:01.000", "00:00:02.000", "Hello", False)]).getroot()
        p = next(root.iter(f"{{{TT}}}p"))
        self.assertEqual(p.get(f"{{{TTS}}}extent"), "11.710% 5.0%")
        self.assertEqual(p.get(f"{{{TTS}}}origin"), "44.145% 82.5%")
        self.assertEqual(p.get(f"{{{TTS}}}textAlign"), "left")

    def test_multiline_stays_separate_and_centered(self):
        root = build_ttml([("00:00:01.000", "00:00:02.000", "Hello\nworld!", False)]).getroot()
        lines = list(root.iter(f"{{{TT}}}p"))
        self.assertEqual([p.text for p in lines], ["Hello", "world!"])
        self.assertEqual([p.get(f"{{{TTS}}}origin") for p in lines],
                         ["44.145% 77.5%", "42.974% 82.5%"])

    def test_long_text_wraps_into_safe_width(self):
        self.assertEqual(caption_lines("A" * 33), ["A" * 32, "A"])

    def test_japanese_language_and_full_width_wrapping(self):
        root = build_ttml([("00:00:01.000", "00:00:02.000", "あ" * 17, False)],
                          "ja").getroot()
        self.assertEqual(root.get(f"{{{XML}}}lang"), "ja")
        lines = list(root.iter(f"{{{TT}}}p"))
        self.assertEqual([p.text for p in lines], ["あ" * 16, "あ"])
        self.assertEqual(lines[0].get(f"{{{TTS}}}extent"), "74.944% 5.0%")


if __name__ == "__main__":
    unittest.main()
