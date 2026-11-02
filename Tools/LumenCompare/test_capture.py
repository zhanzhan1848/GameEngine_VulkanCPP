import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest

spec = importlib.util.spec_from_file_location('capture', Path(__file__).with_name('capture.py'))
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


class CaptureTests(unittest.TestCase):
    def test_native_adjacent_json(self):
        with tempfile.TemporaryDirectory() as name:
            path = Path(name) / 'passes.json'
            path.write_text('{"Id":0}\n{"Id":1}{"Id":2}\n')
            self.assertEqual([r['Id'] for r in capture.json_sequence(path)], [0, 1, 2])

    def test_wrong_frame_identity_is_rejected(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            capture.write(root / 'frame-1/capture.json', {'ordinal': 5, 'resources': [], 'complete': True})
            result = capture.inventory(root, 'primal')
            self.assertEqual(result['frames_present'], [])
            self.assertEqual(result['frames'][0]['status'], 'identity_mismatch')

    def test_archive_deduplicates_and_verifies_before_cleanup(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            (root / 'a.raw').write_bytes(b'float-data' * 100)
            (root / 'b.bin').write_bytes((root / 'a.raw').read_bytes())
            (root / 'capture.json').write_text('{}')
            capture.archive(SimpleNamespace(output=root, clean=True))
            self.assertFalse((root / 'a.raw').exists())
            self.assertTrue((root / 'capture.json').exists())
            manifest = json.loads((root / 'raw-manifest.json').read_text())
            self.assertEqual(len(list((root / 'objects').glob('*.gz'))), 1)
            for entry in manifest['files']:
                decoded = gzip.decompress((root / entry['object']).read_bytes())
                self.assertEqual(hashlib.sha256(decoded).hexdigest(), entry['sha256'])
            restored = root / 'selected-restore'
            capture.restore(SimpleNamespace(output=root, destination=restored, path=['a.raw']))
            self.assertEqual((restored / 'a.raw').read_bytes(), b'float-data' * 100)
            self.assertFalse((restored / 'b.bin').exists())
            with self.assertRaises(ValueError):
                capture.archive(SimpleNamespace(output=root, clean=True))

    def test_corrupt_archive_never_deletes_input(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            raw = root / 'input.raw'
            raw.write_bytes(b'original')
            (root / 'objects').mkdir()
            (root / 'objects' / (capture.digest(raw) + '.gz')).write_bytes(gzip.compress(b'corrupt'))
            with self.assertRaises(ValueError):
                capture.archive(SimpleNamespace(output=root, clean=True))
            self.assertEqual(raw.read_bytes(), b'original')


if __name__ == '__main__':
    unittest.main()
