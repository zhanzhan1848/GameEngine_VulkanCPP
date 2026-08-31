using System;
using System.Collections.Generic;
using System.IO;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Editors.Geometry;

namespace PrimalEditor_Avalonia.Editors.PrimitiveMesh;

public static class PrimitiveMeshCooker
{
    public const ulong MeshMagic = 0x48534D4550474D2DUL;

    public static bool Cook(string path, PrimitiveInitInfo info)
    {
        if (string.IsNullOrEmpty(path)) return false;
        var mesh = GenerateMesh(info);
        if (mesh == null)
        {
            Logger.Error($"PrimitiveMeshCooker: no mesh generated for type {info.Type}");
            return false;
        }

        var tempPath = path + ".partial";
        try
        {
            var dir = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir)) Directory.CreateDirectory(dir);

            using (var fs = File.Create(tempPath))
            using (var bw = new BinaryWriter(fs))
            {
                bw.Write(MeshMagic);
                bw.Write((uint)1);
                var lod = new MeshLOD { Name = "lod0", LodThreshold = 0f };
                lod.Meshes.Add(mesh);
                WriteLod(bw, lod);
            }
            if (File.Exists(path)) File.Delete(path);
            File.Move(tempPath, path);
            return true;
        }
        catch (Exception ex)
        {
            Logger.Error($"PrimitiveMeshCooker.Cook failed: {path}", ex);
            try { if (File.Exists(tempPath)) File.Delete(tempPath); } catch { }
            return false;
        }
    }

    private static void WriteLod(BinaryWriter bw, MeshLOD lod)
    {
        bw.Write(lod.Name ?? string.Empty);
        bw.Write(lod.LodThreshold);
        bw.Write((uint)lod.Meshes.Count);
        foreach (var mesh in lod.Meshes) WriteMesh(bw, mesh);
    }

    private static void WriteMesh(BinaryWriter bw, Mesh mesh)
    {
        bw.Write(mesh.Name ?? string.Empty);
        bw.Write((uint)mesh.VertexCount);
        bw.Write((uint)mesh.IndexCount);
        bw.Write((uint)mesh.IndexSize);
        bw.Write((uint)mesh.ElementsType);
        bw.Write((uint)mesh.Elements.Length);
        bw.Write(mesh.Positions);
        bw.Write(mesh.Elements);
        bw.Write(mesh.Indices);
    }

    private static Mesh? GenerateMesh(PrimitiveInitInfo info)
    {
        return info.Type switch
        {
            PrimitiveMeshType.Plane => BuildPlane(info),
            PrimitiveMeshType.Cube => BuildBox(info.SizeX, info.SizeY, info.SizeZ),
            PrimitiveMeshType.UvSphere => BuildSphere(info.SizeX * 0.5f, Math.Max(3, info.SegmentsX), Math.Max(2, info.SegmentsY)),
            PrimitiveMeshType.IcoSphere => BuildIcoSphere(info.SizeX * 0.5f, Math.Max(0, info.LOD)),
            PrimitiveMeshType.Cylinder => BuildCylinder(info.SizeX * 0.5f, info.SizeY, Math.Max(3, info.SegmentsX)),
            PrimitiveMeshType.Capsule => BuildCapsule(info.SizeX * 0.5f, info.SizeY, Math.Max(3, info.SegmentsX), Math.Max(2, info.SegmentsY)),
            _ => null,
        };
    }

    private static Mesh BuildPlane(PrimitiveInitInfo info)
    {
        var w = info.SizeX <= 0 ? 0.0001f : info.SizeX;
        var d = info.SizeZ <= 0 ? 0.0001f : info.SizeZ;
        var segX = Math.Max(1, info.SegmentsX);
        var segZ = Math.Max(1, info.SegmentsZ);
        var vertCount = (segX + 1) * (segZ + 1);
        var idxCount = segX * segZ * 6;
        var positions = new float[vertCount * 3];
        var elements = new byte[vertCount * ElementStride];
        var indices = new int[idxCount];
        int vi = 0;
        for (int z = 0; z <= segZ; ++z)
        {
            for (int x = 0; x <= segX; ++x)
            {
                var u = (float)x / segX;
                var v = (float)z / segZ;
                positions[vi * 3 + 0] = (u - 0.5f) * w;
                positions[vi * 3 + 1] = 0f;
                positions[vi * 3 + 2] = (v - 0.5f) * d;
                WriteVertexElement(elements, vi, 0f, 1f, 0f, u, v);
                ++vi;
            }
        }
        int ii = 0;
        for (int z = 0; z < segZ; ++z)
        {
            for (int x = 0; x < segX; ++x)
            {
                int a = z * (segX + 1) + x;
                int b = a + (segX + 1);
                indices[ii++] = a;     indices[ii++] = b;     indices[ii++] = a + 1;
                indices[ii++] = a + 1; indices[ii++] = b;     indices[ii++] = b + 1;
            }
        }
        return new Mesh
        {
            Name = "Plane",
            VertexCount = vertCount,
            IndexCount = idxCount,
            IndexSize = 4,
            ElementsType = ElementsType.TSpace,
            Positions = ToBytes(positions),
            Elements = elements,
            Indices = ToBytes(indices),
        };
    }

    private static Mesh BuildBox(float sx, float sy, float sz)
    {
        float hx = sx * 0.5f, hy = sy * 0.5f, hz = sz * 0.5f;
        var p = new (float x, float y, float z, float nx, float ny, float nz, float u, float v)[]
        {
            (-hx,-hy,-hz, 0, 0,-1, 0, 1), ( hx,-hy,-hz, 0, 0,-1, 1, 1), ( hx, hy,-hz, 0, 0,-1, 1, 0), (-hx, hy,-hz, 0, 0,-1, 0, 0),
            ( hx,-hy, hz, 0, 0, 1, 0, 1), (-hx,-hy, hz, 0, 0, 1, 1, 1), (-hx, hy, hz, 0, 0, 1, 1, 0), ( hx, hy, hz, 0, 0, 1, 0, 0),
            (-hx,-hy, hz,-1, 0, 0, 0, 1), (-hx,-hy,-hz,-1, 0, 0, 1, 1), (-hx, hy,-hz,-1, 0, 0, 1, 0), (-hx, hy, hz,-1, 0, 0, 0, 0),
            ( hx,-hy,-hz, 1, 0, 0, 0, 1), ( hx,-hy, hz, 1, 0, 0, 1, 1), ( hx, hy, hz, 1, 0, 0, 1, 0), ( hx, hy,-hz, 1, 0, 0, 0, 0),
            ( hx, hy,-hz, 0, 1, 0, 0, 1), (-hx, hy,-hz, 0, 1, 0, 1, 1), (-hx, hy, hz, 0, 1, 0, 1, 0), ( hx, hy, hz, 0, 1, 0, 0, 0),
            (-hx,-hy, hz, 0,-1, 0, 0, 1), ( hx,-hy, hz, 0,-1, 0, 1, 1), ( hx,-hy,-hz, 0,-1, 0, 1, 0), (-hx,-hy,-hz, 0,-1, 0, 0, 0),
        };
        var positions = new float[p.Length * 3];
        var elements = new byte[p.Length * ElementStride];
        for (int i = 0; i < p.Length; ++i)
        {
            positions[i * 3 + 0] = p[i].x;
            positions[i * 3 + 1] = p[i].y;
            positions[i * 3 + 2] = p[i].z;
            WriteVertexElement(elements, i, p[i].nx, p[i].ny, p[i].nz, p[i].u, p[i].v);
        }
        var idx = new int[p.Length];
        for (int i = 0; i < p.Length; ++i) idx[i] = i;
        return new Mesh
        {
            Name = "Cube",
            VertexCount = p.Length,
            IndexCount = p.Length,
            IndexSize = 4,
            ElementsType = ElementsType.TSpace,
            Positions = ToBytes(positions),
            Elements = elements,
            Indices = ToBytes(idx),
        };
    }

    private static Mesh BuildSphere(float radius, int segments, int rings)
    {
        var vertCount = (rings + 1) * (segments + 1);
        var idxCount = rings * segments * 6;
        var positions = new float[vertCount * 3];
        var elements = new byte[vertCount * ElementStride];
        var indices = new int[idxCount];
        int vi = 0;
        for (int r = 0; r <= rings; ++r)
        {
            float phi = (float)Math.PI * r / rings;
            float sp = (float)Math.Sin(phi), cp = (float)Math.Cos(phi);
            for (int s = 0; s <= segments; ++s)
            {
                float theta = 2f * (float)Math.PI * s / segments;
                float st = (float)Math.Sin(theta), ct = (float)Math.Cos(theta);
                float nx = sp * ct, ny = cp, nz = sp * st;
                positions[vi * 3 + 0] = nx * radius;
                positions[vi * 3 + 1] = ny * radius;
                positions[vi * 3 + 2] = nz * radius;
                WriteVertexElement(elements, vi, nx, ny, nz, (float)s / segments, (float)r / rings);
                ++vi;
            }
        }
        int ii = 0;
        for (int r = 0; r < rings; ++r)
        {
            for (int s = 0; s < segments; ++s)
            {
                int a = r * (segments + 1) + s;
                int b = a + (segments + 1);
                indices[ii++] = a;     indices[ii++] = b;     indices[ii++] = a + 1;
                indices[ii++] = a + 1; indices[ii++] = b;     indices[ii++] = b + 1;
            }
        }
        return new Mesh
        {
            Name = "UvSphere",
            VertexCount = vertCount,
            IndexCount = idxCount,
            IndexSize = 4,
            ElementsType = ElementsType.TSpace,
            Positions = ToBytes(positions),
            Elements = elements,
            Indices = ToBytes(indices),
        };
    }

    private static Mesh BuildIcoSphere(float radius, int subdivisions)
    {
        var t = 0.5f * (1f + (float)Math.Sqrt(5f));
        var verts = new List<(float x, float y, float z)>
        {
            (-1,  t,  0), ( 1,  t,  0), (-1, -t,  0), ( 1, -t,  0),
            ( 0, -1,  t), ( 0,  1,  t), ( 0, -1, -t), ( 0,  1, -t),
            ( t,  0, -1), ( t,  0,  1), (-t,  0, -1), (-t,  0,  1),
        };
        var faces = new List<(int a, int b, int c)>
        {
            (0,11,5),(0,5,1),(0,1,7),(0,7,10),(0,10,11),
            (1,5,9),(5,11,4),(11,10,2),(10,7,6),(7,1,8),
            (3,9,4),(3,4,2),(3,2,6),(3,6,8),(3,8,9),
            (4,9,5),(2,4,11),(6,2,10),(8,6,7),(9,8,1),
        };
        for (int s = 0; s < Math.Max(0, subdivisions); ++s)
        {
            var newFaces = new List<(int, int, int)>();
            var cache = new Dictionary<long, int>();
            int MidPoint(int a, int b)
            {
                long key = ((long)System.Math.Min(a, b) << 32) | (uint)System.Math.Max(a, b);
                if (cache.TryGetValue(key, out var i)) return i;
                var va = verts[a]; var vb = verts[b];
                float vx = va.x + vb.x;
                float vy = va.y + vb.y;
                float vz = va.z + vb.z;
                var len = (float)Math.Sqrt(vx * vx + vy * vy + vz * vz);
                vx /= len; vy /= len; vz /= len;
                verts.Add((vx, vy, vz));
                int idx = verts.Count - 1;
                cache[key] = idx;
                return idx;
            }
            foreach (var (a, b, c) in faces)
            {
                int ab = MidPoint(a, b);
                int bc = MidPoint(b, c);
                int ca = MidPoint(c, a);
                newFaces.Add((a, ab, ca));
                newFaces.Add((b, bc, ab));
                newFaces.Add((c, ca, bc));
                newFaces.Add((ab, bc, ca));
            }
            faces = newFaces;
        }
        var positionArr = new float[verts.Count * 3];
        for (int i = 0; i < verts.Count; ++i)
        {
            var v = verts[i];
            positionArr[i * 3 + 0] = v.x * radius;
            positionArr[i * 3 + 1] = v.y * radius;
            positionArr[i * 3 + 2] = v.z * radius;
        }
        var elements = new byte[verts.Count * ElementStride];
        for (int i = 0; i < verts.Count; ++i)
        {
            var v = verts[i];
            WriteVertexElement(elements, i, v.x, v.y, v.z, 0.5f + 0.5f * (float)Math.Atan2(v.x, v.z) / (float)Math.PI, 0.5f - (float)Math.Asin(v.y) / (float)Math.PI);
        }
        var indexArr = new int[faces.Count * 3];
        int k = 0;
        foreach (var (a, b, c) in faces)
        {
            indexArr[k++] = a; indexArr[k++] = b; indexArr[k++] = c;
        }
        return new Mesh
        {
            Name = "IcoSphere",
            VertexCount = verts.Count,
            IndexCount = indexArr.Length,
            IndexSize = 4,
            ElementsType = ElementsType.TSpace,
            Positions = ToBytes(positionArr),
            Elements = elements,
            Indices = ToBytes(indexArr),
        };
    }

    private static Mesh BuildCylinder(float radius, float height, int segments)
    {
        float hH = height * 0.5f;
        var pos = new List<float>();
        int vertIndex = 0;
        int vertCountHint = (segments + 1) * 2 + 2 * (segments + 2);
        var elem = new byte[vertCountHint * ElementStride];
        void AddV(float x, float y, float z, float nx, float ny, float nz, float u, float v)
        {
            pos.Add(x); pos.Add(y); pos.Add(z);
            int idx = vertIndex++;
            WriteVertexElement(elem, idx, nx, ny, nz, u, v);
        }
        for (int s = 0; s <= segments; ++s)
        {
            float t = 2f * (float)Math.PI * s / segments;
            float c = (float)Math.Cos(t), sn = (float)Math.Sin(t);
            float u = (float)s / segments;
            AddV(c * radius, -hH, sn * radius, c, 0, sn, u, 0);
            AddV(c * radius,  hH, sn * radius, c, 0, sn, u, 1);
        }
        int botCenter = vertIndex; AddV(0, -hH, 0, 0, -1, 0, 0.5f, 0.5f);
        int botRingStart = vertIndex;
        for (int s = 0; s <= segments; ++s)
        {
            float t = 2f * (float)Math.PI * s / segments;
            float c = (float)Math.Cos(t), sn = (float)Math.Sin(t);
            AddV(c * radius, -hH, sn * radius, 0, -1, 0, 0.5f + c * 0.5f, 0.5f + sn * 0.5f);
        }
        int topCenter = vertIndex; AddV(0, hH, 0, 0, 1, 0, 0.5f, 0.5f);
        int topRingStart = vertIndex;
        for (int s = 0; s <= segments; ++s)
        {
            float t = 2f * (float)Math.PI * s / segments;
            float c = (float)Math.Cos(t), sn = (float)Math.Sin(t);
            AddV(c * radius, hH, sn * radius, 0, 1, 0, 0.5f + c * 0.5f, 0.5f - sn * 0.5f);
        }
        var indices = new List<int>();
        for (int s = 0; s < segments; ++s)
        {
            int bl = s * 2, tl = s * 2 + 1;
            int br = bl + 2, tr = tl + 2;
            indices.Add(bl); indices.Add(tl); indices.Add(tr);
            indices.Add(bl); indices.Add(tr); indices.Add(br);
        }
        for (int s = 0; s < segments; ++s)
        {
            indices.Add(botCenter);
            indices.Add(botRingStart + s + 1);
            indices.Add(botRingStart + s);
        }
        for (int s = 0; s < segments; ++s)
        {
            indices.Add(topCenter);
            indices.Add(topRingStart + s);
            indices.Add(topRingStart + s + 1);
        }
        return new Mesh
        {
            Name = "Cylinder",
            VertexCount = vertIndex,
            IndexCount = indices.Count,
            IndexSize = 4,
            ElementsType = ElementsType.TSpace,
            Positions = ToBytes(pos.ToArray()),
            Elements = ResizeElementBuffer(elem, vertIndex),
            Indices = ToBytes(indices.ToArray()),
        };
    }

    private static Mesh BuildCapsule(float radius, float height, int segments, int hemiRings)
    {
        float hH = Math.Max(0f, height * 0.5f);
        var pos = new List<float>();
        var indices = new List<int>();
        int vertIndex = 0;
        int capsuleVertHint = (hemiRings + 1) * (segments + 1) * 2 + (segments + 1) * 2;
        var elem = new byte[capsuleVertHint * ElementStride];
        void AddV(float x, float y, float z, float nx, float ny, float nz, float u, float v)
        {
            pos.Add(x); pos.Add(y); pos.Add(z);
            WriteVertexElement(elem, vertIndex, nx, ny, nz, u, v);
            vertIndex++;
        }
        for (int r = 0; r <= hemiRings; ++r)
        {
            float phi = (float)Math.PI * 0.5f * r / hemiRings;
            float sp = (float)Math.Sin(phi), cp = (float)Math.Cos(phi);
            for (int s = 0; s <= segments; ++s)
            {
                float t = 2f * (float)Math.PI * s / segments;
                float st = (float)Math.Sin(t), ct = (float)Math.Cos(t);
                float nx = sp * ct, ny = cp, nz = sp * st;
                float y = hH + ny * radius;
                AddV(nx * radius, y, nz * radius, nx, ny, nz, (float)s / segments, 0.25f + 0.25f * r / hemiRings);
            }
        }
        int midStart = vertIndex;
        for (int s = 0; s <= segments; ++s)
        {
            float t = 2f * (float)Math.PI * s / segments;
            float c = (float)Math.Cos(t), sn = (float)Math.Sin(t);
            AddV(c * radius, -hH, sn * radius, c, 0, sn, (float)s / segments, 0.5f);
            AddV(c * radius,  hH, sn * radius, c, 0, sn, (float)s / segments, 0.5f);
        }
        for (int r = 0; r <= hemiRings; ++r)
        {
            float phi = (float)Math.PI * 0.5f * r / hemiRings;
            float sp = (float)Math.Sin(phi), cp = -(float)Math.Cos(phi);
            for (int s = 0; s <= segments; ++s)
            {
                float t = 2f * (float)Math.PI * s / segments;
                float st = (float)Math.Sin(t), ct = (float)Math.Cos(t);
                float nx = sp * ct, ny = cp, nz = sp * st;
                float y = -hH + ny * radius;
                AddV(nx * radius, y, nz * radius, nx, ny, nz, (float)s / segments, 0.5f + 0.25f * r / hemiRings);
            }
        }
        int ringCount = hemiRings + 1;
        for (int r = 0; r < ringCount - 1; ++r)
        {
            for (int s = 0; s < segments; ++s)
            {
                int a = r * (segments + 1) + s;
                int b = a + (segments + 1);
                indices.Add(a); indices.Add(b); indices.Add(a + 1);
                indices.Add(a + 1); indices.Add(b); indices.Add(b + 1);
            }
        }
        for (int s = 0; s < segments; ++s)
        {
            int bl = midStart + s * 2;
            int tl = bl + 1;
            int br = midStart + (s + 1) * 2;
            int tr = br + 1;
            indices.Add(bl); indices.Add(tl); indices.Add(tr);
            indices.Add(bl); indices.Add(tr); indices.Add(br);
        }
        int bottomStart = midStart + (segments + 1) * 2;
        int bottomRings = ringCount;
        for (int r = 0; r < bottomRings - 1; ++r)
        {
            for (int s = 0; s < segments; ++s)
            {
                int a = bottomStart + r * (segments + 1) + s;
                int b = a + (segments + 1);
                indices.Add(a); indices.Add(b); indices.Add(a + 1);
                indices.Add(a + 1); indices.Add(b); indices.Add(b + 1);
            }
        }
        return new Mesh
        {
            Name = "Capsule",
            VertexCount = vertIndex,
            IndexCount = indices.Count,
            IndexSize = 4,
            ElementsType = ElementsType.TSpace,
            Positions = ToBytes(pos.ToArray()),
            Elements = ResizeElementBuffer(elem, vertIndex),
            Indices = ToBytes(indices.ToArray()),
        };
    }

    private const int ElementStride = 20;

    private static void WriteVertexElement(byte[] dest, int index, float nx, float ny, float nz, float u, float v)
    {
        float nlen = (float)Math.Sqrt(nx * nx + ny * ny + nz * nz);
        if (nlen > 1e-8f) { nx /= nlen; ny /= nlen; nz /= nlen; }
        else { nx = 0; ny = 1; nz = 0; }

        ushort Pack(float c) => (ushort)Math.Clamp((int)Math.Round((c + 1f) * 32767.5f), 0, 65535);
        ushort n0 = Pack(nx), n1 = Pack(ny);
        byte sign = 0;
        if (nz >= 0) sign |= 0x02;
        uint colorTsign = 0x00FFFFFFu | ((uint)sign << 24);

        int o = index * ElementStride;
        BitConverter.GetBytes(colorTsign).CopyTo(dest, o + 0);
        BitConverter.GetBytes(n0).CopyTo(dest, o + 4);
        BitConverter.GetBytes(n1).CopyTo(dest, o + 6);
        BitConverter.GetBytes((ushort)Pack(1f)).CopyTo(dest, o + 8);
        BitConverter.GetBytes((ushort)Pack(0f)).CopyTo(dest, o + 10);
        BitConverter.GetBytes(u).CopyTo(dest, o + 12);
        BitConverter.GetBytes(v).CopyTo(dest, o + 16);
    }

    private static byte[] ToBytes(float[] arr)
    {
        var bytes = new byte[arr.Length * sizeof(float)];
        Buffer.BlockCopy(arr, 0, bytes, 0, bytes.Length);
        return bytes;
    }

    private static byte[] ToBytes(int[] arr)
    {
        var bytes = new byte[arr.Length * sizeof(int)];
        Buffer.BlockCopy(arr, 0, bytes, 0, bytes.Length);
        return bytes;
    }

    private static byte[] ResizeElementBuffer(byte[] src, int usedVertCount)
    {
        var used = usedVertCount * ElementStride;
        if (used == src.Length) return src;
        var trimmed = new byte[used];
        Buffer.BlockCopy(src, 0, trimmed, 0, used);
        return trimmed;
    }
}
