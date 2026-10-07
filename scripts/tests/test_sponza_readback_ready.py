import importlib.util,json,tempfile,time,unittest
from pathlib import Path
spec=importlib.util.spec_from_file_location('capture',Path(__file__).parents[1]/'sponza_reference.py');capture=importlib.util.module_from_spec(spec);spec.loader.exec_module(capture)
class ReadbackCompletion(unittest.TestCase):
 def test_stale_or_partial_or_failed_record_never_completes_capture(self):
  with tempfile.TemporaryDirectory() as d:
   p=Path(d)/'proof.json'
   self.assertFalse(capture.readbacks_ready([p],0))
   for value in ['{',json.dumps({}),json.dumps({'complete':False,'readback_completed':True}),json.dumps({'complete':True,'completion_checked':False})]:
    p.write_text(value);self.assertFalse(capture.readbacks_ready([p],0))
   p.write_text(json.dumps({'complete':True,'completion_checked':True}));self.assertFalse(capture.readbacks_ready([p],time.time()+10));self.assertTrue(capture.readbacks_ready([p],0))
 def test_each_independent_readback_must_complete(self):
  with tempfile.TemporaryDirectory() as d:
   p=Path(d)/'one.json';q=Path(d)/'two.json';p.write_text(json.dumps({'readback_completed':True}));q.write_text(json.dumps({'complete':False}))
   self.assertFalse(capture.readbacks_ready([p,q],0));q.write_text(json.dumps({'complete':True}));self.assertTrue(capture.readbacks_ready([p,q],0));self.assertTrue(capture.readbacks_ready([],0))
if __name__=='__main__':unittest.main()
