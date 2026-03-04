import struct
import sys

def read_string(f):
    length_bytes = f.read(4)
    if not length_bytes: return None
    length = struct.unpack('<I', length_bytes)[0]
    # print(f"Reading string at {f.tell() - 4}, length: {length}")
    if length == 0: return ""
    data = f.read(length)
    try:
        return data.decode('utf-8')
    except:
        return "<binary>"

def main(input_file):
    with open(input_file, 'rb') as f:
        # scene_name = read_string(f)
        # print(f"Scene Name: {scene_name}")
        
        num_materials = struct.unpack('<I', f.read(4))[0]
        print(f"Num Materials: {num_materials}")
        
        for i in range(num_materials):
            pos = f.tell()
            name = read_string(f)
            diff = read_string(f)
            norm = read_string(f)
            # rough = read_string(f)
            # metal = read_string(f)
            # orm = read_string(f)
            print(f"Mat {i} @ {pos}: {name} | Diff: {diff} | Norm: {norm}")

if __name__ == "__main__":
    main(sys.argv[1])
