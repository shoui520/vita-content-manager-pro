from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
from urllib.parse import unquote
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from app import DesktopApi
import wifi


class Receiver(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, code, payload):
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        state = self.server.state
        self.reply(200, dict(service='vita-content-manager', version=2, **state))

    def do_POST(self):
        body = self.rfile.read(int(self.headers['Content-Length']))
        state = self.server.state
        if self.path.endswith('/queue'):
            lines = body.decode().splitlines()
            rows = [[unquote(part) for part in line.split('\t')] for line in lines[1:]]
            self.server.rows = rows
            state.update(queue=lines[0], count=len(rows), imported=0, failed=0, syncing=0,
                         total=sum(int(r[2])+int(r[3]) for r in rows), transferred=0,
                         items=[dict(id=r[0], state=0, error=0) for r in rows])
            self.reply(201, dict(ok=True))
        elif self.headers.get('X-VCM-Queue') != state['queue']:
            self.reply(409, dict(error='Wrong queue'))
        elif self.path.endswith('/sync'):
            for item in state['items']:
                item['state'] = 5 if self.server.fail_import else 4
                item['error'] = -1001 if self.server.fail_import else 0
            state['failed' if self.server.fail_import else 'imported'] = state['count']
            self.reply(202, dict(ok=True))
        else:
            state['cancelled'] = 1
            self.reply(200, dict(ok=True))

    def do_PUT(self):
        body = self.rfile.read(int(self.headers['Content-Length']))
        self.server.received.append((self.path, body))
        self.server.state['transferred'] += len(body)
        self.reply(201, dict(ok=True, state='received', durable=not self.server.fail_sync))


class WifiTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Receiver)
        self.server.received = []
        self.server.rows = []
        self.server.fail_import = self.server.fail_sync = False
        self.server.state = dict(queue='', count=0, imported=0, failed=0, syncing=0, cancelled=0,
                                 transferred=0, total=0, items=[])
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.photo = Path(self.temp.name) / '日本語 & photo.jpg'
        Image.new('RGB', (64, 64), 'orange').save(self.photo)
        self.api = DesktopApi()

    def tearDown(self):
        self.api._shutdown()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.temp.cleanup()

    def send(self):
        request = wifi.request
        upload = wifi.upload
        def local_request(*args, **kwargs):
            kwargs['port'] = self.server.server_port
            return request(*args, **kwargs)
        def local_upload(*args, **kwargs):
            kwargs['port'] = self.server.server_port
            return upload(*args, **kwargs)
        with patch('wifi.request', local_request), patch('app.request', local_request), patch('app.upload', local_upload):
            return self.api.send_files('127.0.0.1', [dict(path=str(self.photo))])

    def test_queue_transfer_import_preserves_unicode_and_bytes(self):
        updates = []
        self.api._sync_progress = updates.append
        results = self.send()
        self.assertTrue(results[0]['ok'])
        self.assertEqual(results[0]['state'], 'imported')
        self.assertEqual(self.server.rows[0][5], self.photo.name)
        self.assertEqual(self.server.received[0][1], self.photo.read_bytes())
        self.assertTrue(any(u['phase'] == 'transfer' and u['imported'] == 0 for u in updates))
        self.assertEqual(updates[-1]['imported'], 1)

    def test_received_does_not_mean_imported(self):
        self.server.fail_import = True
        results = self.send()
        self.assertFalse(results[0]['ok'])
        self.assertIn('Close the corresponding', results[0]['error'])

    def test_unconfirmed_storage_sync_stops_before_import(self):
        self.server.fail_sync = True
        with self.assertRaisesRegex(RuntimeError, 'durable transfer'):
            self.send()
        self.assertEqual(self.server.state['imported'], 0)
        self.assertEqual(self.server.state['cancelled'], 1)

    def test_missing_sidecar_fails_before_publishing(self):
        with self.assertRaisesRegex(ValueError, 'sidecar'):
            self.api.send_files('127.0.0.1', [dict(path=str(self.photo), sidecars=[dict(path=str(self.photo.with_suffix('.m4t')))])])
        self.assertEqual(self.server.rows, [])

if __name__ == '__main__':
    unittest.main()
