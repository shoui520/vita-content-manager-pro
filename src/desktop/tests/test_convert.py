import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from app import DesktopApi
from media import inspect_media, prepare, settings, command_for


class ConversionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def ffmpeg(self, *args):
        subprocess.run(['ffmpeg', '-v', 'error', '-nostdin', '-y', *map(str, args)], check=True)

    def audio(self, suffix, codec):
        path = self.root / ('song.' + suffix)
        self.ffmpeg('-f', 'lavfi', '-i', 'sine=frequency=440:duration=0.3', '-c:a', codec,
                    '-metadata', 'title=日本語の曲', path)
        return path

    def output(self, name):
        path = self.root / name
        path.mkdir()
        return path

    def test_audio_outputs_and_metadata(self):
        source = self.audio('flac', 'flac')
        self.assertFalse(inspect_media(source)['eligible'])
        for name, output, extra in [('cbr', 'mp3', {'bitrate': 128}), ('vbr', 'mp3', {'mode': 'vbr', 'vbr': 4}), ('aac', 'aac', {'bitrate': 96, 'sample_rate': 48000}), ('wav', 'wav', {'sample_rate': 44100})]:
            with self.subTest(name=name):
                target, result = prepare(source, self.output(name), dict(output=output, keep_compatible=False, **extra))
                self.assertTrue(result['eligible'])
                if output == 'aac':
                    self.assertEqual(result['format'], 'AAC')
                data = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-show_format', '-of', 'json', str(target)]))
                self.assertEqual(data['format']['tags']['title'], '日本語の曲')

    def test_opus_and_unknown_extension_are_detected(self):
        source = self.audio('opus', 'libopus')
        renamed = source.with_suffix('.unusual')
        source.rename(renamed)
        info = inspect_media(renamed)
        self.assertEqual(info['kind'], 'music')
        self.assertEqual(info['codec'], 'opus')
        self.assertEqual(info['format'], 'Opus')
        _, result = prepare(renamed, self.output('mp3'))
        self.assertTrue(result['eligible'])

    def test_vorbis_and_webm_are_labeled_by_content(self):
        vorbis = self.audio('ogg', 'libvorbis')
        self.assertEqual(inspect_media(vorbis)['format'], 'Vorbis')

        video = self.root / 'clip.webm'
        self.ffmpeg('-f', 'lavfi', '-i', 'testsrc2=size=64x64:rate=10:duration=0.3',
                    '-f', 'lavfi', '-i', 'sine=frequency=440:duration=0.3',
                    '-c:v', 'libvpx-vp9', '-c:a', 'libopus', '-shortest', video)
        renamed = video.with_suffix('.unknown')
        video.rename(renamed)
        info = inspect_media(renamed)
        self.assertEqual((info['kind'], info['format'], info['codec']), ('video', 'WebM', 'vp9'))
        self.assertEqual(info['defaults']['output'], 'mp4')

        audio = self.root / 'sound.webm'
        self.ffmpeg('-f', 'lavfi', '-i', 'sine=frequency=440:duration=0.3',
                    '-c:a', 'libopus', audio)
        info = inspect_media(audio)
        self.assertEqual((info['kind'], info['format'], info['codec']), ('music', 'Opus', 'opus'))
        self.assertEqual(info['defaults']['output'], 'mp3')
        _, result = prepare(audio, self.output('webm-audio'))
        self.assertTrue(result['eligible'])

    def test_mkv_video_conversion(self):
        source = self.root / 'clip.mkv'
        self.ffmpeg('-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=24:duration=0.3', '-c:v', 'mpeg4', source)
        info = inspect_media(source)
        self.assertEqual(info['format'], 'MKV')
        self.assertFalse(info['eligible'])
        for fit in ['contain', 'pad', 'stretch', 'width', 'height']:
            with self.subTest(fit=fit):
                _, result = prepare(source, self.output(fit), {'fit': fit, 'preset': 'fast'})
                self.assertTrue(result['eligible'])
                self.assertLessEqual(result['width'], 960)
                self.assertLessEqual(result['height'], 544)
                if fit == 'contain':
                    self.assertEqual((result['width'], result['height']), (320, 180))

    def test_gif_becomes_video_and_repeats(self):
        source = self.root / 'animation.gif'
        frames = [Image.new('RGB', (80, 60), color) for color in ['red', 'blue', 'green']]
        frames[0].save(source, save_all=True, append_images=frames[1:], duration=100, loop=0)
        info = inspect_media(source)
        self.assertEqual(info['format'], 'GIF')
        self.assertFalse(info['eligible'])
        _, result = prepare(source, self.output('gif-video'), {'loops': 3})
        self.assertTrue(result['eligible'])
        self.assertGreater(result['duration'], 0.7)

    def test_photo_limits_and_outputs(self):
        source = self.root / 'picture.png'
        Image.new('RGBA', (1600, 1400), (100, 120, 140, 100)).save(source)
        self.assertFalse(inspect_media(source)['eligible'])
        for fmt in ['jpeg', 'png', 'bmp', 'tiff']:
            _, result = prepare(source, self.output(fmt), {'output': fmt, 'keep_compatible': False})
            self.assertTrue(result['eligible'])
            if fmt == 'jpeg':
                self.assertEqual((result['width'], result['height']), (1600, 1400))

    def test_1080p60_rejected_and_defaults_fixed(self):
        info = {'kind': 'video', 'format': 'MKV', 'video_index': 0, 'audio_tracks': []}
        with self.assertRaises(ValueError):
            settings(info, {'resolution': '1080', 'fps': '60'})
        opts = settings(info, None)
        command = command_for(Path('input.mkv'), Path('output.mp4'), info, opts)
        self.assertIn('slow', command)
        self.assertIn('yuv420p', command)
        self.assertIn('+faststart', command)

    def test_temp_lifecycle_and_failed_inspection(self):
        source = self.audio('mp3', 'libmp3lame')
        original = source.read_bytes()
        api = DesktopApi()
        root = Path(api._temporary.name)
        try:
            self.assertFalse(hasattr(api, 'window'))
            self.assertTrue(api.describe_paths([str(source)])[0]['eligible'])
            first = api.convert_files([str(source)])[0]
            second = api.convert_files([str(source)])[0]
            self.assertTrue(first['ok'], first)
            self.assertNotEqual(first['output'], second['output'])
            self.assertEqual(Path(first['output']).read_bytes(), original)
            junk = self.root / 'bad.mkv'
            junk.write_bytes(b'not media')
            row = api.describe_paths([str(junk)])[0]
            self.assertFalse(row['eligible'])
            self.assertTrue(row['reasons'])
        finally:
            api._shutdown()
        self.assertFalse(root.exists())
        self.assertEqual(source.read_bytes(), original)
        self.assertFalse(api.convert_files([str(source)])[0]['ok'])


if __name__ == '__main__':
    unittest.main()
