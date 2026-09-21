#!/usr/bin/env python3
"""Convert an SRT stream to the SMPTE-TT-flavoured TTML in Sony M4T samples.

This produces the XML payload, not the fragmented ISO-BMFF/M4T wrapper.
"""

import argparse
from html.parser import HTMLParser
import re
from pathlib import Path
import unicodedata
import xml.etree.ElementTree as ET


TT = "http://www.w3.org/ns/ttml"
TTS = TT + "#styling"
TTP = TT + "#parameter"
SMPTE = "http://www.smpte-ra.org/schemas/2052-1/2010/smpte-tt"
M608 = SMPTE + "#cea608"
XSI = "http://www.w3.org/2001/XMLSchema-instance"
XML = "http://www.w3.org/XML/1998/namespace"
CELL_PERCENT = 2.342  # Sony sample's CEA-608-style width for one text column.
MAX_COLUMNS = 32
LINE_HEIGHT_PERCENT = 5.0
BOTTOM_LINE_Y_PERCENT = 82.5
TOP_LINE_Y_PERCENT = 12.5

for prefix, uri in (("", TT), ("tts", TTS), ("ttp", TTP),
                    ("smpte", SMPTE), ("m608", M608), ("xsi", XSI)):
    ET.register_namespace(prefix, uri)


class TextOnly(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.parts = []

    def handle_data(self, data):
        self.parts.append(data)

    def handle_starttag(self, tag, attrs):
        if tag.lower() == "br":
            self.parts.append("\n")


def read_cues(path):
    content = path.read_text(encoding="utf-8-sig").replace("\r\n", "\n")
    cues = []
    for block in re.split(r"\n\s*\n", content):
        lines = block.splitlines()
        if lines and lines[0].strip().isdigit():
            lines = lines[1:]
        if not lines or " --> " not in lines[0]:
            continue
        start, end = lines[0].split(" --> ", 1)
        start = start.strip().replace(",", ".")
        end = end.split()[0].replace(",", ".")
        raw = "\n".join(lines[1:])
        top = "\\an8" in raw or "\\an7" in raw or "\\an9" in raw
        raw = re.sub(r"\{\\[^}]*\}", "", raw)
        parser = TextOnly()
        parser.feed(raw)
        text = "".join(parser.parts).strip()
        # FFmpeg's ASS-to-SRT conversion can expose vector drawing commands as
        # text. They are not subtitles and cannot be represented by TTML text.
        if (re.search(r"\{=\d+\}[mlb]\s+-?\d", text) or
                re.match(r"^[mlb]\s+-?\d+(?:\.\d+)?\s+-?\d", text)):
            continue
        if text:
            cues.append((start, end, text, top))
    return cues


def cell_width(text):
    width = 0
    for char in text:
        if unicodedata.combining(char):
            continue
        width += 2 if unicodedata.east_asian_width(char) in ("W", "F") else 1
    return width


def wrap_line(text):
    """Wrap one logical subtitle line to the sample's 32-column safe area."""
    output = []
    current = ""
    for word in text.split():
        proposed = f"{current} {word}" if current else word
        if cell_width(proposed) <= MAX_COLUMNS:
            current = proposed
            continue
        if current:
            output.append(current)
            current = ""
        for char in word:
            if current and cell_width(current + char) > MAX_COLUMNS:
                output.append(current)
                current = ""
            current += char
    if current:
        output.append(current)
    return output


def caption_lines(text):
    lines = []
    for original in text.splitlines():
        lines.extend(wrap_line(original))
    return lines


def build_ttml(cues, language="en"):
    root = ET.Element(f"{{{TT}}}tt", {
        f"{{{TTP}}}profile": "http://www.decellc.org/profile/2012/01/cff-tt",
        f"{{{TTS}}}extent": "960px 540px",
        f"{{{XML}}}lang": language,
        f"{{{XSI}}}schemaLocation":
            "http://www.w3.org/ns/ttml ttaf1-dfxp.xsd "
            "http://www.smpte-ra.org/schemas/2052-1/2010/smpte-tt smpte-tt.xsd",
    })
    head = ET.SubElement(root, f"{{{TT}}}head")
    ET.SubElement(head, f"{{{SMPTE}}}information", {
        f"{{{M608}}}channel": "CC1",
        "mode": "Enhanced",
        "origin": SMPTE + "#cea608",
    })
    styling = ET.SubElement(head, f"{{{TT}}}styling")
    ET.SubElement(styling, f"{{{TT}}}style", {
        f"{{{XML}}}id": "basic0",
        f"{{{TTS}}}backgroundColor": "black",
        f"{{{TTS}}}color": "white",
        f"{{{TTS}}}fontFamily": "monospace",
        f"{{{TTS}}}fontSize": "75%",
    })
    layout = ET.SubElement(head, f"{{{TT}}}layout")
    for name in ("bottom", "top"):
        ET.SubElement(layout, f"{{{TT}}}region", {
            f"{{{XML}}}id": name,
            f"{{{TTS}}}backgroundColor": "transparent",
            f"{{{TTS}}}showBackground": "whenActive",
            f"{{{TTS}}}textAlign": "left",
        })
    body = ET.SubElement(root, f"{{{TT}}}body", {"timeContainer": "par", f"{{{XML}}}space": "preserve"})
    div = ET.SubElement(body, f"{{{TT}}}div")
    for start, end, text, top in cues:
        lines = caption_lines(text)
        for index, line in enumerate(lines):
            width = cell_width(line) * CELL_PERCENT
            x = (100.0 - width) / 2.0
            y = (TOP_LINE_Y_PERCENT + index * LINE_HEIGHT_PERCENT if top else
                 BOTTOM_LINE_Y_PERCENT - (len(lines) - 1 - index) * LINE_HEIGHT_PERCENT)
            p = ET.SubElement(div, f"{{{TT}}}p", {
                "begin": start,
                "end": end,
                "region": "top" if top else "bottom",
                "style": "basic0",
                f"{{{TTS}}}textAlign": "left",
                f"{{{TTS}}}extent": f"{width:.3f}% {LINE_HEIGHT_PERCENT:.1f}%",
                f"{{{TTS}}}origin": f"{x:.3f}% {y:.1f}%",
            })
            p.text = line
    return ET.ElementTree(root)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input_srt", type=Path)
    parser.add_argument("output_ttml", type=Path)
    parser.add_argument("--language", default="en", help="TTML language tag (default: en)")
    args = parser.parse_args()
    cues = read_cues(args.input_srt)
    if not cues:
        parser.error("no subtitles found")
    tree = build_ttml(cues, args.language)
    tree.write(args.output_ttml, encoding="utf-8", xml_declaration=True)
    print(f"Wrote {len(cues)} cues to {args.output_ttml}")


if __name__ == "__main__":
    main()
