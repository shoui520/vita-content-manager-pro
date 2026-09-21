"""Content-based Vita media checks and bounded, user-adjustable conversion presets.

Policy/evidence: docs/media-formats.md. Eligibility describes encoded media,
not database-import support or a guarantee of stock-app playback.
"""
from __future__ import annotations

import io
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess
import queue
import tempfile
import threading

from PIL import Image, ImageCms, ImageOps, UnidentifiedImageError
from subtitles import discover, prepare_subtitle, default_track
from process_lifetime import own_child_processes


def run(command, cancel=None, timeout=None):
    own_child_processes()
    with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          encoding="utf-8", errors="replace",
                          creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)) as process:
        import time
        start = time.monotonic()
        while True:
            if (cancel is not None and cancel.is_set()) or (timeout and time.monotonic() - start > timeout):
                process.terminate()
                try:
                    process.communicate(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate()
                raise RuntimeError("Operation cancelled" if cancel is not None and cancel.is_set() else "Media inspection timed out")
            try:
                out, error = process.communicate(timeout=0.2)
                break
            except subprocess.TimeoutExpired:
                pass
        if process.returncode:
            raise ValueError(error.strip()[-2000:] or "This file could not be decoded")
        return out


def tool(name):
    result = shutil.which(name)
    if not result:
        raise RuntimeError(f"{name} must be installed on PATH")
    return result


def run_ffmpeg(command, duration, cancel=None, progress=None, phase="Converting"):
    """Read FFmpeg's timestamp reports without blocking the UI or stderr pipe."""
    reports = queue.Queue(maxsize=8)
    own_child_processes()
    command = command[:1] + ["-progress", "pipe:1", "-nostats", "-stats_period", "0.25"] + command[1:]
    with tempfile.TemporaryFile(mode="w+b") as errors, subprocess.Popen(
            command, stdout=subprocess.PIPE, stderr=errors, encoding="utf-8", errors="replace",
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)) as process:
        def read():
            report = {}
            for line in process.stdout:
                key, _, value = line.strip().partition("=")
                report[key] = value
                if key == "progress":
                    try:
                        reports.put_nowait(report)
                    except queue.Full:
                        pass
                    report = {}
        reader = threading.Thread(target=read, daemon=True)
        reader.start()
        def emit(report):
            seconds = max(0, number(report.get("out_time_us", 0)) / 1000000)
            if progress:
                progress(dict(phase=phase, seconds=seconds, duration=duration,
                              percent=min(99.9, seconds / duration * 100) if duration > 0 else None,
                              speed=report.get("speed", "")))
        try:
            while process.poll() is None or reader.is_alive() or not reports.empty():
                if cancel is not None and cancel.is_set():
                    raise RuntimeError("Operation cancelled")
                try:
                    emit(reports.get(timeout=.1))
                except queue.Empty:
                    pass
            if process.wait():
                errors.seek(0, 2)
                errors.seek(max(0, errors.tell()-4000))
                raise ValueError(errors.read().decode("utf-8", "replace").strip() or "FFmpeg failed")
            if progress:
                progress(dict(phase=phase, seconds=duration, duration=duration, percent=100, speed=""))
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            reader.join()


def number(value):
    try:
        if isinstance(value, str) and "/" in value:
            a, b = value.split("/")
            return float(a) / float(b)
        return float(value)
    except (ValueError, TypeError, ZeroDivisionError):
        return 0


def matroska_doctype(path):
    """Distinguish WebM from Matroska; ffprobe names both matroska,webm."""
    with path.open("rb") as source:
        header = source.read(4096)
    if not header.startswith(b"\x1a\x45\xdf\xa3"):
        return None
    marker = header.find(b"\x42\x82", 4)
    if marker < 0 or marker + 3 > len(header):
        return None
    first = header[marker + 2]
    if not first:
        return None
    length = 9 - first.bit_length()
    end = marker + 2 + length
    if end > len(header):
        return None
    size = int.from_bytes(header[marker + 2:end], "big") & ((1 << (7 * length)) - 1)
    if size > 32 or end + size > len(header):
        return None
    doctype = header[end:end + size].decode("ascii", errors="ignore")
    return doctype if doctype in {"webm", "matroska"} else None


