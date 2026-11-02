#!/usr/bin/env python3
"""Reproduce the opt-in Vulkan N-sign-only module without changing its baseline.

The numeric-ID patch is deliberately pinned to the validated baseline revision.
An unknown shader revision fails before any write. Tangent/handedness/FS stay literal.
"""
import argparse,hashlib,struct,sys
from pathlib import Path

def digest(data):return hashlib.sha256(data).hexdigest()
def build(data):
 if digest(data)!='5aeda3f3bc5cf45175c72620398e0ae01c3a134d93811732a8fe463a091682a9':raise ValueError('unvalidated baseline SPIR-V revision; requalify before patching')
 words=list(struct.unpack('<%dI'%(len(data)//4),data))
 if words[3]!=877:raise ValueError('unexpected baseline ID bound')
 instructions=[];i=5
 while i<len(words):
  count=words[i]>>16
  if not count or i+count>len(words):raise ValueError('invalid instruction extent')
  instructions.append(words[i:i+count]);i+=count
 def op(code,*args):return [(len(args)+1)<<16|code,*args]
 result=[];changed=0
 for ins in instructions:
  if ins==[6<<16|57,21,432,184,430,431]:
   result.extend([op(132,6,877,415,70),op(128,6,878,877,104),op(65,82,879,324,878),op(61,6,880,879),op(197,6,881,431,880)])
   updated=ins.copy();updated[-1]=881;result.append(updated);changed+=1
  else:result.append(ins)
 if changed!=1 or len(result)!=len(instructions)+5:raise ValueError('normal call match count mismatch')
 header=words[:5];header[3]=882;out=struct.pack('<%dI'%(5+sum(map(len,result))),*(header+sum(result,[])))
 return out

def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--source',type=Path,default=Path('Engine/Graphics/Vulkan/shaders/Nanite/GPUDrivenDraw.spv'));parser.add_argument('--output',type=Path,default=Path('Engine/Graphics/Vulkan/shaders/Nanite/GPUDrivenDrawQualifiedNormal.vert.spv'));parser.add_argument('--check',action='store_true');args=parser.parse_args()
 if args.source.resolve()==args.output.resolve():raise ValueError('baseline and output must differ')
 source=args.source.read_bytes();out=build(source)
 if args.check:
  if args.output.read_bytes()!=out:raise ValueError('qualified module is not the validated normal-only derivation')
 else:
  args.output.parent.mkdir(parents=True,exist_ok=True);temp=args.output.with_name(args.output.name+'.new');temp.write_bytes(out);temp.replace(args.output)
 if args.source.read_bytes()!=source:raise ValueError('baseline changed concurrently')
 print(('checked' if args.check else 'wrote')+' N-sign-only module '+digest(out),file=sys.stderr)
if __name__=='__main__':main()
