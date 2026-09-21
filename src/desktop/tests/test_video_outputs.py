import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from media import inspect_media, prepare, settings, command_for
from subtitles import discover, prepare_subtitle, validate_m4t
from subtitle_formats.build_vita_m4t import boxes, template_header, one_box, patch_moov


class VideoOutputTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.source = self.root / 'sample.mkv'
        first = self.root / 'signs.srt'
        second = self.root / 'full.srt'
        first.write_text('1\n00:00:00,000 --> 00:00:00,400\nONLY TRACK ONE\n', encoding='utf-8')
        second.write_text('1\n00:00:00,000 --> 00:00:00,400\n日本語 TRACK TWO\n', encoding='utf-8')
        subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=160x90:rate=24:duration=0.5',
                        '-f', 'lavfi', '-i', 'sine=frequency=440:duration=0.5',
                        '-f', 'lavfi', '-i', 'sine=frequency=880:duration=0.5',
                        '-i', str(first), '-i', str(second), '-map', '0', '-map', '1', '-map', '2', '-map', '3', '-map', '4',
                        '-c:v', 'mpeg4', '-c:a', 'flac', '-c:s', 'srt',
                        '-metadata:s:s:0', 'title=English - Signs/Songs', '-metadata:s:s:1', 'title=Japanese - Full',
                        '-metadata:s:s:0', 'language=eng', '-metadata:s:s:1', 'language=jpn', str(self.source)], check=True)

    def tearDown(self):
        self.temp.cleanup()

    def output(self, name):
        folder = self.root / name
        folder.mkdir()
        return folder

    def test_extract_selected_audio_no_video_or_subtitles(self):
        info = inspect_media(self.source)
        for fmt in ('mp3', 'aac', 'wav'):
            opts = settings(info, dict(output=fmt, audio_track=2, resolution='1080', fps='60', extract_bitrate=128))
            command = command_for(self.source, self.root / 'out', info, opts)
            self.assertEqual(command[command.index('-map')+1], '0:2')
            self.assertNotIn('-vf', command)
            target, result = prepare(self.source, self.output(fmt), opts)
            self.assertEqual(result['kind'], 'music')
            self.assertTrue(result['eligible'])
            self.assertFalse(target.with_suffix('.m4t').exists())
            data = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-show_streams', '-of', 'json', str(target)]))
            self.assertEqual([s['codec_type'] for s in data['streams']], ['audio'])
        with self.assertRaisesRegex(ValueError, 'soundtrack'):
            settings(info, dict(output='mp3', audio_track=-1))

    def test_soundless_mp4_gets_silent_aac_even_with_keep_original(self):
        source = self.root / 'silent.mp4'
        subprocess.run(['ffmpeg', '-v', 'error', '-i', str(self.source), '-map', '0:v:0',
                        '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-an', str(source)], check=True)
        info = inspect_media(source)
        self.assertFalse(info['eligible'])
        self.assertTrue(any('silent AAC' in r for r in info['reasons']))
        target, result = prepare(source, self.output('silent'))
        self.assertTrue(result['eligible'])
        data = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-show_streams', '-show_format', '-of', 'json', str(target)]))
        audio = next(s for s in data['streams'] if s['codec_type'] == 'audio')
        self.assertEqual((audio['codec_name'], audio['profile'], audio['sample_rate'], audio['channels']), ('aac', 'LC', '48000', 2))
        self.assertLess(float(data['format']['duration']), 1)
        pcm = subprocess.check_output(['ffmpeg', '-v', 'error', '-i', str(target), '-map', '0:a:0', '-f', 's16le', '-'])
        self.assertTrue(pcm)
        self.assertFalse(any(pcm))

    def test_folder_membership_and_names_survive_preparation(self):
        from app import DesktopApi
        from wifi import metadata
        from media import vita_filename
        api = DesktopApi()
        try:
            items = api.describe_video_folder(self.root)
            self.assertEqual([i['name'] for i in items], ['sample.mkv'])
            self.assertEqual(items[0]['video_folder'], self.root.name)
            target, result = prepare(self.source, self.output('named'))
            self.assertEqual(target.name, 'sample.mp4')
            result['video_folder'] = '日本語 Folder'
            self.assertEqual(metadata(result)['folder'], '日本語 Folder')
            self.assertEqual(vita_filename('Episode 01 日本語.mp4'), 'Episode 01 日本語.mp4')
            self.assertEqual(vita_filename('Episode:01?.mp4'), 'Episode_01_.mp4')
        finally:
            api._shutdown()

    def test_small_video_sizes(self):
        for size, dimensions in [('480', (854, 480)), ('360', (640, 360)),
                                 ('240', (426, 240)), ('144', (256, 144))]:
            with self.subTest(size=size):
                target, result = prepare(self.source, self.output(size),
                                         dict(resolution=size, fit='stretch', subtitle_track='none'))
                self.assertTrue(result['eligible'])
                self.assertEqual((result['width'], result['height']), dimensions)

    def test_subtitle_track_selection_and_language(self):
        info = inspect_media(self.source)
        self.assertEqual(len(info['subtitle_tracks']), 2)
        self.assertEqual(info['defaults']['subtitle_track'], 'stream:4')
        for index, text, absent in [(3, b'ONLY TRACK ONE', b'TRACK TWO'), (4, b'TRACK TWO', b'ONLY TRACK ONE')]:
            target, result = prepare(self.source, self.output(str(index)), dict(subtitle_track=f'stream:{index}'))
            sidecar = target.with_suffix('.m4t')
            self.assertEqual(result['sidecars'][0]['path'], str(sidecar))
            validate_m4t(sidecar)
            data = sidecar.read_bytes()
            self.assertIn(text, data)
            self.assertNotIn(absent, data)
            self.assertIn(b'origin=', data)
            if index == 4:
                self.assertIn('日本語'.encode(), data)
                self.assertIn(b'xml:lang="ja"', data)
        target, result = prepare(self.source, self.output('off'), dict(subtitle_track='none'))
        self.assertEqual(result['sidecars'], [])
        self.assertFalse(target.with_suffix('.m4t').exists())

    def test_external_ass_generic_dialogue_and_sidecar_survives_reprepare(self):
        ass = self.source.with_suffix('.ass')
        ass.write_text('[Script Info]\nScriptType: v4.00+\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n'
                       'Dialogue: 0,0:00:00.00,0:00:00.40,Default,,0,0,0,,Main speech\n'
                       'Dialogue: 0,0:00:00.00,0:00:00.40,Background,,0,0,0,,{\\an8}Background speech\n'
                       'Dialogue: 0,0:00:00.00,0:00:00.40,Signs,,0,0,0,,UNWANTED SIGN\n', encoding='utf-8')
        info = inspect_media(self.source)
        track = next(t for t in info['subtitle_tracks'] if 'path' in t)
        target, result = prepare(self.source, self.output('ass'), dict(subtitle_track=track['id']))
        data = target.with_suffix('.m4t').read_bytes()
        self.assertIn(b'Main speech', data)
        self.assertIn(b'Background speech', data)
        self.assertNotIn(b'UNWANTED SIGN', data)
        self.assertNotIn(b'region="top"', data)
        again, _ = prepare(target, self.output('again'))
        self.assertEqual(again.with_suffix('.m4t').read_bytes(), data)

    def test_bitmap_subtitles_are_detected_but_not_convertible(self):
        tracks = discover(self.source, [dict(index=7, codec_type='subtitle', codec_name='hdmv_pgs_subtitle')])
        self.assertFalse(tracks[0]['supported'])
        self.assertIn('OCR', tracks[0]['label'])

    def test_arbitrary_external_file(self):
        external = self.root / 'a completely different name.vtt'
        external.write_text('WEBVTT\n\n00:00.000 --> 00:00.400\nEXTERNAL CHOSEN\n', encoding='utf-8')
        target, result = prepare(self.source, self.output('external'), dict(subtitle_track='file:' + str(external), subtitle_language='eng'))
        data = target.with_suffix('.m4t').read_bytes()
        self.assertIn(b'EXTERNAL CHOSEN', data)
        self.assertNotIn(b'TRACK TWO', data)
        self.assertEqual(result['sidecars'][0]['language'], 'eng')

    def test_generated_container_header(self):
        header = template_header()
        self.assertEqual([(size, kind) for _, size, kind in boxes(header)], [(28,b'ftyp'), (620,b'moov')])
        off, size = one_box(header, b'moov')
        patched = patch_moov(header[off:off+size], 30000, 960, 540, 'jpn')
        self.assertIn(b'stpp', patched)


if __name__ == '__main__':
    unittest.main()
