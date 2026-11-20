#!/usr/bin/env python3
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parent.parent / 'fetch_assets.py'

class FetchAssetsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)
        self.root = self.base / 'checkout'
        self.source = self.base / 'objects'
        content = b'fixture bytes'
        self.sha = hashlib.sha256(content).hexdigest()
        self.key = f'sha256/{self.sha}/asset.model'
        obj = self.source / self.key
        obj.parent.mkdir(parents=True)
        obj.write_bytes(content)
        self.asset = {'id':'fixture','path':'EngineTest/assets/asset.model','sha256':self.sha,'size':len(content),'key':self.key,'groups':['test']}
        self.manifest = self.base / 'manifest.json'
        self.write_manifest()

    def tearDown(self):
        self.temp.cleanup()

    def write_manifest(self):
        self.manifest.write_text(json.dumps({'version':1,'assets':[self.asset]}))

    def run_cli(self, *args):
        return subprocess.run([sys.executable, str(SCRIPT), '--manifest', str(self.manifest), '--root', str(self.root), *args], capture_output=True, text=True)

    def test_fetch_verify_and_preserve_conflict(self):
        self.assertNotEqual(self.run_cli('verify','test').returncode, 0)
        self.assertEqual(self.run_cli('fetch','test','--source',str(self.source)).returncode, 0)
        self.assertEqual(self.run_cli('verify','test').returncode, 0)
        dest = self.root / self.asset['path']
        dest.write_bytes(b'my work')
        self.assertNotEqual(self.run_cli('fetch','test','--source',str(self.source)).returncode, 0)
        self.assertEqual(dest.read_bytes(), b'my work')

    def test_corrupt_source_is_not_installed(self):
        (self.source / self.key).write_bytes(b'wrong')
        self.assertNotEqual(self.run_cli('fetch','test','--source',str(self.source)).returncode, 0)
        self.assertFalse((self.root / self.asset['path']).exists())
        self.assertFalse(list(self.root.rglob('.asset-*')))

    def test_invalid_paths_and_groups_rejected(self):
        self.asset['path'] = 'EngineTest/assets/../../outside'
        self.write_manifest()
        self.assertNotEqual(self.run_cli('fetch','test','--source',str(self.source)).returncode, 0)
        self.assertFalse((self.base / 'outside').exists())
        self.asset['path'] = 'EngineTest/assets/asset.model'
        self.write_manifest()
        self.assertNotEqual(self.run_cli('verify','nonexistent').returncode, 0)

    def test_symlink_escape_rejected(self):
        asset_dir = self.root / 'EngineTest/assets'
        asset_dir.parent.mkdir(parents=True)
        outside = self.base / 'outside'
        outside.mkdir()
        asset_dir.symlink_to(outside, target_is_directory=True)
        self.assertNotEqual(self.run_cli('fetch','test','--source',str(self.source)).returncode, 0)
        self.assertFalse((outside / 'asset.model').exists())

if __name__ == '__main__':
    unittest.main()
