"""Bounded DUMP2 proof-only SPIR-V float controls, never a production module."""
from pathlib import Path
import argparse,hashlib,json,struct
CORE_FLOAT_ARITHMETIC={129,131,133,136,140,141,142,143,144,145,146,147,148}
ENTRY='ddgi_first_record_proof'
def instruction(op,*args):return [((len(args)+1)<<16)|op,*args]
def parse(blob):
 if len(blob)%4 or not 20<=len(blob)<=64*1024*1024:raise ValueError('invalid bounded module size')
 w=list(struct.unpack('<'+str(len(blob)//4)+'I',blob))
 if w[0]!=0x07230203:raise ValueError('invalid SPIR-V magic')
 chunks=[];i=5
 while i<len(w):
  n=w[i]>>16
  if n==0 or i+n>len(w):raise ValueError('invalid instruction')
  chunks.append(w[i:i+n]);i+=n
 return w[:5],chunks
def string_words(text):
 b=text.encode()+b'\0';b+=b'\0'*((-len(b))%4);return list(struct.unpack('<'+str(len(b)//4)+'I',b))
def make_strict(blob):
 header,chunks=parse(blob);entries=[];floats=[];ids=[]
 for q in chunks:
  op=q[0]&65535;a=q[1:]
  if op==15:
   name=struct.pack('<'+str(len(a[2:]))+'I',*a[2:]).split(b'\0',1)[0].decode()
   entries.append((a[0],a[1],name))
  if op==22:floats.append(a[1])
  if op==71 and a[1] in (0,40,42):raise ValueError('unexpected preexisting precision decoration')
  if op in CORE_FLOAT_ARITHMETIC:ids.append(a[1])
  if op==16 and a[1] in (4459,4460,4461,4462,4463):raise ValueError('preexisting float execution controls')
 if len(entries)!=1 or entries[0][0]!=5 or entries[0][2]!=ENTRY:raise ValueError('requires exact private compute entry')
 if set(floats)!={32} or not ids:raise ValueError('requires float32 arithmetic proof')
 # Float controls are supplied by SPV_KHR_float_controls even for a 1.0
 # module. Runtime Vulkan 1.2+ properties are checked before module creation.
 caps=[instruction(17,4466),instruction(17,4467)]
 ext=[instruction(10,*string_words('SPV_KHR_float_controls'))]
 modes=[instruction(16,entries[0][1],4461,32),instruction(16,entries[0][1],4462,32)]
 decos=[instruction(71,i,42) for i in ids]
 ci=next(i for i,q in enumerate(chunks) if q[0]&65535!=17);chunks[ci:ci]=caps+ext
 mi=max(i for i,q in enumerate(chunks) if q[0]&65535 in (15,16))+1;chunks[mi:mi]=modes
 di=next(i for i,q in enumerate(chunks) if 19<=q[0]&65535<=38);chunks[di:di]=decos
 words=header+[v for q in chunks for v in q];out=struct.pack('<'+str(len(words))+'I',*words)
 return out,dict(schema='ddgi_strict_spirv_v1',entry=ENTRY,core_arithmetic_results=len(ids),no_contraction_decorations=len(ids),float32_RTE=True,float32_signed_zero_inf_nan_preserve=True,global_fast_math_changed=False,input_sha256=hashlib.sha256(blob).hexdigest(),output_sha256=hashlib.sha256(out).hexdigest())
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('input',type=Path);p.add_argument('output',type=Path);p.add_argument('--metadata',type=Path);a=p.parse_args();out,meta=make_strict(a.input.read_bytes());a.output.write_bytes(out)
 if a.metadata:a.metadata.write_text(json.dumps(meta,indent=2)+'\n')
 print(json.dumps(meta,indent=2))
