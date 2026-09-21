"""Subtitle discovery and the hardware-tested SRT -> TTML -> M4T workflow."""
from pathlib import Path
import re
import shutil
import xml.etree.ElementTree as ET

from subtitle_formats.ass_to_vita_dialogue_srt import parse_ass, write_srt
from subtitle_formats.build_vita_m4t import build, boxes, one_box
from subtitle_formats.srt_to_vita_ttml import read_cues

TEXT_CODECS = {"ass", "ssa", "subrip", "srt", "webvtt", "mov_text", "text"}
LANGUAGES = {"en": "eng", "ja": "jpn", "fr": "fra", "de": "deu", "es": "spa",
             "it": "ita", "pt": "por", "zh": "zho", "ko": "kor", "ru": "rus"}


def external_track(path):
    path = Path(path).resolve()
    codec = path.suffix.lower().lstrip(".")
    if codec not in {"srt", "ass", "ssa", "vtt", "m4t"} or not path.is_file():
        raise ValueError("Choose an existing SRT, ASS, SSA, WebVTT or M4T subtitle file")
    language = path.stem.rsplit(".", 1)[-1].lower()
    if language not in LANGUAGES and language not in LANGUAGES.values():
        language = "und"
    return dict(id="file:" + str(path), path=str(path), codec=codec, language=language,
                label=f"{path.name} (external)", supported=True)


def discover(path, streams):
    tracks = []
    for stream in streams:
        if stream.get("codec_type") != "subtitle":
            continue
        codec = stream.get("codec_name", "unknown")
        tags = stream.get("tags", {})
        language = tags.get("language", "und")
        supported = codec in TEXT_CODECS
        tracks.append(dict(id=f"stream:{stream['index']}", index=stream["index"], codec=codec,
                           language=language, default=bool(stream.get("disposition", {}).get("default")),
                           supplemental=bool(stream.get("disposition", {}).get("forced")) or bool(re.search(r"sign|song|forced", tags.get("title", ""), re.I)),
                           supported=supported,
                           label=f"{language} — {tags.get('title', codec)} ({codec})" +
                                 ("" if supported else " — needs OCR; not supported")))
    for candidate in sorted(path.parent.iterdir()):
        if candidate.suffix.lower() in {".srt", ".ass", ".ssa", ".vtt", ".m4t"} and (
                candidate.stem.casefold() == path.stem.casefold() or
                candidate.stem.casefold().startswith(path.stem.casefold() + ".")):
            tracks.append(external_track(candidate))
    return tracks


def default_track(tracks):
    ordered = sorted(tracks, key=lambda t: (bool(t.get("supplemental")), not t.get("default", False)))
    return next((t["id"] for t in ordered if t["supported"]), "none")


def validate_m4t(path):
    data = path.read_bytes()
    one_box(data, b"ftyp")
    off, size = one_box(data, b"moov")
    if b"stpp" not in data[off:off+size]:
        raise ValueError("M4T lacks its TTML subtitle track")
    samples = 0
    for off, size, kind in boxes(data):
        if kind == b"mdat":
            ET.fromstring(data[off+8:off+size])
            samples += 1
    if not samples:
        raise ValueError("M4T contains no subtitle samples")


def prepare_subtitle(source, target, info, options, checked, run, ffmpeg, cancel=None):
    choice = options.get("subtitle_track", "none")
    if choice == "none":
        return None
    tracks = list(info.get("subtitle_tracks", []))
    if choice.startswith("file:") and not any(t["id"] == choice for t in tracks):
        tracks.append(external_track(choice[5:]))
    track = next((t for t in tracks if t["id"] == choice), None)
    if not track or not track["supported"]:
        raise ValueError("Selected subtitle track cannot be converted to M4T (bitmap subtitles need OCR)")
    output = target.with_suffix(".m4t")
    if track["codec"] == "m4t":
        validate_m4t(Path(track["path"]))
        shutil.copy2(track["path"], output)
        return dict(path=str(output), language=track["language"], format="M4T", cues=None)
    language = options.get("subtitle_language", "auto")
    if language == "auto":
        language = track["language"]
    packed = LANGUAGES.get(language, language)
    if not re.fullmatch(r"[a-z]{3}", packed):
        packed = "und"
    xml_language = next((key for key, value in LANGUAGES.items() if value == packed), packed)
    scratch = target.parent / "subtitle-work"
    scratch.mkdir()
    try:
        extracted = scratch / ("source.ass" if track["codec"] in {"ass", "ssa"} else "source.srt")
        command = [ffmpeg, "-nostdin", "-v", "error", "-n", "-i", track.get("path", str(source)),
                   "-map", f"0:{track['index']}" if "index" in track else "0:s:0", "-c:s",
                   "ass" if extracted.suffix == ".ass" else "srt", str(extracted)]
        run(command, cancel)
        if extracted.suffix == ".ass":
            cues, counts = parse_ass(extracted, generic=True, dialogue_only=options.get("subtitle_filter", "dialogue") == "dialogue")
            cleaned = scratch / "dialogue.srt"
            write_srt(sorted(cues, key=lambda c: (c.start, c.end)), cleaned)
        else:
            cleaned = extracted
            counts = {}
        cues = read_cues(cleaned)
        if not cues:
            raise ValueError("No subtitle dialogue remains. Check the track or choose All text for ASS filtering.")
        # Normalize even SRT positioning to the tested bottom caption region.
        text = cleaned.read_text(encoding="utf-8-sig")
        cleaned.write_text(re.sub(r"\{\\[^}]*\}", "", text), encoding="utf-8")
        duration = int(checked.get("duration", 0) * 1000)
        if duration <= 0:
            raise ValueError("Cannot determine video duration for M4T")
        if cancel is not None and cancel.is_set():
            raise RuntimeError("Operation cancelled")
        build(None, cleaned, output, duration, 300000, 24576,
              checked["width"], checked["height"], xml_language, packed)
        validate_m4t(output)
        return dict(path=str(output), language=packed, format="M4T", cues=len(cues), filtered=counts)
    finally:
        shutil.rmtree(scratch)
