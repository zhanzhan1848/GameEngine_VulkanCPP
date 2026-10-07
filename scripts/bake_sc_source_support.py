#!/usr/bin/env python3
from pathlib import Path
import sys,struct,json,hashlib,collections,numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parent))
from audit_sc_mesh_sdf import Reader
from audit_sc_hit_material_uv import parse_runtime_model
def read_source_model(data):
    """Strict legacy packed Sponza branch, not an all-format engine loader."""
    r=Reader(data);count=r.u32()
    if not 0<count<=10000: raise ValueError('Invalid runtime material count')
    start=r.pos;paths=None
    for fields in (5,3):
        r.pos=start
        try:
            candidate=[tuple(r.string() for _ in range(fields)) for _ in range(count)]
            groups=r.u32()
            if 0<groups<=100000:
                paths=candidate;break
        except ValueError: pass
    if paths is None: raise ValueError('Unknown runtime material layout')
    r.take(groups*8) # thresholds + u16/u16 lod offsets
    meshes=[];geometry=[]
    for group in range(groups):
        n=r.u32();r.u32() # count + byte-size metadata
        if not 0<n<=100000: raise ValueError('Invalid runtime mesh count')
        for local in range(n):
            card_bytes=b'';material=r.u32();stride=r.u32();nv=r.u32();ni=r.u32();etype=r.u32();topology=r.u32()
            if material>=len(paths) or stride not in (20,24) or etype & ~0x70 !=3 or topology!=4 or not 0<nv<=10000000 or not 0<ni<=30000000 or ni%3:
                raise ValueError('Unsupported runtime packed header/sidecar')
            pos=np.frombuffer(r.take(nv*12),dtype='<f4').reshape(nv,3).copy()
            r.take((-r.pos)%16)
            elements=r.take(nv*stride);r.take((-r.pos)%16)
            indices=np.frombuffer(r.take(ni*(2 if nv<65536 else 4)),dtype='<u2' if nv<65536 else '<u4').astype(int).reshape(-1,3)
            if np.any(indices>=nv) or not np.all(np.isfinite(pos)): raise ValueError('Invalid runtime geometry')
            if r.data[r.pos:r.pos+4]==b'MSHL':
                r.take(4);r.take(r.u32()*60);r.take(r.u32()*4);r.take(r.u32())
            if r.data[r.pos:r.pos+4]==b'SDF ':
                r.take(40);r.take(r.u32()*2);r.take(r.u32());r.take(r.u32()*2)
            if etype & 0x10:
                if r.take(4)!=b'UVLM': raise ValueError('Missing runtime UV sidecar')
                r.take(r.u32()*8)
            if etype & 0x20:
                magic=r.take(4)
                if magic==b'CDB2':
                    if r.u32()!=2: raise ValueError('Unsupported runtime card version')
                elif magic!=b'CDBK': raise ValueError('Unsupported runtime card sidecar')
                card_bytes=r.take(r.u32()*32);r.take(r.u32()*32)
            vertices=pos[indices];tri=np.stack([vertices[:,0],vertices[:,1]-vertices[:,0],vertices[:,2]-vertices[:,0]],axis=1)
            meshes.append(dict(card_bytes=card_bytes,positions=pos,indices=indices,elements=elements,element_type=etype,
                element_size=stride,material_index=material,material_paths=paths[material],triangles=tri))
            geometry.append(tri)
    if r.pos!=len(data): raise ValueError('Unconsumed runtime model bytes')
    return meshes,np.concatenate(geometry)

def fnv(data):
 h=0xcbf29ce484222325
 for byte in data:h=((h^byte)*0x100000001b3)&0xffffffffffffffff
 return h
import argparse
p=argparse.ArgumentParser(description='Extract original SCS1 surface points for IDs absent in resident owner cards; produce independent merged SCB1, never overwrite source');p.add_argument('--model',required=True);p.add_argument('--primary',required=True);p.add_argument('--boundary',required=True);p.add_argument('--prefix',required=True);p.add_argument('--owners',type=int,nargs='+',required=True);p.add_argument('--out',required=True);p.add_argument('--evidence',required=True);args=p.parse_args()
assert Path(args.out).resolve() not in {Path(args.model).resolve(),Path(args.primary).resolve(),Path(args.boundary).resolve()}
assert Path(args.evidence).resolve() not in {Path(args.out).resolve(),Path(args.model).resolve(),Path(args.primary).resolve(),Path(args.boundary).resolve()}
meshes,_=read_source_model(Path(args.model).read_bytes());native={}
for m in meshes:
 nv,ni=len(m['positions']),m['indices'].size;ib=m['indices'].astype('<u2' if nv<65536 else '<u4').tobytes();g=fnv(struct.pack('<2I',nv,ni)+m['positions'].tobytes()+ib+m['elements']);native[g]=m
primary=Path(args.primary).read_bytes();pos=96;records=[]
for _ in range(struct.unpack_from('<I',primary,24)[0]):
 g,l,mat,nh,num,off,size=struct.unpack_from('<3Q2I2Q',primary,pos);pos+=48;pages=[]
 for _ in range(num):
  pages.append(struct.unpack_from('<2I5Q',primary,pos));pos+=48
 records.append((g,l,mat,pages))
