import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

script=Path(__file__).parents[1]/'sc_permesh_gate.py'
spec=importlib.util.spec_from_file_location('gate',script)
gate=importlib.util.module_from_spec(spec);spec.loader.exec_module(gate)

class GateConfigurationTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.capture=self.root/'vulkan-test';self.capture.mkdir()
        # Invalid image data is intentional: incomparable captures must stop
        # before image measurement, rather than produce a misleading FAIL.
        (self.capture/'frame.ppm').write_bytes(b'not an image')
        self.expected={'SPONZA_REFERENCE':'1','SPGI_CAM_LOCK':'1','PRIMAL_TONE_ADAPT':'0','PRIMAL_TONE_EV':'1.8546'}
    def metadata(self,env):
        (self.capture/'capture.json').write_text(json.dumps({'frame':350,'environment':env}))
    def test_matching_configuration_allows_measurement(self):
        self.metadata(dict(self.expected,SC_BOUNDARY_CONSUMER='1'))
        self.assertEqual(gate.configuration_mismatches(str(self.root),self.expected),[])
    def test_missing_metadata_cannot_certify_comparability(self):
        rows=gate.configuration_mismatches(str(self.root),self.expected)
        self.assertEqual(rows[0]['reason'],'missing capture metadata')
    def test_exposure_and_adaptation_mismatch_rejects_before_roi(self):
        env=dict(self.expected);env.pop('PRIMAL_TONE_ADAPT');env['PRIMAL_TONE_EV']='-0.15';self.metadata(env)
        baseline=self.root/'baseline.json';baseline.write_text(json.dumps({'env':self.expected}))
        process=subprocess.run([sys.executable,str(script),'--baseline',str(baseline),'--dir',str(self.root)],capture_output=True,text=True)
        self.assertEqual(process.returncode,2,process.stderr)
        result=json.loads((self.root/'gate.json').read_text())
        self.assertEqual(result['status'],'INCOMPARABLE');self.assertIsNone(result['gate_pass'])
        self.assertEqual({r['key'] for r in result['configuration_mismatches']},{'PRIMAL_TONE_EV','PRIMAL_TONE_ADAPT'})
    def test_baseline_without_configuration_cannot_certify_comparability(self):
        self.metadata(self.expected)
        rows=gate.configuration_mismatches(str(self.root),{})
        self.assertEqual(rows,[{'reason':'baseline environment provenance missing'}])
    def test_camera_lock_mismatch_is_not_an_exposure_only_guard(self):
        env=dict(self.expected);env['SPGI_CAM_LOCK']='0';self.metadata(env)
        rows=gate.configuration_mismatches(str(self.root),self.expected)
        self.assertEqual(len(rows),1);self.assertEqual(rows[0]['key'],'SPGI_CAM_LOCK')

if __name__=='__main__':unittest.main()
