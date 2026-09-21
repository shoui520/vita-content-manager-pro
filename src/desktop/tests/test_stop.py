import sys
import threading
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from app import DesktopApi


class StopTests(unittest.TestCase):
    def test_stop_cleans_partial_and_next_conversion_can_start(self):
        api = DesktopApi()
        started = threading.Event()
        results = []
        def converting(source, folder, options, cancel, progress):
            (folder / 'partial.mp4').write_bytes(b'partial')
            started.set()
            self.assertTrue(cancel.wait(3))
            raise RuntimeError('Operation cancelled')
        try:
            with patch('app.prepare', converting):
                worker = threading.Thread(target=lambda: results.extend(api.convert_files(['source.mp4'])))
                worker.start()
                self.assertTrue(started.wait(3))
                api.stop_conversion()
                worker.join(3)
                self.assertFalse(worker.is_alive())
            self.assertTrue(results[0]['cancelled'])
            self.assertEqual(list(Path(api._temporary.name).iterdir()), [])
            with patch('app.prepare', return_value=(Path('output.mp4'), {})):
                self.assertTrue(api.convert_files(['source.mp4'])[0]['ok'])
                self.assertFalse(api._conversion_cancel.is_set())
        finally:
            api._shutdown()
