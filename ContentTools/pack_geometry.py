import struct
import sys
import math

def read_string(f):
    length_bytes = f.read(4)
    if not length_bytes: return None
    length = struct.unpack('<I', length_bytes)[0]
    print(f"Reading string at {f.tell() - 4}, length: {length}")
    if length == 0: return ""
    data = f.read(length)
    try:
        return data.decode('utf-8')
    except UnicodeDecodeError:
        print(f"Failed to decode string: {data}")
        return data.decode('utf-8', errors='replace')

def align_up(size, alignment):
    mask = alignment - 1
    return (size + mask) & ~mask

def main(input_file, output_file):
    with open(input_file, 'rb') as f:
        scene_name = read_string(f)
        print(f"Scene Name: {scene_name}")
        
        # Materials
        num_materials_bytes = f.read(4)
        if not num_materials_bytes:
            num_materials = 0
        else:
            num_materials = struct.unpack('<I', num_materials_bytes)[0]
        
        print(f"Num Materials: {num_materials}")
            
        materials = []
        for i in range(num_materials):
            print(f"Reading Material {i}")
            mat_name = read_string(f)
            diffuse = read_string(f)
            normal = read_string(f)
            materials.append({'name': mat_name, 'diffuse': diffuse, 'normal': normal})
            
        # LODs
        num_lods_bytes = f.read(4)
        if not num_lods_bytes:
            num_lods = 0
        else:
            num_lods = struct.unpack('<I', num_lods_bytes)[0]
            
        print(f"Num LODs: {num_lods}")

        lods = []
        for i in range(num_lods):
            print(f"Reading LOD {i}")
            lod_name = read_string(f)
            num_meshes = struct.unpack('<I', f.read(4))[0]
            meshes = []
            for _ in range(num_meshes):
                mesh = {}
                mesh['name'] = read_string(f)
                mesh['lod_id'] = struct.unpack('<I', f.read(4))[0]
                mesh['material_idx'] = struct.unpack('<I', f.read(4))[0]
                mesh['element_size'] = struct.unpack('<I', f.read(4))[0]
                mesh['elements_type'] = struct.unpack('<I', f.read(4))[0]
                mesh['vertex_count'] = struct.unpack('<I', f.read(4))[0]
                
                print(f"DEBUG: Mesh {mesh['name']} LOD={mesh['lod_id']} MatIdx={mesh['material_idx']} ElemSize={mesh['element_size']} ElemType={mesh['elements_type']} VertCount={mesh['vertex_count']}")

                mesh['index_size'] = struct.unpack('<I', f.read(4))[0]
                mesh['index_count'] = struct.unpack('<I', f.read(4))[0]
                mesh['lod_threshold'] = struct.unpack('<f', f.read(4))[0]
                
                # Buffers
                pos_size = 12 * mesh['vertex_count'] # sizeof(v3) = 12
                mesh['positions'] = f.read(pos_size)
                
                elem_buffer_size = mesh['element_size'] * mesh['vertex_count']
                mesh['elements'] = f.read(elem_buffer_size)
                
                idx_buffer_size = mesh['index_size'] * mesh['index_count']
                mesh['indices'] = f.read(idx_buffer_size)
                
                meshes.append(mesh)
            
            # The LOD threshold in C++ is per mesh, but in Engine Format it is per LOD Group.
            # We take the threshold from the first mesh of the LOD.
            lod_threshold = meshes[0]['lod_threshold'] if meshes else 0.0
            lods.append({'threshold': lod_threshold, 'meshes': meshes})

    write_engine_format(lods, materials, output_file)

def write_engine_format(lods, materials, output_file):
    # Write Engine Format (Matching MeshCPU.cpp structure + Materials)
    with open(output_file, 'wb') as f:
        # 0. Materials
        f.write(struct.pack('<I', len(materials)))
        for mat in materials:
            # Name
            name_bytes = mat['name'].encode('utf-8')
            f.write(struct.pack('<I', len(name_bytes)))
            f.write(name_bytes)
            # Diffuse
            diffuse_bytes = mat['diffuse'].encode('utf-8')
            f.write(struct.pack('<I', len(diffuse_bytes)))
            f.write(diffuse_bytes)
            # Normal
            normal_bytes = mat['normal'].encode('utf-8')
            f.write(struct.pack('<I', len(normal_bytes)))
            f.write(normal_bytes)

        # 1. lod_count
        f.write(struct.pack('<I', len(lods)))
        
        # 2. thresholds (float array)
        for lod in lods:
            f.write(struct.pack('<f', lod['threshold']))
            
        # 3. lod_offsets (struct { u16 offset; u16 count; })
        running_offset = 0
        for lod in lods:
            count = len(lod['meshes'])
            f.write(struct.pack('<H', running_offset)) # u16 offset
            f.write(struct.pack('<H', count))          # u16 count
            running_offset += count

        # 4. Per LOD Data
        for lod in lods:
            # submesh_count
            f.write(struct.pack('<I', len(lod['meshes'])))
            
            # Placeholder for size_of_submeshes
            size_pos = f.tell()
            f.write(struct.pack('<I', 0))
            
            start_pos = f.tell()
            
            for mesh in lod['meshes']:
                # Submesh Header (6 u32s now)
                # Note: MeshCPU.cpp does NOT write material_idx in the submesh header.
                # Handle potential unsigned -1 (0xFFFFFFFF) from C++
                mat_idx = mesh['material_idx']
                if mat_idx > 2147483647:
                    mat_idx -= 4294967296
                f.write(struct.pack('<i', mat_idx))
                f.write(struct.pack('<I', mesh['element_size']))
                f.write(struct.pack('<I', mesh['vertex_count']))
                f.write(struct.pack('<I', mesh['index_count']))
                f.write(struct.pack('<I', mesh['elements_type']))
                f.write(struct.pack('<I', 4)) # PrimitiveTopology: 4 (TriangleList)
                
                # Aligned Position Buffer (16-byte alignment)
                f.write(mesh['positions'])
                
                # Padding for Position Buffer
                current = f.tell()
                pad = (16 - (current & 15)) & 15
                if pad > 0:
                    f.write(b'\x00' * pad)
                
                # Aligned Element Buffer (16-byte alignment)
                elem_len = len(mesh['elements'])
                if elem_len > 0:
                    f.write(mesh['elements'])
                    current = f.tell()
                    pad = (16 - (current & 15)) & 15
                    if pad > 0:
                        f.write(b'\x00' * pad)
                
                # Index Buffer
                f.write(mesh['indices'])
                
            end_pos = f.tell()
            submeshes_size = end_pos - start_pos
            
            current = f.tell()
            f.seek(size_pos)
            f.write(struct.pack('<I', submeshes_size))
            f.seek(current)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: uv run ContentTools/pack_geometry.py <input_file> <output_file>")
    else:
        main(sys.argv[1], sys.argv[2])
