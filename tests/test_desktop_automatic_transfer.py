import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src" / "desktop"))
import app


def test_copy_starts_upload_without_vita_approval(monkeypatch, tmp_path):
    source = tmp_path / "photo.jpg"
    source.write_bytes(b"jpeg-test")
    calls = []
    queue_id = [None]
    item_id = "a" * 32

    monkeypatch.setattr(app, "inspect_media", lambda path: {
        "path": str(path), "kind": "photo", "format": "JPEG", "eligible": True,
        "size": path.stat().st_size,
    })
    monkeypatch.setattr(app, "metadata", lambda info, sidecar: {
        "id": item_id, "size": source.stat().st_size, "subtitle_size": 0,
        "extension": "jpg", "folder": "",
    })
    monkeypatch.setattr(app, "status", lambda ip: {
        "video_folders": True, "queue": queue_id[0], "count": 1,
        "imported": 1, "failed": 0, "syncing": 0,
        "items": [{"id": item_id, "state": 4, "error": 0}],
    })
    def publish(_ip, queue, _items):
        queue_id[0] = queue
        calls.append("publish")
    monkeypatch.setattr(app, "publish", publish)
    monkeypatch.setattr(app, "upload", lambda *args, **kwargs: calls.append("upload"))
    monkeypatch.setattr(app, "request", lambda *args, **kwargs: calls.append("sync"))

    desktop = app.DesktopApi()
    try:
        # The mocked Vita offers neither queue_approval nor approved.
        # Publishing must lead directly to upload and import.
        results = desktop.send_files("192.168.0.155", [{"path": str(source)}])
    finally:
        desktop._shutdown()
    assert calls == ["publish", "upload", "sync"]
    assert results[0]["ok"] is True


def test_usb_copy_uses_private_queue_without_wifi(monkeypatch, tmp_path):
    source = tmp_path / "photo.jpg"
    source.write_bytes(b"jpeg-test")
    item_id = "b" * 32
    calls = []

    monkeypatch.setattr(app, "inspect_media", lambda path: {
        "path": str(path), "kind": "photo", "format": "JPEG", "eligible": True,
        "size": path.stat().st_size,
    })
    monkeypatch.setattr(app, "metadata", lambda info, sidecar: {
        "id": item_id, "size": source.stat().st_size, "subtitle_size": 0,
        "extension": "jpg", "folder": "",
    })
    monkeypatch.setattr(app, "queue_payload", lambda queue, items: b"private queue")
    monkeypatch.setattr(app.usb, "available", lambda: True)
    monkeypatch.setattr(app.usb, "publish_queue", lambda payload: calls.append(("queue", payload)))
    monkeypatch.setattr(app.usb, "upload_file",
                        lambda index, sidecar, path, **kwargs: calls.append(("upload", index, sidecar, path)))
    monkeypatch.setattr(app.usb, "queue_action", lambda action: calls.append(("action", action)))
    monkeypatch.setattr(app.usb, "queue_status", lambda: {
        "count": 1, "imported": 1, "failed": 0, "syncing": False, "cancelled": False,
    })
    monkeypatch.setattr(app.usb, "item_status", lambda index: {"state": 4, "error": 0})
    monkeypatch.setattr(app, "status", lambda ip: (_ for _ in ()).throw(AssertionError("Wi-Fi used")))

    desktop = app.DesktopApi()
    try:
        results = desktop.send_files("192.168.0.155", [{"path": str(source)}])
    finally:
        desktop._shutdown()

    assert calls == [("queue", b"private queue"),
                     ("upload", 0, False, source.resolve()),
                     ("action", "sync")]
    assert results == [{"path": str(source.resolve()), "ok": True,
                        "state": "imported", "error": ""}]
