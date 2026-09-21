"""Content Manager Pro desktop companion."""
from __future__ import annotations

import json
from pathlib import Path
import shutil
import tempfile
import threading
import time
import uuid

from media import inspect_media, prepare
from subtitles import external_track
import vcm_usb as usb
from wifi import address, status, upload, metadata, publish, request, error_text, queue_payload
from wifi import library_page as vita_library_page, photo_thumbnails as vita_photo_thumbnails, download_media

class DesktopApi:
    def __init__(self):
        # The bridge recursively scans public fields. Never expose its Window.
        self._window = None
        self._temporary = tempfile.TemporaryDirectory(prefix="vita-content-manager-")
        self._conversion_lock = threading.Lock()
        self._transfer_lock = threading.Lock()
        self._closing = threading.Event()
        self._conversion_cancel = threading.Event()
        self._transfer_cancel = threading.Event()

    def _shutdown(self):
        self._closing.set()
        self._conversion_cancel.set()
        self._transfer_cancel.set()
        with self._conversion_lock:
            with self._transfer_lock:
                self._temporary.cleanup()

    def choose_files(self):
        import webview
        paths = self._window.create_file_dialog(webview.FileDialog.OPEN, allow_multiple=True,
                                                file_types=("All files (*.*)",))
        return self.describe_paths(paths or ())

    def describe_paths(self, paths):
        items = []
        for raw in paths:
            path = Path(raw).expanduser().resolve()
            try:
                items.append(inspect_media(path))
            except Exception as error:
                items.append(dict(path=str(path), name=path.name, kind="unknown",
                                  format=path.suffix.lstrip(".").upper() or "Unknown",
                                  size=path.stat().st_size if path.is_file() else 0,
                                  eligible=False, reasons=[str(error)], notes=[], defaults=None))
        return items

    def describe_video_folder(self, raw):
        folder = Path(raw).expanduser().resolve()
        if not folder.is_dir():
            raise ValueError("Choose a folder of videos")
        items = self.describe_paths(sorted((p for p in folder.iterdir() if p.is_file()), key=lambda p: p.name.casefold()))
        return [dict(item, video_folder=folder.name) for item in items if item['kind'] == 'video']

    def choose_video_folder(self):
        import webview
        paths = self._window.create_file_dialog(webview.FileDialog.FOLDER)
        return self.describe_video_folder(paths[0]) if paths else []

    def choose_subtitle(self):
        import webview
        paths = self._window.create_file_dialog(webview.FileDialog.OPEN, allow_multiple=False,
                                                file_types=("Subtitles (*.srt;*.ass;*.ssa;*.vtt;*.m4t)",))
        return external_track(paths[0]) if paths else None

    def convert_files(self, paths, options=None):
        results = []
        with self._conversion_lock:
            if not self._closing.is_set():
                self._conversion_cancel.clear()
            for raw in paths:
                source = Path(raw).expanduser().resolve()
                if self._closing.is_set():
                    results.append(dict(source=str(source), error="Application closing", ok=False))
                    continue
                folder = Path(tempfile.mkdtemp(dir=self._temporary.name, prefix="item-"))
                try:
                    def progress(update):
                        if self._window is not None and not self._closing.is_set():
                            self._window.evaluate_js("updateConversionProgress(" + json.dumps(dict(update, source=str(source))) + ")")
                    target, item = prepare(source, folder, options, self._conversion_cancel, progress)
                    results.append(dict(source=str(source), output=str(target), item=item, ok=True))
                except Exception as error:
                    shutil.rmtree(folder)
                    results.append(dict(source=str(source), error=str(error), ok=False,
                                        cancelled=self._conversion_cancel.is_set()))
        return results

    def stop_conversion(self):
        self._conversion_cancel.set()

    def _install_drop_handler(self):
        from webview.dom import DOMEventHandler

        def dropped(event):
            if self._closing.is_set():
                return
            if not self._window.evaluate_js("acknowledgeDrop()"):
                return
            try:
                files = event.get("dataTransfer", {}).get("files", [])
                paths = [item.get("pywebviewFullPath") for item in files]
                if not files or not all(paths):
                    raise ValueError("Could not read the dropped file paths. Drop files from your file manager, or use Add files.")
                items = self.describe_paths(paths)
                self._window.evaluate_js("finishDrop(" + json.dumps(items) + ", null)")
            except Exception as error:
                if not self._closing.is_set():
                    self._window.evaluate_js("finishDrop([], " + json.dumps(str(error)) + ")")
        try:
            self._window.dom.get_element("#dropzone").on("drop", DOMEventHandler(dropped, prevent_default=True))
            self._window.evaluate_js("dropReady = true")
        except Exception as error:
            self._window.evaluate_js("setStatus(" + json.dumps("Drag and drop unavailable: " + str(error)) + ")")

    def probe_vita(self, address):
        if usb.available():
            try:
                usb.library_page("photo", 0, 1)
                return dict(online=True, message="PS Vita USB media store is connected", transport="usb")
            except usb.UsbUnavailable:
                pass
        try:
            info = status(address)
            return dict(online=True, message="Content Manager Pro is ready to sync")
        except (ValueError, RuntimeError, OSError) as error:
            return dict(online=False, message=str(error))

    def cancel_transfer(self):
        self._transfer_cancel.set()

    def _connection_succeeded(self, ip):
        if self._window is not None and not self._closing.is_set():
            self._window.evaluate_js("noteVitaConnection(" + json.dumps(ip) + ")")

    def library_page(self, vita_ip, kind, offset=0):
        if usb.available():
            try:
                return usb.library_page(kind, int(offset))
            except usb.UsbUnavailable:
                pass
        ip = address(vita_ip)
        page = vita_library_page(ip, kind, int(offset))
        self._connection_succeeded(ip)
        return dict(page, transport="wifi")

    def photo_thumbnails(self, vita_ip, offset=0):
        if usb.available():
            try:
                return usb.photo_thumbnails(int(offset))
            except usb.UsbUnavailable:
                pass
        ip = address(vita_ip)
        thumbs = vita_photo_thumbnails(ip, int(offset))
        if thumbs:
            self._connection_succeeded(ip)
        return thumbs

    @staticmethod
    def _safe_download_name(raw):
        import re
        name = str(raw).replace("/", "_").replace("\\", "_")
        name = re.sub(r'[<>:"|?*\x00-\x1f]', "_", name).strip(" .")
        if not name or name.split(".")[0].upper() in {
            "CON", "PRN", "AUX", "NUL", *(f"COM{x}" for x in range(1, 10)),
            *(f"LPT{x}" for x in range(1, 10))}:
            name = "Vita media" + Path(name).suffix
        return name[:220]

    def _download_progress(self, update):
        if self._window is not None and not self._closing.is_set():
            self._window.evaluate_js("updateDownloadProgress(" + json.dumps(update) + ")")

    def copy_from_vita(self, vita_ip, kind, selected, transport="wifi"):
        import webview
        if kind not in ("photo", "music", "video") or not selected:
            raise ValueError("Select media to copy")
        # The OS folder picker is deliberately opened only after Copy is pressed.
        destinations = self._window.create_file_dialog(webview.FileDialog.FOLDER)
        if not destinations:
            return {"cancelled": True, "copied": [], "failed": []}
        destination = Path(destinations[0]).expanduser().resolve()
        if not destination.is_dir():
            raise ValueError("Choose an existing destination folder")
        if transport not in ("usb", "wifi"):
            raise ValueError("Invalid transfer transport")
        if transport == "usb":
            if not usb.available():
                raise usb.UsbUnavailable("The Vita USB media store is disconnected")
        else:
            ip = address(vita_ip)
            service = status(ip)
            self._connection_succeeded(ip)
            if not service.get("library_export"):
                raise RuntimeError("Update the Vita app to copy its media")
        with self._transfer_lock:
            self._transfer_cancel.clear()
            copied, failed = [], []
            expected_total = sum(max(0, int(item.get("size", 0))) +
                                 (max(0, int(item.get("sidecar_size", 0))) if kind == "video" else 0)
                                 for item in selected)
            completed = 0
            for index, item in enumerate(selected):
                if self._transfer_cancel.is_set() or self._closing.is_set():
                    raise RuntimeError("Transfer cancelled")
                identifier = str(item.get("id", ""))
                base = self._safe_download_name(item.get("name", ""))
                stem, suffix = Path(base).stem, Path(base).suffix
                target = destination / base
                counter = 2
                has_sidecar = kind == "video" and int(item.get("sidecar_size", 0)) > 0
                while target.exists() or (has_sidecar and target.with_suffix(".m4t").exists()):
                    target = destination / f"{stem} ({counter}){suffix}"
                    counter += 1
                try:
                    def progress(got, _size):
                        self._download_progress({"name": base, "transferred": completed + got,
                                                 "total": expected_total, "copied": len(copied),
                                                 "count": len(selected)})
                    if transport == "usb":
                        size = usb.copy_object(identifier, target, cancel=self._transfer_cancel,
                                               progress=lambda got: progress(got, item.get("size", 0)))
                    else:
                        size = download_media(ip, kind, identifier, target,
                                              cancel=self._transfer_cancel, progress=progress)
                    sidecar_size = 0
                    if has_sidecar:
                        def sidecar_progress(got, _size):
                            self._download_progress({"name": target.with_suffix('.m4t').name,
                                "transferred": completed + size + got, "total": expected_total,
                                "copied": len(copied), "count": len(selected)})
                        if transport == "usb":
                            sidecar_id = str(item.get("sidecar_id", ""))
                            if not sidecar_id:
                                raise RuntimeError("The video subtitle sidecar is missing from USB")
                            sidecar_size = usb.copy_object(sidecar_id, target.with_suffix(".m4t"),
                                cancel=self._transfer_cancel,
                                progress=lambda got: sidecar_progress(got, item.get("sidecar_size", 0)))
                        else:
                            sidecar_size = download_media(ip, kind, identifier, target.with_suffix(".m4t"),
                                cancel=self._transfer_cancel, progress=sidecar_progress, sidecar=True)
                    copied.append({"id": identifier, "path": str(target), "size": size,
                                   "sidecar": str(target.with_suffix('.m4t')) if has_sidecar else None,
                                   "sidecar_size": sidecar_size})
                except Exception as error:
                    failed.append({"id": identifier, "name": base, "error": str(error),
                                   "partial_video": str(target) if has_sidecar and target.exists() else None})
                    if self._transfer_cancel.is_set():
                        break
                completed += (max(0, int(item.get("size", 0))) +
                              (max(0, int(item.get("sidecar_size", 0))) if kind == "video" else 0))
                self._download_progress({"name": base, "transferred": completed,
                                         "total": expected_total, "copied": len(copied),
                                         "count": len(selected)})
            return {"cancelled": self._transfer_cancel.is_set(), "copied": copied, "failed": failed}

    def _sync_progress(self, update):
        if self._window is not None and not self._closing.is_set():
            self._window.evaluate_js("updateSyncProgress(" + json.dumps(update) + ")")

    def send_files(self, vita_ip, items):
        with self._transfer_lock:
            self._transfer_cancel.clear()
            prepared = []
            for item in items:
                path = Path(item["path"]).expanduser().resolve()
                if self._closing.is_set():
                    raise RuntimeError("Application closing")
                checked = inspect_media(path)
                if not checked["eligible"]:
                    raise ValueError(f"Convert {path.name} to a Vita-compatible format first")
                sidecars = [Path(part["path"]).resolve() for part in item.get("sidecars", [])]
                if len(sidecars) > 1 or any(part != path.with_suffix(".m4t") or not part.is_file() for part in sidecars):
                    raise ValueError("The subtitle sidecar must exist and match the video file name")
                if sidecars and checked["kind"] != "video":
                    raise ValueError("Only videos can have subtitle sidecars")
                checked['video_folder'] = item.get('video_folder', '') if checked['kind'] == 'video' else ''
                prepared.append((path, sidecars, metadata(checked, sidecars[0] if sidecars else None)))
            queue = uuid.uuid4().hex
            use_usb = usb.available()
            ip = None
            if use_usb:
                try:
                    usb.publish_queue(queue_payload(queue,[item[2] for item in prepared]))
                except usb.UsbUnavailable:
                    use_usb=False
            if not use_usb:
                ip=address(vita_ip)
                service = status(ip)
                self._connection_succeeded(ip)
                if any(item[2]['folder'] for item in prepared) and not service.get('video_folders'):
                    raise RuntimeError('Update the Vita app to support video folders')
                publish(ip, queue, [item[2] for item in prepared])
            total = sum(item[2]["size"] + item[2]["subtitle_size"] for item in prepared)
            update = dict(phase="transfer", transferred=0, total=total, imported=0, count=len(prepared), items=[])
            try:
                transferred = 0
                for item_index,(path, sidecars, item) in enumerate(prepared):
                    for source, extension in [(path, item["extension"])] + [(part, "m4t") for part in sidecars]:
                        def progress(sent, _size):
                            self._sync_progress(dict(update, transferred=transferred+sent, name=path.name,
                                                     items=[dict(path=str(path), state=1)]))
                        if use_usb:
                            usb.upload_file(item_index, extension=="m4t", source,
                                            cancel=self._transfer_cancel, progress=progress)
                        else:
                            upload(ip, queue, source, item["id"], extension,
                                   cancel=self._transfer_cancel, progress=progress)
                        transferred += source.stat().st_size
                    self._sync_progress(dict(update, transferred=transferred,
                                             items=[dict(path=str(path), state=2)]))
                if use_usb: usb.queue_action("sync")
                else: request(ip, "POST", "sync", queue=queue)
                paths = {item[2]["id"]: str(item[0]) for item in prepared}
                while True:
                    if self._transfer_cancel.is_set():
                        raise RuntimeError("Transfer cancelled")
                    if use_usb:
                        result=usb.queue_status()
                        if result["count"] != len(prepared):
                            raise RuntimeError("The Vita's transfer queue changed")
                        raw_items=[dict(id=prepared[i][2]["id"],**usb.item_status(i))
                                   for i in range(result["count"])]
                        result.update(items=raw_items, transferred=total, total=total)
                    else:
                        result = status(ip)
                        if result["queue"] != queue:
                            raise RuntimeError("The Vita's transfer queue changed")
                    progress_items = [dict(item, path=paths[item["id"]]) for item in result["items"]]
                    self._sync_progress(dict(result, phase="import" if result["syncing"] else "complete", items=progress_items))
                    if result["imported"] + result["failed"] == result["count"] and not result["syncing"]:
                        return [dict(path=paths[item["id"]], ok=item["state"]==4, state="imported" if item["state"]==4 else "failed",
                                     error=error_text(item["error"]) if item["state"]!=4 else "") for item in result["items"]]
                    if result.get("cancelled"):
                        raise RuntimeError("Transfer stopped on the Vita")
                    time.sleep(.25)
            except Exception:
                try:
                    if use_usb: usb.queue_action("cancel")
                    else: request(ip, "POST", "cancel", queue=queue)
                except Exception:
                    pass
                raise


def main():
    from process_lifetime import own_child_processes
    own_child_processes()
    import webview
    api = DesktopApi()
    page = Path(__file__).resolve().parent / "ui" / "index.html"
    api._window = webview.create_window("Vita Content Manager Pro", str(page), js_api=api,
                                       width=1000, height=650, min_size=(720, 460), background_color="#252630")
    api._window.events.closing += api._shutdown
    api._window.events.loaded += api._install_drop_handler
    try:
        webview.start()
    finally:
        api._shutdown()


if __name__ == "__main__":
    main()
