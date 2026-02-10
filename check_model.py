import struct
import sys

def read_uint32(f):
    data = f.read(4)
    if len(data) < 4: return None
    return struct.unpack('<I', data)[0]

def read_float(f):
    data = f.read(4)
    if len(data) < 4: return None
    return struct.unpack('<f', data)[0]

def check_model(path):
    print(f"Checking {path}")
    try:
        with open(path, 'rb') as f:
            lod_count = read_uint32(f)
            print(f"LOD Count: {lod_count}")
            
            if lod_count is None or lod_count > 100:
                print("Invalid LOD count")
                return

            for lod in range(lod_count):
                threshold = read_float(f)
                submesh_count = read_uint32(f)
                size_of_submeshes = read_uint32(f)
                
                print(f"LOD {lod}: Submeshes={submesh_count}")
                
                for i in range(submesh_count):
                    element_size = read_uint32(f)
                    vertex_count = read_uint32(f)
                    index_count = read_uint32(f)
                    elements_type = read_uint32(f)
                    primitive_topology = read_uint32(f)
                    
                    print(f"  Submesh {i}: Verts={vertex_count}, ElemSize={element_size}")
                    
                    # Read Positions
                    min_x, max_x = float('inf'), float('-inf')
                    min_y, max_y = float('inf'), float('-inf')
                    min_z, max_z = float('inf'), float('-inf')
                    
                    # Assuming packed positions (12 bytes per vertex)
                    pos_data = f.read(vertex_count * 12)
                    if len(pos_data) != vertex_count * 12:
                        print("    Error reading positions")
                        return

                    for v in range(vertex_count):
                        x = struct.unpack_from('<f', pos_data, v*12)[0]
                        y = struct.unpack_from('<f', pos_data, v*12+4)[0]
                        z = struct.unpack_from('<f', pos_data, v*12+8)[0]
                        
                        min_x = min(min_x, x); max_x = max(max_x, x)
                        min_y = min(min_y, y); max_y = max(max_y, y)
                        min_z = min(min_z, z); max_z = max(max_z, z)
                    
                    print(f"    Bounds: X[{min_x:.2f}, {max_x:.2f}], Y[{min_y:.2f}, {max_y:.2f}], Z[{min_z:.2f}, {max_z:.2f}]")
                    return
                    
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    check_model("/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/model_win_engine.model")