def image_info(path):
    with Image.open(path) as image:
        fmt = image.format
        frames = getattr(image, "n_frames", 1)
        # GIF is handled by the video pipeline to preserve its animation.
        if fmt == "GIF" or (frames > 1 and fmt in {"PNG", "WEBP"}):
            return None
        width, height = image.size
        pixels = width * height
        reasons, notes = [], []
        if fmt in {"JPEG", "MPO"}:
            limit = 522240 if image.info.get("progressive") or image.info.get("progression") else 40000000
            if image.mode not in {"RGB", "L"}:
                reasons.append("JPEG must use RGB/YCbCr or grayscale, not CMYK")
            if fmt == "MPO":
                notes.append("Gallery displays only the representative MPO image; conversion extracts that image")
        elif fmt == "PNG":
            limit = 522240 if image.mode == "P" or image.info.get("interlace") else 1966080
            if image.mode not in {"RGB", "RGBA", "L", "LA", "P", "1"}:
                reasons.append("PNG bit depth is outside the supported 8-bit preset")
        elif fmt == "BMP":
            with path.open("rb") as source:
                header = source.read(54)
            dib = struct.unpack_from("<I", header, 14)[0] if len(header) >= 30 else 0
            bits = struct.unpack_from("<H", header, 24 if dib == 12 else 28)[0] if dib else 0
            limit = 1966080 if bits in {24, 32} else 522240
            if bits not in {1, 4, 8, 16, 24, 32} or image.info.get("compression", 0) != 0:
                reasons.append("Use an uncompressed BMP or convert to JPEG")
        elif fmt == "TIFF":
            # The firmware has a second, unresolved TIFF gate. Use the lower
            # bound until its discriminator is understood.
            limit = 522240
            if image.mode not in {"RGB", "L"} or image.info.get("compression") not in {None, "raw"}:
                reasons.append("TIFF encoding needs conversion to the conservative uncompressed preset")
            notes.append("TIFF uses the conservative 522,240-pixel check; its larger firmware branch is unresolved")
        else:
            limit = 0
            reasons.append(f"{fmt} is not a stock Gallery format")
        if limit and pixels > limit:
            reasons.append(f"{fmt} exceeds its {limit:,}-pixel limit ({pixels:,} pixels)")
        if frames > 1 and fmt != "MPO":
            reasons.append("Multi-page images need a single-image conversion")
        if image.mode in {"RGBA", "LA", "P"} and ("A" in image.mode or "transparency" in image.info):
            notes.append("JPEG removes transparency; choose a background color or PNG")
        image.load()  # Catch truncated images before showing a green result.
        return dict(kind="photo", format=fmt, width=width, height=height,
                    reasons=reasons, notes=notes, eligible=not reasons, frames=frames)


def audio_reasons(stream, video=False):
    reasons = []
    codec = stream.get("codec_name")
    allowed = {"aac"} if video else {"mp3", "aac", "pcm_s16le"}
    if codec not in allowed:
        reasons.append(f"Audio codec {codec or 'unknown'} needs conversion")
    if codec == "aac" and stream.get("profile") != "LC":
        reasons.append("AAC must use the Low Complexity (LC) profile")
    if int(stream.get("sample_rate", 0)) not in {44100, 48000}:
        reasons.append("Use a 44.1 or 48 kHz audio sample rate")
    if stream.get("channels", 0) not in {1, 2}:
        reasons.append("Audio must be mono or stereo")
    return reasons


