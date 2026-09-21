#!/usr/bin/env python3
"""Focused checks for the intentionally lossy ASS dialogue preset."""

from pathlib import Path
import sys
import tempfile
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src" / "desktop"))
from subtitle_formats.ass_to_vita_dialogue_srt import parse_ass, write_srt  # noqa: E402


class DialogueExtractionTests(unittest.TestCase):
    def test_drops_signs_and_songs_but_keeps_positioned_dialogue(self):
        ass = """[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 3,0:00:01.00,0:00:02.00,Main,A,0,0,0,,Hello, world!
Dialogue: 2,0:00:02.00,0:00:03.00,Signs,Sign,0,0,0,,A sign
Dialogue: 2,0:00:03.00,0:00:04.00,OP_TL_E,,0,0,0,,A lyric
Dialogue: 3,0:00:04.00,0:00:05.00,Main,,0,0,0,,{\\an8}Top text
Dialogue: 3,0:00:05.00,0:00:06.00,Italics,,0,0,0,,{\\pos(100,100)}Background
Dialogue: 3,0:00:06.00,0:00:07.00,Flashback,,0,0,0,,Spoken\\Ndialogue
"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sample.ass"
            path.write_text(ass, encoding="utf-8")
            cues, counts = parse_ass(path)
        self.assertEqual([cue.text for cue in cues],
                         ["Hello, world!", "Top text", "Background", "Spoken\ndialogue"])
        self.assertEqual(counts["style"], 2)
        self.assertEqual(counts["karaoke"], 0)

    def test_srt_preserves_overlapping_dialogue_but_not_top_tag(self):
        ass = """[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 3,0:00:01.00,0:00:04.00,Main,,0,0,0,,Main line
Dialogue: 3,0:00:02.00,0:00:03.00,Italics,,0,0,0,,{\\an8}Background line
"""
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "sample.ass"
            target = Path(directory) / "sample.srt"
            source.write_text(ass, encoding="utf-8")
            cues, _ = parse_ass(source)
            write_srt(cues, target)
            text = target.read_text(encoding="utf-8")
        self.assertIn("00:00:01,000 --> 00:00:04,000", text)
        self.assertIn("00:00:02,000 --> 00:00:03,000", text)
        self.assertIn("Background line", text)
        self.assertNotIn(r"{\an8}", text)


if __name__ == "__main__":
    unittest.main()
