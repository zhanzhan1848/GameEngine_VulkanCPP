#!/usr/bin/env python3
"""Validate SDF sections inside a pipeline model blob.

Parses the mesh/SDF layout expected by OfflineSDFMerger::LoadFromPipelineModel
(all LOD groups), and reports, per LOD0 mesh, the u16 distribution:
  signed   : d = u16/65535 * 2*max_dim - max_dim   (ContentTools ec3234b3+)
  unsigned : d = u16/65535 * max_dim               (legacy bakes)
Packing discriminator: signed packing maps the surface to u16=32768 exactly,
so the ±1-voxel surface band clusters around 32768 (band32k%); legacy
unsigned bakes have no structure at that code.
"""
import struct
import sys
import statistics

def read_u32(b, o): return struct.unpack_from('<I', b, o)[0]

def main(path):
    data = open(path, 'rb').read()
    size = len(data)
    off = 0
    if read_u32(data, off) == 0:
        off += 4
    num_mat = read_u32(data, off); off += 4
    for _ in range(num_mat):
        for _f in range(3):
            slen = read_u32(data, off); off += 4 + slen
    num_lods = read_u32(data, off); off += 4
    print(f"materials={num_mat} lod_groups={num_lods}")
    sdf_meshes = 0
    rows = []
    total_meshes = 0
    for g in range(num_lods):
        lod_name_len = read_u32(data, off); off += 4 + lod_name_len
        mesh_count = read_u32(data, off); off += 4
        total_meshes += mesh_count
        for i in range(mesh_count):
            name_len = read_u32(data, off); off += 4
            name = data[off:off+name_len].decode('utf-8', 'replace'); off += name_len
            lod_id = read_u32(data, off); off += 4
            mat_idx = struct.unpack_from('<i', data, off)[0]; off += 4
            elem_size = read_u32(data, off); off += 4
            elem_type = read_u32(data, off); off += 4
            vert_count = read_u32(data, off); off += 4
            idx_size = read_u32(data, off); off += 4
            idx_count = read_u32(data, off); off += 4
            off += 4  # lod threshold f32
            off += 12 * vert_count + elem_size * vert_count + idx_size * idx_count

            if off + 8 <= size and read_u32(data, off) == 0x4C48534D:  # 'MSHL'
                off += 4
                ml = read_u32(data, off); off += 4 + ml * 60
                mlv = read_u32(data, off); off += 4 + mlv * 4
                mlt = read_u32(data, off); off += 4 + mlt

            if off + 8 <= size and read_u32(data, off) == 0x20464453:  # 'SDF '
                off += 4
                res = [read_u32(data, off + 4*k) for k in range(3)]; off += 12
                bmin = struct.unpack_from('<3f', data, off); off += 12
                bmax = struct.unpack_from('<3f', data, off); off += 12
                sdim = max(bmax[k]-bmin[k] for k in range(3))
                n_data = read_u32(data, off); off += 4
                u16s = struct.unpack_from(f'<{n_data}H', data, off); off += n_data*2
                n_vox = read_u32(data, off); off += 4 + n_vox
                n_vf = read_u32(data, off); off += 4 + n_vf*2
                if (elem_type & 0x10) and off + 8 <= size:
                    off += 4
                    uv_n = read_u32(data, off); off += 4 + uv_n*8
                if lod_id in (0xFFFFFFFF, 0) and n_data > 0:
                    sdf_meshes += 1
                    inside = sum(1 for v in u16s if v < 32768)
                    # Signed packing maps the surface to u16=32768; ±1 voxel
                    # band clusters there. Unsigned bakes: no structure there.
                    voxel = sdim * 1.2 / max(1, res[0])
                    half = int(voxel / (2 * sdim) * 65535) + 1 if sdim > 0 else 1
                    band = sum(1 for v in u16s if abs(v - 32768) <= half)
                    med = statistics.median(u16s)
                    rows.append((name, res[0], sdim, n_data, min(u16s), max(u16s),
                                 med, inside/n_data, band/n_data, half))
    print(f"total meshes={total_meshes}")
    print(f"\nLOD0 meshes with SDF: {sdf_meshes}")
    print(f"{'name':<28}{'res':>4}{'sdim':>8}{'u16min':>8}{'u16max':>8}{'median':>8}{'inside%':>9}{'band32k%':>9}{'±hw':>6}")
    for (name, r, sdim, n, umin, umax, med, ins, band, half) in rows[:60]:
        print(f"{name[:27]:<28}{r:>4}{sdim:>8.2f}{umin:>8}{umax:>8}{med:>8.0f}{ins*100:>8.1f}%{band*100:>8.1f}%{half:>6}")
    if rows:
        total_ins = statistics.mean(r[7] for r in rows)
        total_band = statistics.mean(r[8] for r in rows)
        print(f"\nmean inside-fraction (u16<32768): {total_ins*100:.1f}%")
        print(f"mean surface-band clustering at u16≈32768: {total_band*100:.1f}%  "
              f"(signed bake expect >2-3%; legacy unsigned ~0-1%)")

if __name__ == '__main__':
    main(sys.argv[1])
