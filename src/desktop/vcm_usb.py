"""Windows Portable Devices transport for VCM's curated USB media store.

The helper opens only the Vita's "Virtual Media Library" WPD device. It never
uses VitaShell mass storage, libusb, a driver replacement, or direct ux0 paths.
"""
from __future__ import annotations

import json
import base64
import io
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid
import tempfile
import queue as thread_queue
import threading
from PIL import Image


class UsbUnavailable(RuntimeError):
    pass


def helper_path() -> Path:
    override = os.environ.get("VCM_WPD_HELPER")
    return Path(override) if override else Path(__file__).resolve().parent / "usb_wpd" / "vcm-wpd.exe"


def available() -> bool:
    return sys.platform == "win32" and helper_path().is_file()


def _flags() -> int:
    return getattr(subprocess, "CREATE_NO_WINDOW", 0)


def _error(result: subprocess.CompletedProcess[str]) -> UsbUnavailable:
    return UsbUnavailable((result.stderr or "The Vita USB media store is unavailable.").strip())


def _terminate(process: subprocess.Popen) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def list_objects() -> list[dict]:
    """Return WPD IDs and display properties; no AVContent database parsing."""
    if not available():
        raise UsbUnavailable("The Windows USB helper is not installed.")
    try:
        result = subprocess.run([str(helper_path()), "list"], capture_output=True,
                                text=True, encoding="utf-8", timeout=30,
                                creationflags=_flags(), check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise UsbUnavailable(str(error)) from error
    if result.returncode:
        raise _error(result)
    try:
        entries = json.loads(result.stdout)["items"]
        if not isinstance(entries, list) or len(entries) > 100000:
            raise ValueError("Invalid USB catalogue")
        for item in entries:
            if not isinstance(item, dict) or not isinstance(item.get("id"), str) or \
                    not isinstance(item.get("name"), str) or not isinstance(item.get("size"), int):
                raise ValueError("Invalid USB catalogue item")
        return entries
    except (ValueError, KeyError, TypeError) as error:
        raise UsbUnavailable("The USB helper returned an invalid catalogue") from error


def library_page(kind: str, offset: int = 0, limit: int = 32) -> dict:
    """Present the Vita-owned virtual store in the desktop library shape."""
    if kind not in ("photo", "music", "video") or offset < 0 or not 1 <= limit <= 32:
        raise ValueError("Invalid USB library page")
    objects = list_objects()
    folders = {item["name"]: item["id"] for item in objects if item["folder"]}
    parent = folders.get(kind.title())
    if parent is None:
        raise UsbUnavailable("The Vita USB media folders are not available")
    children = [item for item in objects if item["parent"] == parent and not item["folder"]]
    sidecars = {Path(item["name"]).stem.casefold(): item for item in children
                if item["name"].lower().endswith(".m4t")} if kind == "video" else {}
    if kind == "video":
        children = [item for item in children if not item["name"].lower().endswith(".m4t")]
    children.sort(key=(lambda item: (item.get("created") or "", item["name"].casefold()))
                  if kind == "photo" else (lambda item: item.get("title", item["name"]).casefold()),
                  reverse=kind == "photo")
    page = []
    for item in children[offset:offset + limit]:
        companion = sidecars.get(Path(item["name"]).stem.casefold())
        page.append(dict(id=item["id"], name=item["name"], title=item.get("title") or item["name"],
                         size=item["size"], sidecar_size=companion["size"] if companion else 0,
                         sidecar_id=companion["id"] if companion else "", thumbnail=kind == "photo",
                         artist=item.get("artist", ""), album=item.get("album", ""),
                         created=item.get("created", ""), modified=item.get("modified", ""),
                         width=item.get("width", 0), height=item.get("height", 0)))
    next_offset = offset + len(page)
    return dict(items=page, next_offset=next_offset if next_offset < len(children) else -1,
                transport="usb")


def photo_thumbnails(offset: int = 0, limit: int = 32) -> dict[str, str]:
    """Read only cached Vita thumbnails, in one WPD session for the page."""
    page = library_page("photo", offset, limit)
    ids = [item["id"] for item in page["items"]]
    if not ids:
        return {}
    try:
        result = subprocess.run([str(helper_path()), "thumbs", *ids], capture_output=True,
                                timeout=90, creationflags=_flags(), check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise UsbUnavailable(str(error)) from error
    if result.returncode:
        raise UsbUnavailable(result.stderr.decode("utf-8", "replace").strip() or
                             "USB thumbnail request failed")
    data = result.stdout
    if len(data) < 4 or int.from_bytes(data[:4], "little") != len(ids):
        raise UsbUnavailable("Invalid USB thumbnail response")
    at = 4
    thumbnails = {}
    for identifier in ids:
        if at + 4 > len(data):
            raise UsbUnavailable("Incomplete USB thumbnail response")
        size = int.from_bytes(data[at:at + 4], "little")
        at += 4
        if size > 256 * 256 * 3 + 54 or at + size > len(data):
            raise UsbUnavailable("Invalid USB thumbnail size")
        if size:
            with Image.open(io.BytesIO(data[at:at + size])) as image:
                if image.format != "BMP" or image.width > 256 or image.height > 256:
                    raise UsbUnavailable("Invalid USB photo thumbnail")
                output = io.BytesIO()
                image.convert("RGB").save(output, format="PNG", optimize=True)
                thumbnails[identifier] = ("data:image/png;base64," +
                                          base64.b64encode(output.getvalue()).decode("ascii"))
        at += size
    if at != len(data):
        raise UsbUnavailable("Unexpected USB thumbnail data")
    return thumbnails


def copy_object(object_id: str, destination: Path, cancel=None, progress=None) -> int:
    """Copy one read-only WPD object to a new file; remove only our partial on failure."""
    if not available():
        raise UsbUnavailable("The Windows USB helper is not installed.")
    if not object_id or len(object_id) > 256 or any(ord(c) < 32 for c in object_id):
        raise ValueError("Invalid USB object ID")
    destination = Path(destination)
    if destination.exists():
        raise FileExistsError(destination)
    if not destination.parent.is_dir():
        raise FileNotFoundError(destination.parent)
    partial = destination.with_name(destination.name + "." + uuid.uuid4().hex + ".vcm-part")
    process = subprocess.Popen([str(helper_path()), "copy", object_id, str(partial)],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, encoding="utf-8", creationflags=_flags())
    try:
        while process.poll() is None:
            if cancel is not None and cancel.is_set():
                _terminate(process)
                raise RuntimeError("USB transfer cancelled")
            if progress is not None:
                progress(partial.stat().st_size if partial.exists() else 0)
            time.sleep(0.1)
        out, err = process.communicate(timeout=5)
        if process.returncode:
            raise UsbUnavailable(err.strip() or "USB copy failed")
        size = int(json.loads(out)["bytes"])
        if size < 0 or not partial.is_file() or partial.stat().st_size != size:
            raise UsbUnavailable("USB copy size mismatch")
        partial.rename(destination)  # Windows rename fails if the destination appeared meanwhile.
        if progress is not None:
            progress(size)
        return size
    except Exception:
        if process.poll() is None:
            _terminate(process)
        if partial.exists():
            partial.unlink()
        raise


def _vendor_json(arguments: list[str], timeout: int = 30) -> dict:
    if not available():
        raise UsbUnavailable("The Windows USB helper is not installed.")
    try:
        result=subprocess.run([str(helper_path()),*arguments],capture_output=True,text=True,
                              encoding="utf-8",timeout=timeout,creationflags=_flags(),check=False)
    except (OSError,subprocess.TimeoutExpired) as error:
        raise UsbUnavailable(str(error)) from error
    if result.returncode:
        raise _error(result)
    try:
        return json.loads(result.stdout) if result.stdout.strip() else {}
    except ValueError as error:
        raise UsbUnavailable("The USB helper returned invalid transfer status") from error


def publish_queue(payload: bytes) -> None:
    if not payload or len(payload)>512*1024:
        raise ValueError("Invalid USB queue metadata")
    handle,path=tempfile.mkstemp(prefix="vcm-usb-queue-",suffix=".txt")
    try:
        with os.fdopen(handle,"wb") as output:
            output.write(payload); output.flush(); os.fsync(output.fileno())
        _vendor_json(["queue",path],60)
    finally:
        Path(path).unlink(missing_ok=True)


def upload_file(index: int, sidecar: bool, path: Path, cancel=None, progress=None) -> int:
    if not 0<=index<128: raise ValueError("Invalid queue index")
    path=Path(path); total=path.stat().st_size
    process=subprocess.Popen([str(helper_path()),"upload",str(index),"1" if sidecar else "0",str(path)],
        stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding="utf-8",
        creationflags=_flags())
    try:
        assert process.stdout is not None
        lines=thread_queue.Queue()
        def read_progress():
            for value in process.stdout: lines.put(value)
            lines.put(None)
        threading.Thread(target=read_progress,daemon=True).start()
        while True:
            if cancel is not None and cancel.is_set():
                _terminate(process); raise RuntimeError("USB transfer cancelled")
            try: line=lines.get(timeout=.1)
            except thread_queue.Empty:
                if process.poll() is not None: break
                continue
            if line is not None:
                try: sent=int(json.loads(line)["sent"])
                except (ValueError,KeyError,TypeError) as error:
                    raise UsbUnavailable("Invalid USB upload progress") from error
                if progress: progress(sent,total)
            elif process.poll() is not None: break
        error=process.stderr.read() if process.stderr else ""
        if process.returncode: raise UsbUnavailable(error.strip() or "USB upload failed")
        if progress: progress(total,total)
        return total
    finally:
        _terminate(process)


def queue_action(action: str) -> None:
    if action not in ("sync","cancel"): raise ValueError("Invalid USB queue action")
    _vendor_json([action])


def queue_status() -> dict:
    values=_vendor_json(["status"]).get("values")
    if not isinstance(values,list) or len(values)!=5: raise UsbUnavailable("Invalid USB queue status")
    return dict(count=values[0],imported=values[1],failed=values[2],syncing=bool(values[3]),cancelled=bool(values[4]))


def item_status(index: int) -> dict:
    values=_vendor_json(["item",str(index)]).get("values")
    if not isinstance(values,list) or len(values)!=2: raise UsbUnavailable("Invalid USB item status")
    error=values[1]-(1<<32) if values[1]>=1<<31 else values[1]
    return dict(state=values[0],error=error)
