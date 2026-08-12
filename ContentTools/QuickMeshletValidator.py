#!/usr/bin/env python3
"""
快速Meshlet验证工具 - 基于现有.read_model_info.py扩展
验证Sponza.model中的meshlet数据有效性
"""

import sys
import struct
import os
from pathlib import Path

def read_model_info(model_file):
    """
    读取.model文件的基础信息（基于现有的read_model_info.py）
    """
    if not os.path.exists(model_file):
        print(f"错误: 文件不存在: {model_file}")
        return None

    with open(model_file, 'rb') as f:
        # 跳过材质数据
        num_materials = struct.unpack('<I', f.read(4))[0]
        for _ in range(num_materials):
            _ = read_string(f)  # name
            _ = read_string(f)  # diffuse
            _ = read_string(f)  # normal
            _ = read_string(f)  # roughness
            _ = read_string(f)  # metallic

        # LOD数据
        num_lods = struct.unpack('<I', f.read(4))[0]

        # 阈值
        for _ in range(num_lods):
            f.read(4)  # threshold

        # LOD偏移
        for _ in range(num_lods):
            f.read(2)  # offset
            f.read(2)  # count

        # 读取第一个LOD的mesh数据
        submesh_count = struct.unpack('<I', f.read(4))[0]
        size_of_submeshes = struct.unpack('<I', f.read(4))[0]

        print(f"LOD数量: {num_lods}")
        print(f"Submesh数量: {submesh_count}")

        meshlets_info = []

        for mesh_idx in range(submesh_count):
            # 读取submesh header
            mat_idx = struct.unpack('<i', f.read(4))[0]
            element_size = struct.unpack('<I', f.read(4))[0]
            vertex_count = struct.unpack('<I', f.read(4))[0]
            index_count = struct.unpack('<I', f.read(4))[0]

            print(f"\nSubmesh {mesh_idx}:")
            print(f"  材质索引: {mat_idx}")
            print(f"  元素大小: {element_size} 字节")
            print(f"  顶点数: {vertex_count}")
            print(f"  索引数: {index_count}")

            # 跳过位置数据
            f.read(12 * vertex_count)
            # 对齐
            align_pos = f.tell()
            padding = (16 - (align_pos % 16)) % 16
            if padding > 0:
                f.read(padding)

            # 跳过元素数据
            if element_size > 0:
                f.read(element_size * vertex_count)
                padding = (16 - (f.tell() % 16)) % 16
                if padding > 0:
                    f.read(padding)

            # 跳过索引数据
            # 尝试确定索引步长
            current_pos = f.tell()

            # 尝试4字节步长
            f.seek(current_pos + index_count * 4)
            magic_check = f.read(4)
            stride = 0
            if len(magic_check) == 4 and struct.unpack('<I', magic_check)[0] == 0x4C48534D:
                stride = 4
            else:
                # 尝试2字节步长
                f.seek(current_pos + index_count * 2)
                magic_check = f.read(4)
                if len(magic_check) == 4 and struct.unpack('<I', magic_check)[0] == 0x4C48534D:
                    stride = 2

            # 回到索引数据开始
            f.seek(current_pos)
            f.read(index_count * stride)

            # 读取MSHL标记
            magic_mshl = struct.unpack('<I', f.read(4))[0]
            if magic_mshl != 0x4C48534D:
                print(f"  ❌ 未找到MSHL标记 (找到: {hex(magic_mshl)})")
                continue

            print(f"  ✅ 找到MSHL标记")

            # 读取meshlet数据
            meshlet_count = struct.unpack('<I', f.read(4))[0]
            print(f"  Meshlet数量: {meshlet_count}")

            meshlet_data = []

            for ml_idx in range(meshlet_count):
                # 读取meshlet结构 (60字节)
                meshlet_bytes = f.read(60)
                if len(meshlet_bytes) < 60:
                    print(f"  ❌ Meshlet {ml_idx}: 数据不完整")
                    break

                # 解析meshlet数据
                meshlet_info = struct.unpack('<15I', meshlet_bytes[:60])  # 15个32位整数

                ml_data = {
                    'index': ml_idx,
                    'vertex_offset': meshlet_info[0],
                    'triangle_offset': meshlet_info[1],
                    'vertex_count': meshlet_info[2],
                    'triangle_count': meshlet_info[3],
                    # 假设后续数据格式
                }

                meshlet_data.append(ml_data)

                # 基本验证
                if ml_data['vertex_count'] == 0:
                    print(f"  ❌ Meshlet {ml_idx}: 顶点数量为0")
                elif ml_data['vertex_count'] > 64:
                    print(f"  ⚠️  Meshlet {ml_idx}: 顶点数量过多 ({ml_data['vertex_count']} > 64)")

                if ml_data['triangle_count'] == 0:
                    print(f"  ❌ Meshlet {ml_idx}: 三角形数量为0")
                elif ml_data['triangle_count'] > 124:
                    print(f"  ⚠️  Meshlet {ml_idx}: 三角形数量过多 ({ml_data['triangle_count']} > 124)")

            # 读取meshlet顶点数据
            meshlet_vertex_count = struct.unpack('<I', f.read(4))[0]
            print(f"  Meshlet顶点总数: {meshlet_vertex_count}")
            f.read(meshlet_vertex_count * 4)  # 跳过顶点数据

            # 读取meshlet三角形数据
            meshlet_triangle_count = struct.unpack('<I', f.read(4))[0]
            print(f"  Meshlet三角形总数: {meshlet_triangle_count}")
            triangle_data = f.read(meshlet_triangle_count)  # 跳过三角形数据

            # 验证三角形数据长度
            expected_triangle_bytes = sum(ml['triangle_count'] * 3 for ml in meshlet_data)
            if len(triangle_data) == expected_triangle_bytes:
                print(f"  ✅ 三角形数据长度正确 ({len(triangle_data)} 字节)")
            else:
                print(f"  ❌ 三角形数据长度不匹配 (期望: {expected_triangle_bytes}, 实际: {len(triangle_data)})")

            # 读取SDF标记
            magic_sdf = struct.unpack('<I', f.read(4))[0]
            if magic_sdf != 0x20464453:
                print(f"  ❌ 未找到SDF标记 (找到: {hex(magic_sdf)})")
            else:
                print(f"  ✅ 找到SDF标记")

            # 跳过SDF数据
            f.read(36)  # SDF header
            sdf_data_count = struct.unpack('<I', f.read(4))[0]
            f.read(sdf_data_count * 2)  # SDF data
            voxel_count = struct.unpack('<I', f.read(4))[0]
            f.read(voxel_count)  # Voxels
            vector_field_count = struct.unpack('<I', f.read(4))[0]
            f.read(vector_field_count * 2)  # Vector field

            meshlets_info.append({
                'mesh_index': mesh_idx,
                'meshlet_count': meshlet_count,
                'meshlets': meshlet_data,
                'total_vertices': meshlet_vertex_count,
                'total_triangles': meshlet_triangle_count
            })

        return meshlets_info

