"""Streaming LAN transfer and native-library sync; no user pairing codes."""
from __future__ import annotations
import http.client
import ipaddress
import json
from pathlib import Path
import re
import time
from urllib.parse import quote
import uuid
import io
import base64
from PIL import Image

PORT = 39323
CHUNK = 64 * 1024
ERRORS = {-1001: "Close the corresponding Music, Videos or Gallery app on the Vita, then retry.",
          -1002: "An earlier operation needs recovery; its files have been retained.",
          -1003: "The Vita could not confirm that its storage was synced.",
          -1004: "The Vita photo decoder rejected this image.",
          -1005: "The Vita exporter did not return a destination.",
          -1006: "Not enough free space on the Vita.",
          -1007: "The transfer was interrupted.",
          -1008: "The exported file or media-library row could not be verified; the upload was retained."}

def address(value):
    ip = ipaddress.ip_address(value.strip())
    if not isinstance(ip, ipaddress.IPv4Address):
        raise ValueError("Enter the Vita's IPv4 address")
    return str(ip)

def request(ip, method, route, body=b"", queue=None, port=PORT):
    connection = http.client.HTTPConnection(address(ip), port, timeout=10)
    try:
        headers = {"Content-Length": str(len(body))}
        if queue:
            headers["X-VCM-Queue"] = queue
        connection.request(method, "/v2/" + route, body, headers)
        response = connection.getresponse()
        payload = json.loads(response.read(32768))
        if not 200 <= response.status < 300:
            raise RuntimeError(payload.get("error", f"Vita returned HTTP {response.status}"))
        return payload
    finally:
        connection.close()

def status(ip, port=PORT):
    result = request(ip, "GET", "status", port=port)
    if result.get("service") != "vita-content-manager" or result.get("version") != 2:
        raise RuntimeError("Update the Vita app to the current Wi-Fi sync build")
    return result

def library_page(ip, kind, offset=0, limit=32, port=PORT):
    if kind not in ("photo", "music", "video") or offset < 0 or not 1 <= limit <= 32:
        raise ValueError("Invalid Vita library page")
    result = request(ip, "GET", f"library/{kind}?offset={offset}&limit={limit}", port=port)
    if not isinstance(result.get("items"), list) or not isinstance(result.get("next_offset"), int):
        raise RuntimeError("Invalid library response from Vita")
    return result

def _photo_thumbnails_batch(ip, offset, limit, port):
    if offset < 0 or not 1 <= limit <= 32:
        raise ValueError("Invalid thumbnail page")
    conn = http.client.HTTPConnection(address(ip), port, timeout=20)
    try:
        conn.request("GET", f"/v2/library/photo/thumbs?offset={offset}&limit={limit}")
        response = conn.getresponse()
        if response.status != 200:
            raise RuntimeError(f"Vita thumbnail request failed (HTTP {response.status})")
        data = response.read(32 * (65536 + 12) + 1)
        if len(data) > 32 * (65536 + 12):
            raise RuntimeError("Oversized thumbnail response")
    finally:
        conn.close()
    thumbs = {}
    at = 0
    while at < len(data):
        if at + 12 > len(data):
            raise RuntimeError("Incomplete Vita thumbnail header")
        identifier = str(int.from_bytes(data[at:at+8], "little"))
        size = int.from_bytes(data[at+8:at+12], "little")
        at += 12
        if size > 65536 or at + size > len(data):
            raise RuntimeError("Incomplete Vita thumbnail")
        if size:
            with Image.open(io.BytesIO(data[at:at+size])) as source:
                if source.format != "DDS" or source.width > 256 or source.height > 256:
                    raise RuntimeError("Invalid Vita photo thumbnail")
                output = io.BytesIO()
                source.convert("RGB").save(output, format="PNG", optimize=True)
                thumbs[identifier] = "data:image/png;base64," + base64.b64encode(output.getvalue()).decode("ascii")
        at += size
    return thumbs

def photo_thumbnails(ip, offset, limit=32, port=PORT):
    if offset < 0 or not 1 <= limit <= 32:
        raise ValueError("Invalid thumbnail page")
    try:
        return _photo_thumbnails_batch(ip, offset, limit, port)
    except (http.client.IncompleteRead, ConnectionError, TimeoutError):
        if limit == 1:
            return {}
        first = limit // 2
        return (photo_thumbnails(ip, offset, first, port) |
                photo_thumbnails(ip, offset + first, limit - first, port))

