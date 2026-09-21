import io
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import sys
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import wifi
import app
import vcm_usb
from app import DesktopApi


class LibraryReceiver(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        if self.path == '/v2/status':
            data = json.dumps({'service': 'vita-content-manager', 'version': 2,
                               'library_export': True}).encode()
            content_type = 'application/json'
        elif self.path == '/v2/library/photo?offset=0&limit=32':
            data = json.dumps({'items': [{'id': '144115192370823322', 'title': 'Photo',
                                           'name': 'photo-input.jpg', 'size': 9, 'thumbnail': True}],
                               'next_offset': -1}).encode()
            content_type = 'application/json'
        elif self.path == '/v2/library/photo/thumbs?offset=0&limit=32':
            stream = io.BytesIO()
            Image.new('RGB', (8, 8), 'red').save(stream, format='DDS')
            icon = stream.getvalue()
            data = (144115192370823322).to_bytes(8, 'little') + len(icon).to_bytes(4, 'little') + icon
            content_type = 'application/octet-stream'
        elif self.path == '/v2/library/photo/144115192370823322':
            data = b'photo-data'
            content_type = 'application/octet-stream'
        elif self.path == '/v2/library/video/44':
            data = b'video-data'
            content_type = 'application/octet-stream'
        elif self.path == '/v2/library/video/44/m4t':
            data = b'm4t-data'
            content_type = 'application/octet-stream'
        elif self.path == '/v2/library/video/45':
            data = b'video-without-sidecar'
            content_type = 'application/octet-stream'
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)


class LibraryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), LibraryReceiver)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.temp.cleanup()

    def test_catalog_and_gallery_icon_are_small_separate_requests(self):
        page = wifi.library_page('127.0.0.1', 'photo', port=self.server.server_port)
        self.assertEqual(page['items'][0]['id'], '144115192370823322')
        thumbs = wifi.photo_thumbnails('127.0.0.1', 0, port=self.server.server_port)
        self.assertTrue(thumbs[page['items'][0]['id']].startswith('data:image/png;base64,'))

    def test_usb_library_preserves_titles_tags_dates_and_photo_order(self):
        objects = [
            {'id': 'p', 'parent': 'root', 'name': 'Photo', 'folder': True},
            {'id': 'm', 'parent': 'root', 'name': 'Music', 'folder': True},
            {'id': 'v', 'parent': 'root', 'name': 'Video', 'folder': True},
            {'id': 'o4', 'parent': 'p', 'name': 'old.jpg', 'title': 'Old', 'size': 10,
             'folder': False, 'created': '2025-01-01T01:00:00', 'modified': '',
             'width': 128, 'height': 64},
            {'id': 'o5', 'parent': 'p', 'name': 'new.jpg', 'title': 'New', 'size': 20,
             'folder': False, 'created': '2026-01-01T01:00:00', 'modified': '',
             'width': 64, 'height': 128},
            {'id': 'o6', 'parent': 'm', 'name': 'nice.mp3', 'title': 'Nice', 'size': 30,
             'folder': False, 'artist': 'Artist', 'album': 'Album',
             'created': '2026-02-03T04:05:06', 'modified': '2026-02-04T04:05:06'},
        ]
        with patch.object(vcm_usb, 'list_objects', return_value=objects):
            photos = vcm_usb.library_page('photo')['items']
            music = vcm_usb.library_page('music')['items'][0]
        self.assertEqual([item['id'] for item in photos], ['o5', 'o4'])
        self.assertTrue(photos[0]['thumbnail'])
        self.assertEqual((photos[0]['width'], photos[0]['height']), (64, 128))
        self.assertEqual((music['title'], music['artist'], music['album']),
                         ('Nice', 'Artist', 'Album'))
        self.assertEqual(music['modified'], '2026-02-04T04:05:06')

    def test_usb_thumbnail_batch_uses_cached_small_images(self):
        bitmap = io.BytesIO()
        Image.new('RGB', (8, 4), 'green').save(bitmap, format='BMP')
        payload = (1).to_bytes(4, 'little') + len(bitmap.getvalue()).to_bytes(4, 'little') + bitmap.getvalue()
        page = {'items': [{'id': 'o4'}]}
        with patch.object(vcm_usb, 'library_page', return_value=page), \
             patch.object(vcm_usb.subprocess, 'run', return_value=SimpleNamespace(
                 returncode=0, stdout=payload, stderr=b'')) as run:
            thumbs = vcm_usb.photo_thumbnails()
        self.assertEqual(run.call_args.args[0][1:], ['thumbs', 'o4'])
        self.assertTrue(thumbs['o4'].startswith('data:image/png;base64,'))

    def test_download_preserves_existing_file_and_publishes_complete_copy(self):
        folder = Path(self.temp.name)
        original = folder / 'photo-input.jpg'
        original.write_bytes(b'original')
        target = folder / 'photo-input (2).jpg'
        got = wifi.download_media('127.0.0.1', 'photo', '144115192370823322', target,
                                  port=self.server.server_port)
        self.assertEqual(got, len(b'photo-data'))
        self.assertEqual(original.read_bytes(), b'original')
        self.assertEqual(target.read_bytes(), b'photo-data')
        self.assertEqual(list(folder.glob('*.vcm-part')), [])

    def test_bad_filename_is_sanitized(self):
        self.assertEqual(DesktopApi._safe_download_name('bad:name?.jpg'), 'bad_name_.jpg')
        self.assertFalse('/' in DesktopApi._safe_download_name('../bad.jpg'))

    def test_successful_library_request_updates_connection_indicator(self):
        calls = []
        api = DesktopApi()
        api._window = SimpleNamespace(evaluate_js=calls.append)
        try:
            with patch.object(app, 'vita_library_page', return_value={'items': [], 'next_offset': -1}):
                self.assertEqual(api.library_page('127.0.0.1', 'photo')['items'], [])
        finally:
            api._shutdown()
        self.assertEqual(calls, ['noteVitaConnection("127.0.0.1")'])

    def test_video_sidecar_endpoint_and_optional_copy(self):
        folder = Path(self.temp.name)
        (folder / 'Episode.m4t').write_bytes(b'existing-subtitle')
        api = DesktopApi()
        api._window = SimpleNamespace(create_file_dialog=lambda *_: [str(folder)],
                                      evaluate_js=lambda *_: None)
        local_download = lambda *args, **kwargs: wifi.download_media(
            *args, **dict(kwargs, port=self.server.server_port))
        try:
            with patch.dict(sys.modules, {'webview': SimpleNamespace(FileDialog=SimpleNamespace(FOLDER='folder'))}), \
                 patch.object(app, 'status', lambda ip: {'library_export': True}), \
                 patch.object(app, 'download_media', local_download):
                result = api.copy_from_vita('127.0.0.1', 'video', [
                    {'id': '44', 'name': 'Episode.mp4', 'size': 10, 'sidecar_size': 8},
                    {'id': '45', 'name': 'Silent.mp4', 'size': 21, 'sidecar_size': 0}])
        finally:
            api._shutdown()
        self.assertEqual(result['failed'], [])
        self.assertEqual((folder / 'Episode.m4t').read_bytes(), b'existing-subtitle')
        self.assertEqual((folder / 'Episode (2).mp4').read_bytes(), b'video-data')
        self.assertEqual((folder / 'Episode (2).m4t').read_bytes(), b'm4t-data')
        self.assertEqual((folder / 'Silent.mp4').read_bytes(), b'video-without-sidecar')
        self.assertFalse((folder / 'Silent.m4t').exists())

    def test_copy_button_path_uses_folder_picker_and_preserves_existing(self):
        folder = Path(self.temp.name)
        (folder / 'photo-input.jpg').write_bytes(b'original')
        api = DesktopApi()
        api._window = SimpleNamespace(create_file_dialog=lambda *_: [str(folder)],
                                      evaluate_js=lambda *_: None)
        local_status = lambda ip: {'library_export': True}
        local_download = lambda *args, **kwargs: wifi.download_media(
            *args, **dict(kwargs, port=self.server.server_port))
        try:
            with patch.dict(sys.modules, {'webview': SimpleNamespace(FileDialog=SimpleNamespace(FOLDER='folder'))}), \
                 patch.object(app, 'status', local_status), patch.object(app, 'download_media', local_download):
                result = api.copy_from_vita('127.0.0.1', 'photo',
                    [{'id': '144115192370823322', 'name': 'photo-input.jpg', 'size': 10}])
        finally:
            api._shutdown()
        self.assertEqual(len(result['copied']), 1)
        self.assertEqual((folder / 'photo-input.jpg').read_bytes(), b'original')
        self.assertEqual((folder / 'photo-input (2).jpg').read_bytes(), b'photo-data')


if __name__ == '__main__':
    unittest.main()
