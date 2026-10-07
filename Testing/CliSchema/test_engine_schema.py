#!/usr/bin/env python3
"""Offline CLI schema gate and boundary-bake parameter failure semantics."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
ENGINE=ROOT/'Darwin/Debug/primal-engine'


class EngineSchemaTests(unittest.TestCase):
    def invoke(self,*args):
        env=os.environ.copy()
        if sys.platform=='darwin':
            paths=[ROOT/'third_party/FBX_SDK/Mac/lib/debug',ROOT/'third_party/FBX_SDK/Mac/lib/release']
            env['DYLD_LIBRARY_PATH']=':'.join(map(str,paths))+':'+env.get('DYLD_LIBRARY_PATH','')
        reply=subprocess.run([str(ENGINE),*args],capture_output=True,text=True,env=env,timeout=30)
        return reply.returncode,json.loads(reply.stdout)

    def test_schema_snapshot(self):
        code,reply=self.invoke('schema')
        self.assertEqual(code,0)
        self.assertTrue(reply['ok'])
        expected=json.loads((ROOT/'Testing/CliSchema/engine-schema-snapshot.json').read_text())
        self.assertEqual(reply['data'],expected)

    def test_boundary_invalid_parameters_do_not_write(self):
        with tempfile.TemporaryDirectory(prefix='sc-boundary-cli-') as directory:
            d=Path(directory);source=str(d/'missing.model');primary=str(d/'primary.scsurf');aux=str(d/'aux.scboundary')
            cases=[['--boundary-spacing','nan'],['--boundary-spacing','0'],['--boundary-spacing','0.1junk'],
                   ['--boundary-spacing','0.1'],['--boundary-out',aux],['--boundary-texture','floor'],
                   ['--boundary-out',primary,'--boundary-texture','floor'],
                   ['--boundary-out',source,'--boundary-texture','floor']]
            for flags in cases:
                with self.subTest(flags=flags):
                    code,reply=self.invoke('sc-bake-surface','--in',source,'--out',primary,*flags)
                    self.assertEqual(code,2)
                    self.assertFalse(reply['ok'])
                    self.assertEqual(reply['error']['code'],'INVALID_PARAMS')
                    self.assertEqual(list(d.iterdir()),[])


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--engine',type=Path,default=ENGINE)
    args=parser.parse_args();ENGINE=args.engine.resolve()
    unittest.main(argv=[sys.argv[0]])