def download_media(ip, kind, identifier, target, cancel=None, progress=None, port=PORT, sidecar=False):
    if kind not in ("photo", "music", "video") or not re.fullmatch(r"[0-9]{1,20}", str(identifier)):
        raise ValueError("Invalid Vita media ID")
    if sidecar and kind != "video":
        raise ValueError("Only videos can have M4T sidecars")
    target = Path(target)
    conn = http.client.HTTPConnection(address(ip), port, timeout=30)
    part = target.with_name(target.name + f".{uuid.uuid4().hex}.vcm-part")
    try:
        conn.request("GET", f"/v2/library/{kind}/{identifier}" + ("/m4t" if sidecar else ""))
        response = conn.getresponse()
        if response.status != 200:
            try:
                message = json.loads(response.read(4096)).get("error")
            except (ValueError, OSError):
                message = None
            raise RuntimeError(message or f"Vita returned HTTP {response.status}")
        length = response.getheader("Content-Length")
        if not length or not length.isdigit() or int(length) > 8 * 1024**3:
            raise RuntimeError("Invalid media length from Vita")
        expected = int(length)
        received = 0
        with part.open("xb") as sink:
            while received < expected:
                if cancel is not None and cancel.is_set():
                    raise RuntimeError("Transfer cancelled")
                chunk = response.read(min(CHUNK, expected - received))
                if not chunk:
                    raise RuntimeError("Vita transfer was interrupted")
                sink.write(chunk)
                received += len(chunk)
                if progress:
                    progress(received, expected)
            sink.flush()
            import os
            os.fsync(sink.fileno())
        # Publish atomically without replacing an existing user file.
        import os
        if os.name == "nt":
            os.rename(part, target)  # Windows rename fails if target exists.
        else:
            os.link(part, target)
            part.unlink()
        return expected
    finally:
        conn.close()
        part.unlink(missing_ok=True)

def metadata(info, sidecar=None):
    path = Path(info["path"])
    kind = {"photo": 0, "music": 1, "video": 2}[info["kind"]]
    ext = ("mp4" if kind == 2 else {"mp3": "mp3", "aac": "m4a", "pcm_s16le": "wav"}[info["codec"]]
           if kind == 1 else {"JPEG": "jpg", "PNG": "png", "BMP": "bmp", "TIFF": "tif", "GIF": "gif", "MPO": "mpo"}[info["format"]])
    tags = {k.lower(): v for k, v in info.get("tags", {}).items()}
    def text(value, capacity=256):
        value = "".join(c if ord(c) >= 32 and ord(c) != 127 else " " for c in str(value))
        return value.encode("utf-8")[:capacity-1].decode("utf-8", "ignore")
    track = str(tags.get("track", "0")).split("/")[0]
    return dict(id=uuid.uuid4().hex, extension=ext, size=path.stat().st_size,
                subtitle_size=Path(sidecar).stat().st_size if sidecar else 0, kind=kind,
                name=text(path.name), title=text(tags.get("title") or path.stem),
                artist=text(tags.get("artist", "")), album=text(tags.get("album", "")),
                album_artist=text(tags.get("album_artist", tags.get("albumartist", ""))),
                genre=text(tags.get("genre", ""), 128), duration_ms=round(info.get("duration", 0)*1000),
                sample_rate=info.get("sample_rate", 0), channels=info.get("channels", 0),
                track=int(track) if track.isdigit() and int(track)<100000 else 0,
                folder=text(info.get('video_folder', '')) if kind == 2 else '')

def queue_payload(queue, items):
    if not 1 <= len(items) <= 128:
        raise ValueError("Select between 1 and 128 files per transfer")
    fields = ("id", "extension", "size", "subtitle_size", "kind", "name", "title", "artist",
                "album", "album_artist", "genre", "duration_ms", "sample_rate", "channels", "track", "folder")
    body = queue + "\n" + "".join("\t".join(quote(str(item[key]), safe="") for key in fields) + "\n" for item in items)
    return body.encode("ascii")

def publish(ip, queue, items, port=PORT):
    return request(ip, "POST", "queue", queue_payload(queue, items), port=port)

def upload(ip, queue, path, transfer_id, extension=None, port=PORT, cancel=None, progress=None):
    path = Path(path)
    extension = extension or path.suffix[1:].lower()
    if not re.fullmatch(r"[0-9a-f]{32}", transfer_id) or not re.fullmatch(r"[a-z0-9]{2,4}", extension):
        raise ValueError("Invalid transfer identifier")
    size = path.stat().st_size
    if not 0 < size <= 8 * 1024**3:
        raise ValueError("File must be between 1 byte and 8 GiB")
    conn = http.client.HTTPConnection(address(ip), port, timeout=20)
    try:
        conn.putrequest("PUT", f"/v2/files/{transfer_id}.{extension}")
        conn.putheader("Content-Length", str(size))
        conn.putheader("X-VCM-Queue", queue)
        conn.endheaders()
        sent, last = 0, 0
        with path.open("rb") as source:
            while chunk := source.read(CHUNK):
                if cancel is not None and cancel.is_set():
                    raise RuntimeError("Transfer cancelled")
                conn.send(chunk)
                sent += len(chunk)
                now = time.monotonic()
                if progress and (now-last >= .1 or sent == size):
                    progress(sent, size)
                    last = now
        response = conn.getresponse()
        receipt = json.loads(response.read(4096))
        if response.status != 201 or not receipt.get("ok") or receipt.get("durable") is not True:
            raise RuntimeError(receipt.get("error") or "The Vita did not confirm a durable transfer")
        return receipt
    finally:
        conn.close()

def error_text(code):
    return ERRORS.get(code, f"Vita import failed (0x{code & 0xffffffff:08X}); the staged file was retained.")
