from pathlib import Path
import struct,unittest
from make_ddgi_strict_spirv import make_strict,parse,instruction,string_words,CORE_FLOAT_ARITHMETIC
class StrictSPIRVTests(unittest.TestCase):
 def blob(self,name='ddgi_first_record_proof',width=32,extras=()):
  ins=[instruction(17,1),instruction(14,0,1),instruction(15,5,4,*string_words(name)),instruction(16,4,17,32,1,1),*extras,instruction(19,1),instruction(22,2,width),instruction(129,2,8,6,7),instruction(133,2,9,8,7)]
  w=[0x07230203,0x10000,0,10,0]+[v for q in ins for v in q];return struct.pack('<'+str(len(w))+'I',*w)
 def test_precise_modes_and_complete_arithmetic(self):
  b,m=make_strict(self.blob());_,qs=parse(b);self.assertEqual(m['core_arithmetic_results'],2)
  self.assertEqual({tuple(q[1:]) for q in qs if q[0]&65535==16 and q[2] in (4461,4462)},{(4,4461,32),(4,4462,32)})
  self.assertEqual({tuple(q[1:]) for q in qs if q[0]&65535==71},{(8,42),(9,42)})
 def test_production_entry_rejected(self):
  with self.assertRaises(ValueError):make_strict(self.blob(name='ddgi_trace_sdf'))
 def test_float64_rejected(self):
  with self.assertRaises(ValueError):make_strict(self.blob(width=64))
 def test_relaxed_precision_rejected(self):
  with self.assertRaises(ValueError):make_strict(self.blob(extras=[instruction(71,8,0)]))
 def test_conflicting_or_repeat_modes_rejected(self):
  with self.assertRaises(ValueError):make_strict(self.blob(extras=[instruction(16,4,4463,32)]))
  with self.assertRaises(ValueError):make_strict(make_strict(self.blob())[0])
 def test_truncated_rejected(self):
  with self.assertRaises(ValueError):make_strict(self.blob()[:-4])
 def test_host_strict_scope_and_support_guard(self):
  s=(Path(__file__).resolve().parents[1]/'EngineTest/IntegrationTests/DDGIFirstRecordProof.inl').read_text()
  for q in ('DDGI_FIRST_RECORD_STRICT','DDGIFirstRecordProof_strict.comp.spv','shaderRoundingModeRTEFloat32','shaderSignedZeroInfNanPreserveFloat32','arithmetic != decorated','strict_core_decorations_checked_before_create') :self.assertIn(q,s)
  self.assertNotIn('vkSetMoltenVKConfigurationMVK',s)
if __name__=='__main__':unittest.main()
