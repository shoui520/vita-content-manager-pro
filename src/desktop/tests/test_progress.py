import sys
import threading
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from media import run_ffmpeg, tool


class ProgressTests(unittest.TestCase):
    def command(self):
        return [tool('ffmpeg'), '-v', 'error', '-re', '-f', 'lavfi', '-i',
                'sine=duration=1', '-f', 'null', '-']

    def test_real_timestamp_progress(self):
        updates = []
        run_ffmpeg(self.command(), 1, progress=updates.append)
        self.assertTrue(any(0 < u['percent'] < 100 for u in updates))
        self.assertEqual(updates[-1]['percent'], 100)
        self.assertTrue(all(u['phase'] == 'Converting' for u in updates))

    def test_unknown_duration(self):
        updates = []
        run_ffmpeg(self.command(), 0, progress=updates.append)
        self.assertIsNone(updates[0]['percent'])
        self.assertEqual(updates[-1]['percent'], 100)

    def test_failure_never_completes(self):
        updates = []
        with self.assertRaises(ValueError):
            run_ffmpeg([tool('ffmpeg'), '-i', '/nonexistent-vcm-progress-input'], 1, progress=updates.append)
        self.assertFalse(any(u['percent'] == 100 for u in updates))

    def test_cancellation(self):
        cancel = threading.Event()
        cancel.set()
        with self.assertRaisesRegex(RuntimeError, 'cancelled'):
            run_ffmpeg(self.command(), 1, cancel)

    def test_stop_during_running_ffmpeg(self):
        cancel = threading.Event()
        def stop(update):
            if update['seconds'] > 0:
                cancel.set()
        with self.assertRaisesRegex(RuntimeError, 'cancelled'):
            run_ffmpeg(self.command(), 1, cancel, stop)
