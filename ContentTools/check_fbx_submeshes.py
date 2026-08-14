#!/usr/bin/env python3
"""
FBX Submesh 详细分析器
分析FBX文件中每个网格的详细信息
"""

import sys
import os

try:
    import fbx
except ImportError:
    print("错误: 无法导入FBX SDK。")
    print("请确保已激活虚拟环境: source .venv/bin/activate")
    sys.exit(1)

def analyze_fbx_submeshes(fbx_file_path):
    """
    详细分析FBX文件中的submesh信息，包括LOD和贴图信息
    """
    manager = fbx.FbxManager.Create()
    if not manager:
        raise RuntimeError("无法创建FBX管理器")

    try:
        io_settings = fbx.FbxIOSettings.Create(manager, fbx.IOSROOT)
        manager.SetIOSettings(io_settings)

        importer = fbx.FbxImporter.Create(manager, "")
        if not importer.Initialize(fbx_file_path, -1, io_settings):
            raise RuntimeError(f"FBX导入失败: {importer.GetStatus().GetErrorString()}")

        scene = fbx.FbxScene.Create(manager, "Scene")
        importer.Import(scene)
        importer.Destroy()

        root_node = scene.GetRootNode()

        submesh_info = []
        texture_info = {}
        lod_groups = []

        # 收集场景中的所有贴图信息
        def collect_textures():
            """收集场景中所有纹理信息"""
            # 通过遍历场景中的所有材质来收集贴图
            material_count = scene.GetMaterialCount()
            for i in range(material_count):
                material = scene.GetMaterial(i)
                if material:
                    material_name = material.GetName()

                    # 遍历材质的所有属性来查找贴图
                    for property_name in ['DiffuseColor', 'NormalMap', 'Roughness', 'Metallic', 'Bump', 'EmissiveColor', 'SpecularColor']:
                        property_obj = material.FindProperty(property_name)
                        if property_obj and hasattr(property_obj, 'GetSrcObjectCount'):
                            texture_count = property_obj.GetSrcObjectCount()
                            for j in range(texture_count):
                                texture = property_obj.GetSrcObject(j)
                                if texture:
                                    texture_name = texture.GetName()
                                    if not texture_name:
                                        texture_name = f"Texture_{len(texture_info)}"

                                    texture_file = texture.GetFileName()
                                    texture_type = texture.GetTypeName()

                                    # 获取UV通道信息
                                    uv_set_name = ""
                                    if hasattr(texture, 'GetUVSet'):
                                        uv_set_name = texture.GetUVSet()

                                    if texture_name not in texture_info:
                                        texture_info[texture_name] = {
                                            'name': texture_name,
                                            'file': texture_file,
                                            'type': texture_type,
                                            'uv_set': uv_set_name,
                                            'users': []
                                        }

                                    # 记录贴图使用者
                                    texture_info[texture_name]['users'].append(material_name)

        # 收集LOD组信息
        def collect_lod_groups(node, parent_name="Root"):
            """收集LOD组信息"""
            if node is None:
                return

            node_attribute = node.GetNodeAttribute()
            if node_attribute:
                type_name = node_attribute.GetTypeName()

                if "LodGroup" in type_name or "LODGroup" in type_name:
                    lod_group_name = node.GetName()
                    children_count = node.GetChildCount()

                    lod_info = {
                        'name': lod_group_name,
                        'parent': parent_name,
                        'lod_levels': children_count,
                        'children': []
                    }

                    for i in range(children_count):
                        child = node.GetChild(i)
                        lod_info['children'].append(child.GetName())

                    lod_groups.append(lod_info)

            # 递归处理子节点
            for i in range(node.GetChildCount()):
                child = node.GetChild(i)
                collect_lod_groups(child, node.GetName() if node.GetName() else "Root")

        def process_mesh_node(node, index=0, parent_name="Root", lod_level=0):
            """处理网格节点，获取详细几何和材质信息"""
            if node is None:
                return

            node_attribute = node.GetNodeAttribute()
            if node_attribute:
                type_name = node_attribute.GetTypeName()

                if "Mesh" in type_name:
                    mesh = node_attribute
                    node_name = node.GetName()

                    # 获取网格信息
                    vertex_count = mesh.GetControlPointsCount()
                    polygon_count = mesh.GetPolygonCount()

                    # 获取材质信息
                    material_count = node.GetMaterialCount()
                    materials = []

                    for i in range(material_count):
                        material = node.GetMaterial(i)
                        if material:
                            material_name = material.GetName()

                            # 收集材质的贴图信息
                            material_textures = {
                                'name': material_name,
                                'diffuse': [],
                                'normal': [],
                                'roughness': [],
                                'metallic': [],
                                'other': []
                            }

                            # 遍历材质的所有贴图
                            for property_name in ['DiffuseColor', 'NormalMap', 'Roughness', 'Metallic', 'Bump', 'EmissiveColor']:
                                property_obj = material.FindProperty(property_name)
                                if property_obj and hasattr(property_obj, 'GetSrcObjectCount'):
                                    texture_count = property_obj.GetSrcObjectCount()
                                    for j in range(texture_count):
                                        texture = property_obj.GetSrcObject(j)
                                        if texture:
                                            texture_name = texture.GetName()
                                            texture_file = texture.GetFileName()

                                            texture_info_item = {
                                                'texture_name': texture_name,
                                                'texture_file': texture_file
                                            }

                                            # 根据属性类型分类
                                            if 'Diffuse' in property_name or 'Emissive' in property_name:
                                                material_textures['diffuse'].append(texture_info_item)
                                            elif 'Normal' in property_name or 'Bump' in property_name:
                                                material_textures['normal'].append(texture_info_item)
                                            elif 'Roughness' in property_name:
                                                material_textures['roughness'].append(texture_info_item)
                                            elif 'Metallic' in property_name:
                                                material_textures['metallic'].append(texture_info_item)
                                            else:
                                                material_textures['other'].append(texture_info_item)

                                            # 记录贴图使用者
                                            if texture_name in texture_info:
                                                texture_info[texture_name]['users'].append(f"{node_name}:{material_name}")

                            materials.append(material_textures)

                    # 获取UV信息
                    uv_count = 0
                    uv_set_names = []
                    if mesh.GetElementUVCount() > 0:
                        uv_count = mesh.GetElementUVCount()
                        for i in range(uv_count):
                            uv_element = mesh.GetElementUV(i)
                            if uv_element:
                                uv_set_names.append(uv_element.GetName())

                    # 获取法线信息
                    has_normals = mesh.GetElementNormalCount() > 0

                    # 获取切线信息
                    has_tangents = mesh.GetElementTangentCount() > 0

                    # 计算三角形数量
                    triangle_count = 0
                    for i in range(polygon_count):
                        polygon_size = mesh.GetPolygonSize(i)
                        if polygon_size == 3:
                            triangle_count += 1
                        elif polygon_size == 4:
                            triangle_count += 2  # 四边形转换为两个三角形

                    submesh_info.append({
                        'index': len(submesh_info),
                        'name': node_name,
                        'parent': parent_name,
                        'lod_level': lod_level,
                        'vertices': vertex_count,
                        'polygons': polygon_count,
                        'triangles': triangle_count,
                        'materials': materials,
                        'material_count': material_count,
                        'uv_channels': uv_count,
                        'uv_set_names': uv_set_names,
                        'has_normals': has_normals,
                        'has_tangents': has_tangents
                    })

            # 递归处理子节点
            for i in range(node.GetChildCount()):
                child = node.GetChild(i)
                process_mesh_node(child, len(submesh_info), node.GetName() if node.GetName() else "Root", lod_level)

        # 首先收集贴图和LOD信息
        collect_textures()
        if root_node:
            for i in range(root_node.GetChildCount()):
                collect_lod_groups(root_node.GetChild(i))

        # 然后处理所有网格节点
        if root_node:
            for i in range(root_node.GetChildCount()):
                process_mesh_node(root_node.GetChild(i))

        return {
            'submeshes': submesh_info,
            'textures': texture_info,
            'lod_groups': lod_groups
        }

    finally:
        manager.Destroy()

