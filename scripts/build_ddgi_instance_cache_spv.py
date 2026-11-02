#!/usr/bin/env python3
"""Build only the three opt-in instance normal cache modules. Never publish existing SPVs."""
from pathlib import Path
import subprocess, tempfile, hashlib
root=Path(__file__).resolve().parents[1]
s=root/'Engine/Graphics/Vulkan/Shaders';l=s/'Lumen'
with tempfile.TemporaryDirectory(prefix='primal-instance-normal-') as tmp:
    outputs=[]
    for name in ['DDGIInstanceNormalProduce','DDGIInstanceNormalVerify','DDGIGIGather_instance_cache']:
        out=Path(tmp)/(name+'.comp.spv')
        flags=[];source=l/(name+'.comp')
        if name.startswith('DDGIGIGather'):
            source=l/'DDGIGIGather.comp'
            flags=['-DDDGI_GEOMETRY_CONNECTIONS','-DDDGI_GEOMETRY_REPLACE','-DDDGI_GEOMETRY_ANY_HIT','-DDDGI_GEOMETRY_QUERY_KERNEL','-DDDGI_INSTANCE_NORMAL_CACHE','-Dddgi_gi_gather=main','--source-entrypoint','main','-e','ddgi_gi_gather']
        subprocess.run(['glslangValidator','--quiet','-V','-I'+str(s),*flags,str(source),'-o',str(out)],check=True)
        subprocess.run(['spirv-val','--target-env','vulkan1.2',str(out)],check=True)
        outputs.append((l/out.name,out.read_bytes()))
    for path,data in outputs:
        path.write_bytes(data)
        print(path.name,hashlib.sha256(data).hexdigest())
