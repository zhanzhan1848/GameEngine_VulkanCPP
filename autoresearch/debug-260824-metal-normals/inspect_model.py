import math
import struct
import sys


class Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def u32(self):
        value = struct.unpack_from("<I", self.data, self.pos)[0]
        self.pos += 4
        return value

    def f32(self):
        value = struct.unpack_from("<f", self.data, self.pos)[0]
        self.pos += 4
        return value

    def string(self):
        size = self.u32()
        value = self.data[self.pos:self.pos + size]
        self.pos += size
        return value.decode("utf-8", errors="replace")

    def bytes(self, size):
        value = self.data[self.pos:self.pos + size]
        self.pos += size
        return value


def normalize(v):
    length = math.sqrt(sum(x * x for x in v))
    return tuple(x / length for x in v) if length > 1.0e-12 else (0.0, 0.0, 0.0)


def cross(a, b):
    return (
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    )


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def inspect(path):
    reader = Reader(open(path, "rb").read())
    scene_name = reader.string()
    material_count = reader.u32()
    for _ in range(material_count):
        reader.string()
        reader.string()
        reader.string()

    lod_groups = reader.u32()
    sign_counts = [0] * 8
    vertices = 0
    triangles = 0
    stored_dot_sum = 0.0
    flipped_dot_sum = 0.0
    compared = 0
    stored_better = 0
    flipped_better = 0
    light = normalize((0.9, 1.5, 0.8))
    lit_vertices = 0
    ndotl_sum = 0.0

    for _ in range(lod_groups):
        reader.string()
        mesh_count = reader.u32()
        for _ in range(mesh_count):
            reader.string()
            reader.u32()  # lod id
            reader.u32()  # material
            elem_size = reader.u32()
            reader.u32()  # element type
            vertex_count = reader.u32()
            index_size = reader.u32()
            index_count = reader.u32()
            reader.f32()

            pos_bytes = reader.bytes(vertex_count * 12)
            elem_bytes = reader.bytes(vertex_count * elem_size)
            index_bytes = reader.bytes(index_count * index_size)

            positions = [struct.unpack_from("<3f", pos_bytes, i * 12)
                         for i in range(vertex_count)]
            normals = []
            for i in range(vertex_count):
                base = i * elem_size
                t_sign = elem_bytes[base + 3]
                sign_counts[t_sign & 7] += 1
                nx, ny = struct.unpack_from("<2H", elem_bytes, base + 4)
                x = nx * (2.0 / 65535.0) - 1.0
                y = ny * (2.0 / 65535.0) - 1.0
                z = math.sqrt(max(0.0, 1.0 - x * x - y * y))
                if not (t_sign & 2):
                    z = -z
                normals.append(normalize((x, y, z)))
                ndotl = max(0.0, dot(normals[-1], light))
                lit_vertices += int(ndotl > 0.001)
                ndotl_sum += ndotl

            fmt = "<H" if index_size == 2 else "<I"
            indices = [struct.unpack_from(fmt, index_bytes, i * index_size)[0]
                       for i in range(index_count)]
            for i in range(0, index_count - 2, 3):
                ia, ib, ic = indices[i:i + 3]
                face = normalize(cross(sub(positions[ib], positions[ia]),
                                       sub(positions[ic], positions[ia])))
                if face == (0.0, 0.0, 0.0):
                    continue
                triangles += 1
                for index in (ia, ib, ic):
                    n = normals[index]
                    flipped_z = (n[0], n[1], -n[2])
                    ds = dot(n, face)
                    df = dot(flipped_z, face)
                    stored_dot_sum += ds
                    flipped_dot_sum += df
                    compared += 1
                    if ds >= df:
                        stored_better += 1
                    else:
                        flipped_better += 1
            vertices += vertex_count

            assert reader.u32() == 0x4C48534D
            meshlets = reader.u32()
            reader.bytes(meshlets * 60)
            reader.bytes(reader.u32() * 4)
            reader.bytes(reader.u32())
            assert reader.u32() == 0x20464453
            reader.bytes(12 + 24)
            reader.bytes(reader.u32() * 2)
            reader.bytes(reader.u32())
            reader.bytes(reader.u32() * 2)

    print(f"scene={scene_name!r} materials={material_count} lod_groups={lod_groups}")
    print(f"vertices={vertices} triangles={triangles} bytes_consumed={reader.pos}/{len(reader.data)}")
    print(f"t_sign_low3={sign_counts}")
    print(f"avg_dot_stored={stored_dot_sum / compared:.6f} avg_dot_flip_z={flipped_dot_sum / compared:.6f}")
    print(f"stored_better={stored_better} flipped_z_better={flipped_better}")
    print(f"lit_vertices={lit_vertices}/{vertices} avg_ndotl={ndotl_sum / vertices:.6f}")


inspect(sys.argv[1])
