#!/usr/bin/env python3
"""Build a Sony-style fragmented M4T sidecar from SRT subtitle cues.

The default ftyp/moov layout is constructed from the measured structure.
An optional reference M4T can supply that layout; its subtitle samples are
never copied into the output.
"""

import argparse
from io import BytesIO
from pathlib import Path
import struct

try:
    from .srt_to_vita_ttml import build_ttml, read_cues
except ImportError:  # Direct CLI invocation.
    from srt_to_vita_ttml import build_ttml, read_cues


TIMESCALE = 30000


def template_header():
    """Construct the measured Sony container layout, without a sample asset."""
    matrix = struct.pack(">9I", 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000)
    mvhd = fullbox("mvhd", 0, 0, struct.pack(">4IIH", 0, 0, TIMESCALE, 0, 0x10000, 0x100) +
                   bytes(10) + matrix + bytes(24) + struct.pack(">I", 2))
    tkhd = fullbox("tkhd", 0, 7, struct.pack(">5I", 0, 0, 1, 0, 0) + bytes(8) +
                   struct.pack(">4H", 0xffff, 0, 0, 0) + matrix + struct.pack(">2I", 960 << 16, 540 << 16))
    mdhd = fullbox("mdhd", 0, 0, struct.pack(">4I2H", 0, 0, TIMESCALE, 0, iso639_packed("eng"), 0))
    hdlr = fullbox("hdlr", 0, 0, bytes(4) + b"subt" + bytes(13))
    namespace = b"http://www.smpte-ra.org/schemas/2052-1/2010/smpte-tt\0"
    stpp = box("stpp", bytes(6) + b"\0\1" + namespace * 2 + b"\0")
    stbl = box("stbl", fullbox("stsd", 0, 0, struct.pack(">I", 1) + stpp) +
               fullbox("stts", 0, 0, bytes(4)) + fullbox("stsc", 0, 0, bytes(4)) +
               fullbox("stsz", 0, 0, bytes(8)) + fullbox("stco", 0, 0, bytes(4)))
    dinf = box("dinf", fullbox("dref", 0, 0, struct.pack(">I", 1) + fullbox("url ", 0, 1, b"")))
    mdia = box("mdia", mdhd + hdlr + box("minf", fullbox("sthd", 0, 0, b"") + dinf + stbl))
    mvex = box("mvex", fullbox("mehd", 1, 0, bytes(8)) +
               fullbox("trex", 0, 0, struct.pack(">5I", 1, 1, 1001, 0, 0)))
    return box("ftyp", b"senv" + bytes(4) + b"senviso6dash") + box("moov", mvhd + box("trak", tkhd + mdia) + mvex)


def box(name, data):
    return struct.pack(">I4s", len(data) + 8, name.encode("ascii")) + data


def fullbox(name, version, flags, data):
    return box(name, bytes((version,)) + flags.to_bytes(3, "big") + data)


def boxes(data, start=0, end=None):
    if end is None:
        end = len(data)
    offset = start
    while offset < end:
        if offset + 8 > end:
            raise ValueError("truncated box header")
        size, name = struct.unpack_from(">I4s", data, offset)
        if size < 8 or offset + size > end:
            raise ValueError(f"invalid {name!r} box at {offset}")
        yield offset, size, name
        offset += size


def one_box(data, name, start=0, end=None):
    entries = [(off, size) for off, size, kind in boxes(data, start, end) if kind == name]
    if len(entries) != 1:
        raise ValueError(f"expected one {name!r}, found {len(entries)}")
    return entries[0]


def iso639_packed(language):
    if len(language) != 3 or not language.isascii() or not language.islower() or not language.isalpha():
        raise ValueError("track language must be three lowercase ISO 639-2 letters")
    return ((ord(language[0]) - 96) << 10 |
            (ord(language[1]) - 96) << 5 |
            (ord(language[2]) - 96))


def patch_moov(template, duration_ticks, width, height, track_language):
    moov = bytearray(template)
    mvhd_off, _ = one_box(moov, b"mvhd", 8)
    struct.pack_into(">II", moov, mvhd_off + 12, 0, 0)
    trak_off, trak_size = one_box(moov, b"trak", 8)
    tkhd_off, tkhd_size = one_box(moov, b"tkhd", trak_off + 8, trak_off + trak_size)
    struct.pack_into(">II", moov, tkhd_off + 12, 0, 0)
    struct.pack_into(">II", moov, tkhd_off + tkhd_size - 8, width << 16, height << 16)
    mdia_off, mdia_size = one_box(moov, b"mdia", trak_off + 8, trak_off + trak_size)
    mdhd_off, _ = one_box(moov, b"mdhd", mdia_off + 8, mdia_off + mdia_size)
    if moov[mdhd_off + 8] != 0:
        raise ValueError("template mdhd is not version 0")
    struct.pack_into(">II", moov, mdhd_off + 12, 0, 0)
    struct.pack_into(">H", moov, mdhd_off + 28, iso639_packed(track_language))
    mvex_off, mvex_size = one_box(moov, b"mvex", 8)
    mehd_off, _ = one_box(moov, b"mehd", mvex_off + 8, mvex_off + mvex_size)
    if moov[mehd_off + 8] != 1:
        raise ValueError("template mehd is not version 1")
    struct.pack_into(">Q", moov, mehd_off + 12, duration_ticks)
    if b"stpp" not in moov or b"smpte-tt" not in moov:
        raise ValueError("template lacks Sony SMPTE-TT stpp description")
    return bytes(moov)