def inspect_media(raw, cancel=None):
    path = Path(raw).expanduser().resolve()
    if not path.is_file():
        raise ValueError("File not found")
    try:
        info = image_info(path)
    except (UnidentifiedImageError, OSError):
        info = None
    if info is None:
        data = json.loads(run([tool("ffprobe"), "-v", "error", "-show_format", "-show_streams",
                               "-of", "json", str(path)], cancel=cancel, timeout=30))
        streams = data.get("streams", [])
        videos = [s for s in streams if s.get("codec_type") == "video" and not s.get("disposition", {}).get("attached_pic")]
        audios = [s for s in streams if s.get("codec_type") == "audio"]
        container = data.get("format", {}).get("format_name", "")
        reasons, notes = [], []
        if not videos and not audios:
            raise ValueError("No decodable picture, video, or audio stream found")
        is_mp4 = "mp4" in container and data.get("format", {}).get("tags", {}).get("major_brand", "").strip() != "qt"
        if not videos:
            audio_codec = audios[0].get("codec_name", "unknown")
            fmt = {"opus": "Opus", "vorbis": "Vorbis", "pcm_s16le": "WAV" if container == "wav" else "PCM"}.get(audio_codec, audio_codec.upper())
        elif "matroska" in container:
            fmt = "WebM" if matroska_doctype(path) == "webm" else "MKV"
        else:
            fmt = "GIF" if container == "gif" else "MP4" if is_mp4 else container.split(",")[0].upper()
        info = dict(kind="video" if videos else "music", format=fmt,
                    tags=data.get("format", {}).get("tags", {}),
                    duration=number(data.get("format", {}).get("duration")),
                    audio_tracks=[dict(index=s["index"], label=f"Track {i+1}: {s.get('tags', {}).get('language', 'und')} — {s.get('codec_name')} ({s.get('channels', 0)} channels)") for i, s in enumerate(audios)],
                    has_cover=any(s.get("disposition", {}).get("attached_pic") for s in streams))
        if videos:
            v = videos[0]
            w, h = int(v.get("width", 0)), int(v.get("height", 0))
            fps = max(number(v.get("avg_frame_rate")), number(v.get("r_frame_rate")))
            mb = math.ceil(w / 16) * math.ceil(h / 16)
            codec, profile = v.get("codec_name"), v.get("profile", "")
            if not is_mp4:
                reasons.append("Video needs an MP4 container" if fmt != "GIF" else "Gallery shows GIF as a still; convert to MP4 to keep animation")
            if codec == "h264":
                if profile not in {"High", "Main", "Baseline", "Constrained Baseline"} or not 0 < v.get("level", 0) <= 40:
                    reasons.append("Use H.264 Baseline/Main/High at Level 4.0 or lower")
                if w > 1920 or h > 1080 or mb > 8192 or mb * fps > 245760.5 or fps <= 0:
                    reasons.append("Resolution or frame rate exceeds the H.264 Level 4.0 limits")
                if v.get("refs", 1) * mb > 32768:
                    reasons.append("Too many H.264 reference frames for Level 4.0")
            elif codec == "mpeg4":
                if profile != "Simple Profile" or w > 1280 or h > 720 or fps > 30:
                    reasons.append("MPEG-4 Part 2 must use the conservative Simple Profile 720p30 preset")
            else:
                reasons.append(f"Video codec {codec} needs H.264 conversion")
            if v.get("pix_fmt") != "yuv420p":
                reasons.append("Video must use 8-bit 4:2:0 color")
            rate = number(v.get("bit_rate") or data.get("format", {}).get("bit_rate"))
            if rate > (25000000 if profile == "High" else 20000000):
                reasons.append("Video bitrate exceeds the Level 4.0 limit")
            if any(number(x.get("rotation")) for x in v.get("side_data_list", [])):
                reasons.append("Video rotation needs to be baked into the picture")
            if not audios:
                reasons.append("Videos needs an AAC audio track; conversion adds silent AAC-LC")
            for a in audios:
                reasons += audio_reasons(a, video=True)
            if len(audios) > 1:
                notes.append("Choose which audio track to keep when converting")
            info["subtitle_tracks"] = discover(path, streams)
            unsupported = sum(not t["supported"] for t in info["subtitle_tracks"])
            if unsupported:
                notes.append(f"{unsupported} subtitle track(s) require OCR and cannot be converted to M4T")
            info.update(width=w, height=h, fps=fps, video_index=v["index"], codec=codec)
        else:
            a = audios[0]
            reasons += audio_reasons(a)
            codec = a.get("codec_name")
            if not ((codec == "mp3" and container == "mp3") or (codec == "aac" and is_mp4) or (codec == "pcm_s16le" and container == "wav")):
                reasons.append("Use MP3, AAC-LC in M4A, or 16-bit PCM in WAV")
            info.update(codec=codec, sample_rate=int(a.get("sample_rate", 0)), channels=int(a.get("channels", 0)))
        if any(s.get("codec_tag_string") in {"encv", "enca"} for s in streams):
            reasons.append("Protected media cannot be converted by this app")
        info.update(eligible=not reasons, reasons=list(dict.fromkeys(reasons)), notes=notes)
    info.update(path=str(path), name=path.name, size=path.stat().st_size)
    info["defaults"] = defaults(info)
    return info