def print_submesh_summary(analysis_data):
    """打印submesh统计摘要，包括LOD和贴图信息"""
    print("=" * 80)
    print("FBX Submesh 详细分析 (包含LOD和贴图)")
    print("=" * 80)

    submesh_info = analysis_data['submeshes']
    texture_info = analysis_data['textures']
    lod_groups = analysis_data['lod_groups']

    if not submesh_info:
        print("未找到任何submesh")
        return

    total_vertices = sum(m['vertices'] for m in submesh_info)
    total_triangles = sum(m['triangles'] for m in submesh_info)
    total_materials = sum(m['material_count'] for m in submesh_info)

    print(f"总Submesh数量: {len(submesh_info)}")
    print(f"总顶点数: {total_vertices:,}")
    print(f"总三角形数: {total_triangles:,}")
    print(f"总材质引用: {total_materials}")
    print(f"总贴图数量: {len(texture_info)}")
    print(f"总LOD组数量: {len(lod_groups)}")
    print("=" * 80)

    # 打印每个submesh的详细信息
    print(f"\n{'Index':<6} {'Name':<35} {'Vertices':<10} {'Triangles':<10} {'Materials':<8} {'UV':<2}")
    print("-" * 80)

    for mesh in submesh_info:
        name = mesh['name'][:35] if mesh['name'] else "Unnamed"
        material_count = mesh['material_count']
        print(f"{mesh['index']:<6} {name:<35} {mesh['vertices']:<10} {mesh['triangles']:<10} {material_count:<8} {mesh['uv_channels']:<2}")

    print("-" * 80)

    # 打印LOD组信息
    if lod_groups:
        print(f"\nLOD组信息:")
        print("-" * 80)
        for i, lod_group in enumerate(lod_groups):
            print(f"LOD组 {i+1}: {lod_group['name']}")
            print(f"  LOD级别数: {lod_group['lod_levels']}")
            print(f"  子网格: {', '.join(lod_group['children'][:5])}", end="")
            if len(lod_group['children']) > 5:
                print(f", ... (共{len(lod_group['children'])}个)")
            else:
                print()
        print("-" * 80)

    # 打印贴图信息
    if texture_info:
        print(f"\n贴图信息:")
        print("-" * 80)
        for texture_name, texture_data in texture_info.items():
            print(f"贴图: {texture_name}")
            print(f"  文件: {texture_data['file']}")
            print(f"  类型: {texture_data['type']}")
            if texture_data['users']:
                print(f"  使用者: {', '.join(texture_data['users'][:3])}", end="")
                if len(texture_data['users']) > 3:
                    print(f", ... (共{len(texture_data['users'])}个)")
                else:
                    print()
            print()

        # 统计贴图类型分布
        texture_types = {}
        for texture_data in texture_info.values():
            texture_type = texture_data['type']
            texture_types[texture_type] = texture_types.get(texture_type, 0) + 1

        print("贴图类型分布:")
        for texture_type, count in sorted(texture_types.items()):
            print(f"  {texture_type}: {count}")
        print("-" * 80)

    # 打印材质和贴图详细信息（前10个网格）
    print(f"\n前10个网格的材质和贴图详情:")
    print("-" * 80)

    for i, mesh in enumerate(submesh_info[:10]):
        print(f"\n网格 {i}: {mesh['name']}")
        print(f"  材质数量: {mesh['material_count']}")

        for j, material in enumerate(mesh['materials']):
            print(f"  材质 {j}: {material['name']}")

            if material['diffuse']:
                print(f"    漫反射贴图: {', '.join([t['texture_file'] for t in material['diffuse']])}")
            if material['normal']:
                print(f"    法线贴图: {', '.join([t['texture_file'] for t in material['normal']])}")
            if material['roughness']:
                print(f"    粗糙度贴图: {', '.join([t['texture_file'] for t in material['roughness']])}")
            if material['metallic']:
                print(f"    金属度贴图: {', '.join([t['texture_file'] for t in material['metallic']])}")
            if material['other']:
                print(f"    其他贴图: {', '.join([t['texture_file'] for t in material['other']])}")

    if len(submesh_info) > 10:
        print(f"\n... 还有 {len(submesh_info) - 10} 个网格 (详情省略)")

    print("-" * 80)

    # 额外统计信息
    print(f"\n详细属性统计:")
    meshes_with_normals = sum(1 for m in submesh_info if m['has_normals'])
    meshes_with_tangents = sum(1 for m in submesh_info if m['has_tangents'])
    meshes_with_uv = sum(1 for m in submesh_info if m['uv_channels'] > 0)

    print(f"包含法线的网格: {meshes_with_normals}/{len(submesh_info)}")
    print(f"包含切线的网格: {meshes_with_tangents}/{len(submesh_info)}")
    print(f"包含UV的网格: {meshes_with_uv}/{len(submesh_info)}")

    # 材质统计
    total_diffuse = sum(len(m['materials'][k]['diffuse']) for m in submesh_info for k in range(len(m['materials'])) if m['materials'])
    total_normal = sum(len(m['materials'][k]['normal']) for m in submesh_info for k in range(len(m['materials'])) if m['materials'])
    total_roughness = sum(len(m['materials'][k]['roughness']) for m in submesh_info for k in range(len(m['materials'])) if m['materials'])
    total_metallic = sum(len(m['materials'][k]['metallic']) for m in submesh_info for k in range(len(m['materials'])) if m['materials'])

    print(f"\n贴图类型统计:")
    print(f"  漫反射贴图总数: {total_diffuse}")
    print(f"  法线贴图总数: {total_normal}")
    print(f"  粗糙度贴图总数: {total_roughness}")
    print(f"  金属度贴图总数: {total_metallic}")

    # 大小分布
    vertex_ranges = {
        'small (< 100)': sum(1 for m in submesh_info if m['vertices'] < 100),
        'medium (100-1000)': sum(1 for m in submesh_info if 100 <= m['vertices'] < 1000),
        'large (1000-10000)': sum(1 for m in submesh_info if 1000 <= m['vertices'] < 10000),
        'huge (> 10000)': sum(1 for m in submesh_info if m['vertices'] >= 10000)
    }

    print(f"\n顶点数量分布:")
    for range_name, count in vertex_ranges.items():
        print(f"  {range_name}: {count}")

    print("=" * 80)

def main():
    """主函数"""
    if len(sys.argv) < 2:
        print("用法: python check_fbx_submeshes.py <fbx_file_path>")
        print("示例: python check_fbx_submeshes.py EngineTest/assets/models/Sponza/Sponza.fbx")
        sys.exit(1)

    fbx_file_path = sys.argv[1]

    if not os.path.exists(fbx_file_path):
        print(f"错误: 文件不存在: {fbx_file_path}")
        sys.exit(1)

    print(f"正在分析FBX文件: {fbx_file_path}")

    try:
        analysis_data = analyze_fbx_submeshes(fbx_file_path)
        print_submesh_summary(analysis_data)

    except Exception as e:
        print(f"分析失败: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)

if __name__ == "__main__":
    main()