def read_string(f):
    """读取字符串"""
    length_bytes = f.read(4)
    if not length_bytes:
        return None
    length = struct.unpack('<I', length_bytes)[0]
    if length == 0:
        return ""
    data = f.read(length)
    try:
        return data.decode('utf-8')
    except UnicodeDecodeError:
        return data.decode('utf-8', errors='replace')

def validate_meshlets(meshlets_info):
    """
    验证meshlet数据的有效性
    """
    print("\n" + "=" * 60)
    print("🔍 MESHLET数据验证报告")
    print("=" * 60)

    if not meshlets_info:
        print("❌ 没有找到任何meshlet数据")
        return False

    total_meshlets = sum(info['meshlet_count'] for info in meshlets_info)
    print(f"总Meshlet数量: {total_meshlets}")

    all_valid = True
    warnings = []

    for mesh_info in meshlets_info:
        mesh_idx = mesh_info['mesh_index']
        meshlets = mesh_info['meshlets']

        print(f"\n📋 Mesh {mesh_idx} 验证:")

        # 统计验证
        valid_vertex_counts = 0
        valid_triangle_counts = 0
        zero_vertex = 0
        zero_triangle = 0
        overfilled_vertex = 0
        overfilled_triangle = 0

        for ml in meshlets:
            if ml['vertex_count'] == 0:
                zero_vertex += 1
                all_valid = False
            elif ml['vertex_count'] <= 64:
                valid_vertex_counts += 1
            else:
                overfilled_vertex += 1
                warnings.append(f"Mesh {mesh_idx}, Meshlet {ml['index']}: 顶点数过多 ({ml['vertex_count']})")

            if ml['triangle_count'] == 0:
                zero_triangle += 1
                all_valid = False
            elif ml['triangle_count'] <= 124:
                valid_triangle_counts += 1
            else:
                overfilled_triangle += 1
                warnings.append(f"Mesh {mesh_idx}, Meshlet {ml['index']}: 三角形数过多 ({ml['triangle_count']})")

        print(f"  ✅ 有效顶点数: {valid_vertex_counts}/{len(meshlets)}")
        print(f"  ✅ 有效三角形数: {valid_triangle_counts}/{len(meshlets)}")

        if zero_vertex > 0:
            print(f"  ❌ 零顶点Meshlet: {zero_vertex}")
        if zero_triangle > 0:
            print(f"  ❌ 零三角形Meshlet: {zero_triangle}")
        if overfilled_vertex > 0:
            print(f"  ⚠️  顶点过载Meshlet: {overfilled_vertex}")
        if overfilled_triangle > 0:
            print(f"  ⚠️  三角形过载Meshlet: {overfilled_triangle}")

    print("\n" + "=" * 60)

    if warnings:
        print(f"⚠️  发现 {len(warnings)} 个警告:")
        for i, warning in enumerate(warnings[:5]):
            print(f"  {i+1}. {warning}")
        if len(warnings) > 5:
            print(f"  ... 还有 {len(warnings) - 5} 个警告")

    if all_valid:
        print("✅ 所有Meshlet数据基本验证通过！")
        print("   数据结构有效，可以进行更深入的几何验证")
    else:
        print("❌ 发现严重的Meshlet数据问题，需要检查生成过程")

    return all_valid

def main():
    """主函数"""
    if len(sys.argv) < 2:
        print("用法: python QuickMeshletValidator.py <model_file>")
        print("示例: python QuickMeshletValidator.py Sponza.model")
        sys.exit(1)

    model_file = sys.argv[1]

    print(f"🔍 验证模型文件: {model_file}")
    print("=" * 60)

    try:
        meshlets_info = read_model_info(model_file)

        if meshlets_info:
            is_valid = validate_meshlets(meshlets_info)

            if is_valid:
                print("\n🎉 Meshlet数据验证完成，数据结构有效！")
                sys.exit(0)
            else:
                print("\n⚠️  Meshlet数据存在结构问题")
                sys.exit(1)
        else:
            print("\n❌ 无法读取模型文件")
            sys.exit(1)

    except Exception as e:
        print(f"\n❌ 验证过程出错: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)

if __name__ == "__main__":
    main()