def defaults(info):
    common = dict(keep_compatible=True)
    if info["kind"] == "photo":
        return dict(common, output="jpeg", quality=90, resolution="original", background="white")
    if info["kind"] == "music":
        return dict(common, output="mp3", mode="cbr", bitrate=192, vbr=2, sample_rate=44100)
    return dict(common, output="mp4", resolution="vita", fit="contain", fps="30" if info["format"] == "GIF" else "auto",
                quality=19, rate_mode="quality", bitrate=4000, preset="slow", audio_bitrate=192,
                extract_bitrate=192, mode="cbr", vbr=2,
                subtitle_track=default_track(info.get("subtitle_tracks", [])),
                subtitle_filter="dialogue", subtitle_language="auto",
                sample_rate=48000, audio_track=info.get("audio_tracks", [{}])[0].get("index", -1) if info.get("audio_tracks") else -1,
                loops=1)


def settings(info, supplied):
    options = defaults(info)
    options.update(supplied or {})
    choices = {"resolution": {"original", "vita", "720", "1080"} | ({"480", "360", "240", "144"} if info["kind"] == "video" else set()), "fit": {"contain", "pad", "stretch", "width", "height"},
               "fps": {"auto", "15", "24", "25", "30", "50", "60"}, "preset": {"ultrafast", "fast", "medium", "slow", "slower"},
               "mode": {"cbr", "vbr"}, "rate_mode": {"quality", "bitrate"}, "background": {"white", "black"},
               "subtitle_filter": {"dialogue", "all"},
               "output": {"mp4", "mp3", "aac", "wav"} if info["kind"] == "video" else {"mp3", "aac", "wav"} if info["kind"] == "music" else {"jpeg", "png", "bmp", "tiff"}}
    for key, allowed in choices.items():
        if key in options and options[key] not in allowed:
            raise ValueError(f"Invalid {key} setting")
    bounds = {"quality": (1, 100) if info["kind"] == "photo" else (16, 28), "bitrate": (500, 16000) if info["kind"] == "video" else (32, 320),
              "audio_bitrate": (32, 320), "extract_bitrate": (32, 320), "vbr": (0, 9), "loops": (1, 100)}
    for key, (low, high) in bounds.items():
        if key in options:
            options[key] = int(options[key])
            if not low <= options[key] <= high:
                raise ValueError(f"{key} must be between {low} and {high}")
    if "sample_rate" in options:
        options["sample_rate"] = int(options["sample_rate"])
        if options["sample_rate"] not in {44100, 48000}:
            raise ValueError("Choose 44.1 or 48 kHz")
    if info["kind"] == "music" and options["output"] == "mp3" and options["mode"] == "cbr" and options["bitrate"] not in {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}:
        raise ValueError("Choose a supported MP3 CBR bitrate")
    if info["kind"] == "video":
        options["audio_track"] = int(options["audio_track"])
        if options["audio_track"] not in [-1] + [s["index"] for s in info.get("audio_tracks", [])]:
            raise ValueError("Audio track is not in this file")
        if options["output"] != "mp4" and options["audio_track"] < 0:
            raise ValueError("This video has no selected soundtrack to extract")
        if options["output"] == "mp3" and options["mode"] == "cbr" and options["extract_bitrate"] not in {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}:
            raise ValueError("Choose a supported MP3 CBR bitrate")
        if options["output"] == "mp4" and options["resolution"] == "1080" and options["fps"] not in {"auto", "15", "24", "25", "30"}:
            raise ValueError("1080p supports up to 30 fps. Choose 720p for 50/60 fps.")
    return options