def milliseconds(clock):
    hours, minutes, seconds = clock.split(":")
    whole, fraction = seconds.split(".")
    return ((int(hours) * 60 + int(minutes)) * 60 + int(whole)) * 1000 + int(fraction)


def clock(ms):
    seconds, fraction = divmod(ms, 1000)
    minutes, seconds = divmod(seconds, 60)
    hours, minutes = divmod(minutes, 60)
    return f"{hours:02}:{minutes:02}:{seconds:02}.{fraction:03}"


def xml_samples(cues, duration_ms, chunk_ms, max_xml_bytes, language):
    start = 0
    while start < duration_ms:
        end = min(duration_ms, start + chunk_ms)
        while True:
            output = BytesIO()
            piece = []
            for begin, finish, text, top in cues:
                begin_ms, end_ms = milliseconds(begin), milliseconds(finish)
                if begin_ms < end and end_ms > start:
                    piece.append((clock(max(begin_ms, start)), clock(min(end_ms, end)), text, top))
            build_ttml(piece, language).write(output, encoding="utf-8", xml_declaration=True)
            xml = output.getvalue() + b"\n"
            if len(xml) <= max_xml_bytes:
                break
            if end - start <= 40:
                raise ValueError(f"TTML exceeds {max_xml_bytes} bytes within 40 ms at {start} ms")
            end = start + max(40, (end - start) // 2)
        yield xml, end - start
        start = end


def make_moof(number, decode_ticks, duration_ticks, sample_size):
    mfhd = fullbox("mfhd", 0, 0, struct.pack(">I", number))
    tfhd = fullbox("tfhd", 0, 0x20000, struct.pack(">I", 1))
    tfdt = fullbox("tfdt", 1, 0, struct.pack(">Q", decode_ticks))
    trun = fullbox("trun", 0, 0x301, struct.pack(">IIII", 1, 126, duration_ticks, sample_size))
    subs = fullbox("subs", 0, 0, struct.pack(">IIH", 1, 1, 0))
    result = box("moof", mfhd + box("traf", tfhd + tfdt + trun + subs))
    if len(result) != 118:
        raise AssertionError(f"unexpected moof size: {len(result)}")
    return result


def sidx(earliest, first_offset, references):
    refs = b"".join(struct.pack(">III", kind_size, duration, 0x90000000)
                    for kind_size, duration in references)
    payload = struct.pack(">IIQQHH", 1, TIMESCALE, earliest, first_offset,
                          0, len(references)) + refs
    return fullbox("sidx", 1, 0, payload)


def make_sidx(fragment_sizes, durations):
    # Sony's specimen uses a master SIDX whose references point to 52-byte
    # child SIDX boxes. All child indexes precede the media fragments.
    count = len(fragment_sizes)
    child_size = 52
    master = sidx(0, 0, [(0x80000000 | child_size, duration)
                         for duration in durations])
    children = []
    media_before = 0
    time_before = 0
    for i, (fragment_size, duration) in enumerate(zip(fragment_sizes, durations)):
        first_offset = (count - i - 1) * child_size + media_before
        child = sidx(time_before, first_offset, [(fragment_size, duration)])
        if len(child) != child_size:
            raise AssertionError("child segment index is not 52 bytes")
        children.append(child)
        media_before += fragment_size
        time_before += duration
    return master + b"".join(children)


def build(template_path, srt_path, output_path, duration_ms, chunk_ms,
          max_xml_bytes, width, height, language="en", track_language="eng"):
    template = template_path.read_bytes() if template_path else template_header()
    ftyp_off, ftyp_size = one_box(template, b"ftyp")
    moov_off, moov_size = one_box(template, b"moov")
    if ftyp_off != 0 or moov_off != ftyp_size:
        raise ValueError("template must begin ftyp/moov")
    cues = read_cues(srt_path)
    if not cues:
        raise ValueError("SRT has no cues")
    samples = list(xml_samples(cues, duration_ms, chunk_ms, max_xml_bytes, language))
    durations = [ms * TIMESCALE // 1000 for _, ms in samples]
    fragments = []
    decode_ticks = 0
    for number, ((xml, _), duration) in enumerate(zip(samples, durations), 1):
        fragments.append(make_moof(number, decode_ticks, duration, len(xml)) + box("mdat", xml))
        decode_ticks += duration
    output = (template[:ftyp_size] +
              patch_moov(template[moov_off:moov_off + moov_size], decode_ticks,
                         width, height, track_language) +
              make_sidx([len(part) for part in fragments], durations) + b"".join(fragments))
    output_path.write_bytes(output)
    print(f"{output_path}: {len(cues)} cues, {len(fragments)} fragments, "
          f"largest XML {max(len(xml) for xml, _ in samples)} bytes, {len(output)} bytes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--template", type=Path, help="optional reference container; not required")
    parser.add_argument("--srt", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--duration-ms", required=True, type=int)
    parser.add_argument("--chunk-ms", type=int, default=300000)
    parser.add_argument("--max-xml-bytes", type=int, default=24576)
    parser.add_argument("--width", type=int, default=960)
    parser.add_argument("--height", type=int, default=540)
    parser.add_argument("--language", default="en", help="TTML language tag (default: en)")
    parser.add_argument("--track-language", default="eng",
                        help="three-letter ISO 639-2 track language (default: eng)")
    args = parser.parse_args()
    if args.duration_ms <= 0 or args.chunk_ms <= 0 or args.max_xml_bytes <= 0:
        parser.error("durations must be positive")
    build(args.template, args.srt, args.output, args.duration_ms,
          args.chunk_ms, args.max_xml_bytes, args.width, args.height,
          args.language, args.track_language)


if __name__ == "__main__":
    main()
