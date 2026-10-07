import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
import numpy as np

spec = importlib.util.spec_from_file_location('audit', Path(__file__).parents[1] / 'sc_ray_first_hit_audit.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)

class OracleTest(unittest.TestCase):
    def test_known_triangle_hit_and_parallel_miss(self):
        nodes = np.zeros((1, 3, 4))
        nodes[0, 0, :3] = [-.001, -.001, .999]
        nodes[0, 1, :3] = [1.001, 1.001, 1.001]
        links = np.array([[0, 0, 0, 1]], dtype=np.uint32)
        tris = np.array([[[0,0,1,0],[1,0,0,0],[0,1,0,0]]], dtype=float)
        self.assertEqual(audit.cpu_first(np.array([.2,.2,0,.05,0,0,1,5]), nodes, links, tris), 1)
        self.assertEqual(audit.cpu_first(np.array([2,2,0,.05,0,0,1,5]), nodes, links, tris), -1)
        self.assertEqual(audit.cpu_first(np.array([.2,.2,0,.05,1,0,0,5]), nodes, links, tris), -1)

    def test_nonfinite_input_is_counted_and_ray_bytes_preserved(self):
        words = np.zeros((2,16), dtype='<u4')
        values = words.view('<f4')
        values[:,:8] = [1,2,3,.05,0,0,1,50]
        values[1,2] = float('nan')
        words[:,12] = 351
        with tempfile.TemporaryDirectory() as d:
            source=Path(d)/'source.bin'; target=str(Path(d)/'finite')
            source.write_bytes(struct.pack('<II',2,16)+words.tobytes())
            audit.prepare(source,target)
            self.assertEqual(json.loads(Path(target+'.prepare.json').read_text())['invalid_rays'],1)
            self.assertEqual(Path(target+'.rays.bin').read_bytes()[16:],words[0,:8].tobytes())

if __name__ == '__main__': unittest.main()
