#!/usr/bin/env python3
"""Restore a retained SC stage's gzip raw manifest into a new audit directory."""
import argparse, gzip, hashlib, json
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--stage',required=True,type=Path)
p.add_argument('--out',required=True,type=Path)
a=p.parse_args(); root=a.out.resolve(); restored=0
for name, entry in json.loads((a.stage/'raw-manifest.json').read_text()).items():
    target=(root/name).resolve()
    if not target.is_relative_to(root): raise ValueError('Manifest path escapes output directory')
    source=(a.stage/entry['gzip']).resolve()
    if not source.is_relative_to(a.stage.resolve()): raise ValueError('Gzip path escapes stage directory')
    data=gzip.decompress(source.read_bytes())
    if len(data)!=entry['bytes'] or hashlib.sha256(data).hexdigest()!=entry['sha256']:
        raise ValueError(f'Raw checksum mismatch: {name}')
    if target.exists() and target.read_bytes()!=data: raise FileExistsError(target)
    target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data);restored+=1
print(json.dumps({'restored':restored,'sha256_verified':True,'out':str(root)}))