base=args.prefix;c=np.fromfile(base+'.atlas-cards.bin','<u4',offset=16).reshape(-1,16);size=int(np.fromfile(base+'.atlas-cards.bin','<u4',count=1)[0]);atlas=np.fromfile(base+'.atlas-ids.bin','<u4').reshape(size,size)
dtype=np.dtype([('id','<u4'),('position','<f4',(3,)),('normal','<f4',(3,)),('albedo','u1',(4,)),('params','u1',(4,))]);assert dtype.itemsize==36
new={};evidence=[]
for owner in args.owners:
 cs=c[c[:,12]==owner];slots=np.unique(cs[:,13]);assert len(slots)==1;g,l,mat,pages=records[int(slots[0])];m=native[g];assert fnv(m['card_bytes'])==l
 localcards=np.frombuffer(m['card_bytes'],'<u4').reshape(-1,8);lf=localcards.copy().view('<f4');pieces=[];maxerr=0.
 for card in cs:
  pc=int(card[15]);ci,res,alb,norm,dep,param,tri=pages[pc];assert ci==pc
  ids=np.frombuffer(primary,'<u4',count=res*res,offset=tri).reshape(res,res);d=np.frombuffer(primary,'<f4',count=res*res,offset=dep).reshape(res,res);a=np.frombuffer(primary,'u1',count=res*res*4,offset=alb).reshape(res,res,4);n=np.frombuffer(primary,'<f2',count=res*res*2,offset=norm).astype('float32').reshape(res,res,2);p=np.frombuffer(primary,'u1',count=res*res*4,offset=param).reshape(res,res,4)
  r,x,y=map(int,card[9:12]);resident=np.unique(atlas[y:y+r,x:x+r]);mask=(ids!=0xffffffff)&~np.isin(ids,resident)&np.isfinite(d)&(d>=0)&(a[:,:,3]==255);yy,xx=np.where(mask)
  out=np.zeros(len(xx),dtype=dtype);out['id']=ids[yy,xx];axis=int(localcards[pc,3]&255);sgn=1 if localcards[pc,3]>>8 else -1;u=1 if axis==0 else 0;v=1 if axis==2 else 2;center=lf[pc,:3];extent=lf[pc,4:7]
  points=np.tile(center,(len(xx),1));points[:,axis]+=sgn*d[yy,xx];points[:,u]+=((xx+.5)/res-.5).astype('float32')*2*extent[u];points[:,v]+=((yy+.5)/res-.5).astype('float32')*2*extent[v];out['position']=points
  enc=n[yy,xx]*2-1;normal=np.column_stack([enc,1-np.abs(enc).sum(axis=1)]);old=normal.copy();fold=normal[:,2]<0;normal[fold,:2]=(1-np.abs(old[fold,:2][:,::-1]))*np.sign(old[fold,:2]);normal/=np.linalg.norm(normal,axis=1)[:,None];out['normal']=normal;out['albedo']=a[yy,xx];out['params']=p[yy,xx]
  # Validate each point against its original primitive, never project/clamp a bad point.
  for tid in np.unique(out['id']):
   tr=m['triangles'][int(tid)].astype(float);origin,e1,e2=tr;pts=out['position'][out['id']==tid].astype(float);gram=np.array([[e1@e1,e1@e2],[e1@e2,e2@e2]]);bary=np.linalg.solve(gram,np.stack([(pts-origin)@e1,(pts-origin)@e2])).T;proj=origin+bary[:,0,None]*e1+bary[:,1,None]*e2;dist=np.where((bary>=0).all(axis=1)&(bary.sum(axis=1)<=1),np.linalg.norm(pts-proj,axis=1),np.inf)
   for j in range(3):
    corners=[origin,origin+e1,origin+e2];start=corners[j];edge=corners[(j+1)%3]-start;q=start+np.clip(((pts-start)@edge)/(edge@edge),0,1)[:,None]*edge;dist=np.minimum(dist,np.linalg.norm(pts-q,axis=1))
   maxerr=max(maxerr,float(dist.max()))
  pieces.append(out)
 samples=np.concatenate(pieces);samples=samples[np.argsort(samples['id'],kind='stable')];assert len(samples) and maxerr<=.0001 and np.isfinite(samples['normal']).all();key=(g,l,mat);assert key not in new;assert len(samples)<=1048576;new[key]=(len(m['triangles']),samples.tobytes());evidence.append(dict(owner=owner,slot=int(slots[0]),key=[f'{v:016x}' for v in key],samples=len(samples),target_IDs=len(np.unique(samples['id'])),max_point_triangle_error=maxerr))
old=Path(args.boundary).read_bytes();assert struct.unpack_from('<3I',old)==(0x31424353,1,40) and fnv(old[40:])==struct.unpack_from('<Q',old,32)[0];n=struct.unpack_from('<I',old,12)[0];existing={}
for i in range(n):
 g,l,mat,tc,count,off,sz=struct.unpack_from('<3Q2I2Q',old,40+i*48);existing[g,l,mat]=(tc,old[off:off+sz]);assert sz==count*36
assert not (existing.keys()&new.keys());merged=existing|new;offset=40+48*len(merged);directory=b'';payload=b''
for key,(tc,data) in sorted(merged.items()):
 directory+=struct.pack('<3Q2I2Q',*key,tc,len(data)//36,offset,len(data));payload+=data;offset+=len(data)
body=directory+payload;assert len(body)+40<=64*1024*1024;header=struct.pack('<8IQ',0x31424353,1,40,len(merged),48,36,0,0,fnv(body));out=Path(args.out);assert not out.exists();out.write_bytes(header+body)
report=dict(scope='original SCS1 points whose raw ID is absent from the corresponding runtime card, owners16/18/7 only; true original geometry/encoded surface attributes; no radiance or GPU hit proof',source_primary_sha256=hashlib.sha256(primary).hexdigest(),boundary=str(out),boundary_sha256=hashlib.sha256(out.read_bytes()).hexdigest(),original_boundary_samples_byte_preserved=True,original_records=len(existing),new_records=len(new),bytes=out.stat().st_size,owners=evidence)
Path(args.evidence).write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
