from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from app import DesktopApi


class DropTests(unittest.TestCase):
    def setUp(self):
        self.api = DesktopApi()
        self.window = self.api._window = Mock()
        self.element = self.window.dom.get_element.return_value
        self.window.evaluate_js.return_value = True
        module = SimpleNamespace(DOMEventHandler=lambda callback, **kwargs: callback)
        with patch.dict(sys.modules, {'webview.dom': module}):
            self.api._install_drop_handler()
        self.element.on.assert_called_once()
        self.assertEqual(self.element.on.call_args.args[0], 'drop')
        self.callback = self.element.on.call_args.args[1]
        self.window.evaluate_js.reset_mock()

    def tearDown(self):
        self.api._shutdown()

    def test_registration_and_paths(self):
        self.api.describe_paths = Mock(return_value=[{'name': 'audio-input.mp3'}])
        self.callback({'dataTransfer': {'files': [{'pywebviewFullPath': 'audio-input.mp3'}]}})
        self.api.describe_paths.assert_called_once_with(['audio-input.mp3'])
        self.assertIn('finishDrop(', self.window.evaluate_js.call_args.args[0])

    def test_missing_paths_reports_error(self):
        self.callback({'dataTransfer': {'files': [{'name': 'audio-input.mp3'}]}})
        self.assertIn('Could not read the dropped file paths', self.window.evaluate_js.call_args.args[0])

    def test_inspection_error_reports_error(self):
        self.api.describe_paths = Mock(side_effect=RuntimeError('inspection failed'))
        self.callback({'dataTransfer': {'files': [{'pywebviewFullPath': 'audio-input.mp3'}]}})
        self.assertIn('inspection failed', self.window.evaluate_js.call_args.args[0])

    def test_busy_ui_does_not_accept_drop(self):
        self.window.evaluate_js.return_value = False
        self.api.describe_paths = Mock()
        self.callback({'dataTransfer': {'files': [{'pywebviewFullPath': 'audio-input.mp3'}]}})
        self.api.describe_paths.assert_not_called()
