import struct
import sys
import os

def read_string(f):
    length_bytes = f.read(4)
    if not length_bytes: return None
    length = struct.unpack('<I', length_bytes)[0]
    if length == 0: return ""
    return f.read(length).decode('utf-8')

def analyze(file_path):
    print(f"Analyzing {file_path}")
    size = os.path.getsize(file_path)
    print(f"File size: {size}")
    
    with open(file_path, 'rb') as f:
        # Read Header
        lod_count_bytes = f.read(4)
        if not lod_count_bytes:
            print("Empty file")
            return
        lod_count = struct.unpack('<I', lod_count_bytes)[0]
        print(f"LOD Count: {lod_count}")
        
        if lod_count > 1000:
            print("LOD Count seems too high. Maybe not an Engine Format file?")
            # Try to read as Content Format (starts with string length)
            f.seek(0)
            try:
                name = read_string(f)
                print(f"Possible Content Format. Scene Name: {name}")
            except:
                print("Not Content Format either.")
            return

        # Read Thresholds
        thresholds = []
        for i in range(lod_count):
            t = struct.unpack('<f', f.read(4))[0]
            thresholds.append(t)
        print(f"Thresholds (first 5): {thresholds[:5]}")
        
        # Read Offsets
        offsets = []
        for i in range(lod_count):
            offset = struct.unpack('<H', f.read(2))[0]
            count = struct.unpack('<H', f.read(2))[0]
            offsets.append((offset, count))
        print(f"Offsets (first 5): {offsets[:5]}")
        
        # Calculate Mesh Data Start
        # lod_count * 4 + lod_count * 4 + lod_count * 4
        # Wait, offsets loop writes 2 shorts (4 bytes).
        # So Header Size = 4 + lod_count * 4 + lod_count * 4.
        header_end = 4 + lod_count * 4 + lod_count * 4
        print(f"Header ends at: {header_end}")
        
        # Read Meshes
        # Mesh Size Offset
        mesh_size_offset = f.tell()
        mesh_data_size = struct.unpack('<I', f.read(4))[0]
        print(f"Mesh Data Size: {mesh_data_size}")
        
        # Mesh 0 Start
        mesh0_start = f.tell()
        print(f"Mesh 0 Start: {mesh0_start}")
        
        # Try New Format
        print("--- Trying New Format (MatIdx, ElemSize, ElemType, VertCount, IdxCount) ---")
        f.seek(mesh0_start)
        try:
            mat_idx = struct.unpack('<i', f.read(4))[0]
            elem_size = struct.unpack('<I', f.read(4))[0]
            vert_count = struct.unpack('<I', f.read(4))[0] # Wait, SceneDataAdapter reads VertCount 3rd?
            idx_count = struct.unpack('<I', f.read(4))[0]
            elem_type = struct.unpack('<I', f.read(4))[0]
            primitive_topo = struct.unpack('<I', f.read(4))[0]
            
            print(f"MatIdx: {mat_idx}")
            print(f"ElemSize: {elem_size}")
            print(f"VertCount: {vert_count}")
            print(f"IdxCount: {idx_count}")
            print(f"ElemType: {elem_type}")
            print(f"PrimitiveTopo: {primitive_topo}")
        except Exception as e:
            print(f"Error reading New Format: {e}")

        # Try Old Format
        print("--- Trying Old Format (ElemSize, VertCount, IdxCount, ElemType, PrimitiveTopo) ---")
        f.seek(mesh0_start)
        try:
            elem_size = struct.unpack('<I', f.read(4))[0]
            vert_count = struct.unpack('<I', f.read(4))[0]
            idx_count = struct.unpack('<I', f.read(4))[0]
            elem_type = struct.unpack('<I', f.read(4))[0]
            primitive_topo = struct.unpack('<I', f.read(4))[0]
            
            print(f"ElemSize: {elem_size}")
            print(f"VertCount: {vert_count}")
            print(f"IdxCount: {idx_count}")
            print(f"ElemType: {elem_type}")
            print(f"PrimitiveTopo: {primitive_topo}")
        except Exception as e:
            print(f"Error reading Old Format: {e}")

if __name__ == "__main__":
    if len(sys.argv) > 1:
        analyze(sys.argv[1])
    else:
        print("Usage: python analyze_model.py <file>")