def image_convert(source, target, options):
    with Image.open(source) as original:
        image = ImageOps.exif_transpose(original)
        alpha = image.convert("RGBA").getchannel("A") if "A" in image.getbands() or "transparency" in image.info else None
        profile = image.info.get("icc_profile")
        if profile:
            image = ImageCms.profileToProfile(image, ImageCms.ImageCmsProfile(io.BytesIO(profile)), ImageCms.createProfile("sRGB"), outputMode="RGB")
        else:
            image = image.convert("RGB")
        if alpha is not None:
            if options["output"] == "png":
                image.putalpha(alpha)
            else:
                background = Image.new("RGB", image.size, options["background"])
                background.paste(image, mask=alpha)
                image = background
        limit = {"jpeg": 39900000, "png": 1966080, "bmp": 1966080, "tiff": 522240}[options["output"]]
        scale = min(1, math.sqrt(limit / (image.width * image.height)))
        if options["resolution"] != "original":
            w, h = {"vita": (960, 544), "720": (1280, 720), "1080": (1920, 1080)}[options["resolution"]]
            scale = min(scale, w / image.width, h / image.height)
        if scale < 1:
            image = image.resize((max(1, int(image.width * scale)), max(1, int(image.height * scale))), Image.Resampling.LANCZOS)
        fmt = options["output"]
        kwargs = dict(quality=options["quality"], subsampling=2, progressive=False, optimize=True) if fmt == "jpeg" else dict(compression="raw") if fmt == "tiff" else {}
        image.save(target, format=fmt.upper(), **kwargs)


def command_for(source, target, info, options):
    command = [tool("ffmpeg"), "-hide_banner", "-nostdin", "-v", "error", "-n"]
    if info["format"] == "GIF":
        command += ["-ignore_loop", "1", "-stream_loop", str(options["loops"] - 1)]
    command += ["-i", str(source)]
    silent = info["kind"] == "video" and options["output"] == "mp4" and options["audio_track"] < 0
    if silent:
        command += ["-f", "lavfi", "-i", f"anullsrc=channel_layout=stereo:sample_rate={options['sample_rate']}"]
    command += ["-map_metadata", "0"]
    if info["kind"] == "video" and options["output"] == "mp4":
        w, h = {"vita": (960, 544), "720": (1280, 720), "1080": (1920, 1080), "original": (1920, 1080),
                "480": (854, 480), "360": (640, 360), "240": (426, 240), "144": (256, 144)}[options["resolution"]]
        fit = options["fit"]
        if fit == "stretch":
            scale = f"scale={w}:{h}"
        elif fit in {"width", "height"}:
            scale = f"scale={w}:{h}:force_original_aspect_ratio=decrease:force_divisible_by=2"
            # Fill the chosen axis; crop any overflow to the selected canvas.
            scale = (f"scale={w}:-2,crop=iw:min(ih\\,{h})" if fit == "width" else f"scale=-2:{h},crop=min(iw\\,{w}):ih")
        else:
            scale = f"scale=w='min({w},iw)':h='min({h},ih)':force_original_aspect_ratio=decrease:force_divisible_by=2"
            if fit == "pad":
                scale += f",pad={w}:{h}:(ow-iw)/2:(oh-ih)/2"
        scale += ",setsar=1"
        command += ["-map", f"0:{info['video_index']}", "-vf", scale,
                    "-c:v", "libx264", "-profile:v", "high", "-level:v", "4.0", "-pix_fmt", "yuv420p",
                    "-preset", options["preset"], "-maxrate", "16000k", "-bufsize", "20000k", "-refs", "3"]
        command += ["-crf", str(options["quality"])] if options["rate_mode"] == "quality" else ["-b:v", f"{options['bitrate']}k"]
        ceiling = 30 if w > 1280 or h > 720 else 60
        command += ["-fpsmax", str(ceiling)] if options["fps"] == "auto" else ["-r", str(min(int(options["fps"]), ceiling))]
        if options["audio_track"] >= 0:
            command += ["-map", f"0:{options['audio_track']}", "-c:a", "aac", "-profile:a", "aac_low",
                        "-b:a", f"{options['audio_bitrate']}k", "-ar", str(options["sample_rate"]), "-ac", "2"]
        else:
            command += ["-map", "1:a:0", "-c:a", "aac", "-profile:a", "aac_low",
                        "-b:a", f"{options['audio_bitrate']}k", "-ar", str(options["sample_rate"]),
                        "-ac", "2", "-shortest"]
        command += ["-sn", "-movflags", "+faststart", "-f", "mp4"]
    else:
        extracting = info["kind"] == "video"
        options = dict(options, bitrate=options["extract_bitrate"]) if extracting else options
        cover = info.get("has_cover") and not extracting
        command += ["-map", f"0:{options['audio_track']}" if extracting else "0:a:0", "-sn", "-ar", str(options["sample_rate"]), "-ac", "2"]
        if extracting:
            command += ["-vn"]
        if options["output"] == "mp3":
            command += ["-c:a", "libmp3lame", "-id3v2_version", "3"]
            command += ["-b:a", f"{options['bitrate']}k"] if options["mode"] == "cbr" else ["-q:a", str(options["vbr"])]
            if cover:
                command += ["-map", "0:v:0", "-c:v", "mjpeg", "-disposition:v", "attached_pic"]
            command += ["-f", "mp3"]
        elif options["output"] == "aac":
            command += ["-c:a", "aac", "-profile:a", "aac_low", "-b:a", f"{options['bitrate']}k"]
            if cover:
                command += ["-map", "0:v:0", "-c:v", "mjpeg", "-disposition:v", "attached_pic"]
            command += ["-movflags", "+faststart", "-f", "ipod"]
        else:
            command += ["-vn", "-c:a", "pcm_s16le", "-f", "wav"]
    return command + [str(target)]


