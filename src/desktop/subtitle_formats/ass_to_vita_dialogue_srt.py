#!/usr/bin/env python3
"""Extract one clean dialogue stream from an ASS subtitle file.

This is intentionally lossy: typeset signs and lyrics/karaoke are omitted.
Dialogue is kept even when ASS positions it at the top. Simple top alignment
and other positioning are discarded: all output dialogue uses the Vita's
working bottom caption region.
"""

import argparse
from dataclasses import dataclass
from pathlib import Path
import re


DIALOGUE_STYLES = {"Main", "Italics", "Flashback", "Flashback_Italics", "Preview_Main"}
STYLE_PRIORITY = {"Main": 3, "Preview_Main": 3, "Italics": 2,
                  "Flashback": 2, "Flashback_Italics": 2}
OVERRIDE = re.compile(r"\{[^{}]*\}")
KARAOKE = re.compile(r"\\(?:k|kf|ko)\s*\d", re.I)


@dataclass
class Cue:
    start: int
    end: int
    text: str
    style: str


def ass_time(value):
    match = re.fullmatch(r"(\d+):(\d{2}):(\d{2})\.(\d{2})", value.strip())
    if not match:
        raise ValueError(f"bad ASS timestamp: {value!r}")
    hours, minutes, seconds, centiseconds = map(int, match.groups())
    return ((hours * 60 + minutes) * 60 + seconds) * 1000 + centiseconds * 10


def srt_time(value):
    hours, rem = divmod(value, 3600000)
    minutes, rem = divmod(rem, 60000)
    seconds, millis = divmod(rem, 1000)
    return f"{hours:02}:{minutes:02}:{seconds:02},{millis:03}"


def parse_ass(path, *, generic=False, dialogue_only=True):
    cues = []
    counts = {"events": 0, "style": 0, "karaoke": 0, "empty": 0}
    in_events = False
    fields = None
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        if line.startswith("["):
            in_events = line.strip().lower() == "[events]"
            continue
        if not in_events:
            continue
        if line.startswith("Format:"):
            fields = [field.strip().lower() for field in line.partition(":")[2].split(",")]
            continue
        if not line.startswith("Dialogue:"):
            continue
        counts["events"] += 1
        if fields is None or not {"start", "end", "style", "text"} <= set(fields):
            raise ValueError("ASS Events section lacks the required Format fields")
        values = [part.strip() for part in line.partition(":")[2].split(",", len(fields) - 1)]
        if len(values) != len(fields):
            raise ValueError("incomplete ASS Dialogue event")
        event = dict(zip(fields, values))
        style = event["style"]
        excluded = re.search(r"(?:sign|lyric|karaoke|song|title|logo|credit)|(?:^|[_\W])(?:op|ed)(?:$|[_\W])", style, re.I)
        if dialogue_only and (bool(excluded) if generic else style not in DIALOGUE_STYLES):
            counts["style"] += 1
            continue
        raw = event["text"]
        if (dialogue_only and KARAOKE.search(raw)) or re.search(r"\\p\s*[1-9]", raw):
            counts["karaoke"] += 1
            continue
        text = OVERRIDE.sub("", raw).replace(r"\N", "\n").replace(r"\n", "\n")
        text = text.replace(r"\h", " ")
        text = "\n".join(" ".join(part.split()) for part in text.split("\n")).strip()
        start, end = ass_time(event["start"]), ass_time(event["end"])
        if not text or end <= start:
            counts["empty"] += 1
            continue
        cues.append(Cue(start, end, text, style))
    return cues, counts


def write_srt(cues, path):
    with path.open("w", encoding="utf-8", newline="\n") as output:
        for number, cue in enumerate(cues, 1):
            output.write(f"{number}\n{srt_time(cue.start)} --> {srt_time(cue.end)}\n"
                         f"{cue.text}\n\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input_ass", type=Path)
    parser.add_argument("output_srt", type=Path)
    args = parser.parse_args()
    cues, counts = parse_ass(args.input_ass)
    if not cues:
        parser.error("no dialogue cues found")
    clean = sorted(cues, key=lambda cue: (cue.start, -STYLE_PRIORITY[cue.style], -cue.end))
    write_srt(clean, args.output_srt)
    print(f"{args.output_srt}: {len(clean)} dialogue cues; skipped "
          f"{counts['style']} non-dialogue styles, {counts['karaoke']} "
          f"karaoke cues, {counts['empty']} empty cues")


if __name__ == "__main__":
    main()