def vita_filename(name):
    name = ''.join('_' if ord(c) < 32 or c in '<>:"/\\|?*' else c for c in name).rstrip(' .')
    if not name or name in {'.', '..'}:
        name = '_'
    if len(name.encode('utf-8')) > 255:
        suffix = Path(name).suffix
        name = Path(name).stem.encode('utf-8')[:255-len(suffix.encode('utf-8'))].decode('utf-8', 'ignore') + suffix
    return name


def prepare(source, directory, supplied=None, cancel=None, progress=None):
    def stage(name):
        if cancel is not None and cancel.is_set():
            raise RuntimeError("Operation cancelled")
        if progress:
            progress(dict(phase=name, percent=None, seconds=0, duration=0, speed=""))
    stage("Inspecting source")
    info = inspect_media(source, cancel)
    options = settings(info, supplied)
    if info["eligible"] and options.get("keep_compatible") and not (info["kind"] == "video" and options["output"] != "mp4"):
        target = directory / vita_filename(source.name)
        stage("Copying compatible original")
        shutil.copy2(source, target)
    else:
        suffix = {"mp4": ".mp4", "mp3": ".mp3", "aac": ".m4a", "wav": ".wav", "jpeg": ".jpg", "png": ".png", "bmp": ".bmp", "tiff": ".tif"}[options["output"]]
        target = directory / vita_filename(source.stem + suffix)
        if info["kind"] == "photo":
            stage("Converting image")
            image_convert(source, target, options)
        else:
            duration = info.get("duration", 0) * (options.get("loops", 1) if info["format"] == "GIF" else 1)
            run_ffmpeg(command_for(source, target, info, options), duration, cancel, progress)
    stage("Checking eligibility")
    checked = inspect_media(target, cancel)
    if not checked["eligible"]:
        raise ValueError("Output did not pass eligibility: " + "; ".join(checked["reasons"]))
    if checked["kind"] != "photo":
        # Decode the full output before publishing a green replacement row.
        run_ffmpeg([tool("ffmpeg"), "-nostdin", "-v", "error", "-xerror", "-i", str(target),
             "-map", "0:a?", "-map", "0:v?", "-f", "null", "-"], checked.get("duration", 0), cancel, progress, "Verifying output")
    if checked["kind"] == "video":
        stage("Preparing subtitles")
        sidecar = prepare_subtitle(source, target, info, options, checked, run, tool("ffmpeg"), cancel)
        checked["sidecars"] = [sidecar] if sidecar else []
        if sidecar:
            checked["subtitle_tracks"] = discover(target, [])
            checked["defaults"] = defaults(checked)
            checked["notes"].append("M4T subtitles prepared beside the video; transfer must keep both files together")
    if cancel is not None and cancel.is_set():
        raise RuntimeError("Operation cancelled")
    return target, checked
