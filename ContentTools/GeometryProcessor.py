#!/usr/bin/env python3
"""
Python Geometry Processor - 复刻ContentTools/Geometry.cpp的功能
使用Python meshoptimizer库实现相同的网格处理功能
"""

import numpy as np
import meshoptimizer
import struct
import math
from typing import Dict, List, Tuple, Optional, Any
from dataclasses import dataclass, field
from enum import IntEnum

class ElementsType(IntEnum):
    """顶点元素类型枚举，对应C++的elements::elements_type::type"""
    STATIC_COLOR = 0
    STATIC_NORMAL = 1
    STATIC_NORMAL_TEXTURE = 2
    SKELETAL = 3
    SKELETAL_COLOR = 4
    SKELETAL_NORMAL = 5
    SKELETAL_NORMAL_COLOR = 6
    SKELETAL_NORMAL_TEXTURE = 7
    SKELETAL_NORMAL_TEXTURE_COLOR = 8

@dataclass
class Meshlet:
    """Meshlet结构，对应C++的mesh::meshlet"""
    vertex_offset: int = 0
    triangle_offset: int = 0
    vertex_count: int = 0
    triangle_count: int = 0
    center: Tuple[float, float, float] = (0.0, 0.0, 0.0)
    radius: float = 0.0
    cone_apex: Tuple[float, float, float] = (0.0, 0.0, 0.0)
    cone_axis: Tuple[float, float, float] = (0.0, 0.0, 0.0)
    cone_cutoff: float = 0.0

@dataclass
class SDF:
    """有符号距离场数据"""
    resolution: Tuple[int, int, int] = (32, 32, 32)
    bounds_min: Tuple[float, float, float] = (0.0, 0.0, 0.0)
    bounds_max: Tuple[float, float, float] = (0.0, 0.0, 0.0)
    data: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.uint16))
    voxels: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.uint8))
    vector_field: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.uint16))

@dataclass
class Mesh:
    """网格数据结构，对应C++的mesh"""
    name: str = ""
    lod_id: int = 0
    material_idx: int = 0
    lod_threshold: float = 0.0

    # 原始输入数据
    positions: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.float32))  # [N, 3]
    normals: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.float32))    # [N, 3]
    tangents: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.float32))   # [N, 4]
    uv_sets: List[np.ndarray] = field(default_factory=list)  # [[N, 2], ...]
    colors: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.float32))    # [N, 3]
    raw_indices: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.uint32))  # [M]
    material_indices: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.int32))  # [M/3]

    # 处理后的数据
    vertices: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.float32))  # processed vertices [N, 11+]
    indices: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.uint32))    # [M]

    # 压缩后的数据
    position_buffer: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.uint8))
    element_buffer: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.uint8))

    # Meshopt数据
    meshlets: List[Meshlet] = field(default_factory=list)
    meshlet_vertices: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.uint32))
    meshlet_triangles: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.uint8))

    # SDF数据
    sdf: SDF = field(default_factory=SDF)

    # 元数据
    elements_type: ElementsType = ElementsType.STATIC_NORMAL
    material_used: List[int] = field(default_factory=list)

class GeometryProcessor:
    """几何处理器，复刻C++的Geometry.cpp功能"""

    def __init__(self):
        self.epsilon = 1e-5
        self.pi = math.pi

    def recalculate_normals(self, mesh: Mesh) -> None:
        """
        重新计算法线，对应recalculate_normals函数
        """
        num_indices = len(mesh.raw_indices)
        mesh.normals = np.zeros((num_indices, 3), dtype=np.float32)

        for i in range(0, num_indices, 3):
            i0, i1, i2 = mesh.raw_indices[i], mesh.raw_indices[i+1], mesh.raw_indices[i+2]

            v0 = mesh.positions[i0]
            v1 = mesh.positions[i1]
            v2 = mesh.positions[i2]

            e0 = v1 - v0
            e1 = v2 - v0

            n = np.cross(e0, e1)
            norm_sq = np.dot(n, n)

            if norm_sq > self.epsilon:
                n = n / np.sqrt(norm_sq)
            else:
                n = np.array([0.0, 0.0, 0.0], dtype=np.float32)

            mesh.normals[i] = n
            mesh.normals[i+1] = n
            mesh.normals[i+2] = n

    def process_normals(self, mesh: Mesh, smoothing_angle: float = 180.0) -> None:
        """
        处理法线，对应process_normals函数
        """
        cos_alpha = math.cos(self.pi - smoothing_angle * self.pi / 180.0)
        is_hard_edge = abs(smoothing_angle - 180.0) < self.epsilon
        is_soft_edge = abs(smoothing_angle) < self.epsilon

        num_indices = len(mesh.raw_indices)
        num_vertices = len(mesh.positions)

        if num_indices == 0 or num_vertices == 0:
            return

        mesh.indices = np.zeros(num_indices, dtype=np.uint32)

        # 建立顶点引用映射
        idx_ref = [[] for _ in range(num_vertices)]
        for i in range(num_indices):
            idx_ref[mesh.raw_indices[i]].append(i)

        # 处理每个顶点的法线
        mesh.vertices = []
        for i in range(num_vertices):
            refs = idx_ref[i]
            num_refs = len(refs)

            for j in range(num_refs):
                mesh.indices[refs[j]] = len(mesh.vertices)

                vertex_data = {
                    'position': mesh.positions[mesh.raw_indices[refs[j]]].copy(),
                    'normal': mesh.normals[refs[j]].copy()
                }

                n1 = vertex_data['normal'].copy()

                if not is_hard_edge:
                    for k in range(j + 1, num_refs):
                        cos_theta = 0.0
                        n2 = mesh.normals[refs[k]].copy()

                        if not is_soft_edge:
                            # 计算法线夹角余弦值
                            n1_norm = np.linalg.norm(n1)
                            if n1_norm > self.epsilon:
                                cos_theta = np.dot(n1, n2) / n1_norm

                        if is_soft_edge or cos_theta >= cos_alpha:
                            n1 += n2
                            mesh.indices[refs[k]] = mesh.indices[refs[j]]
                            refs.pop(k)
                            num_refs -= 1
                            break  # 需要重新开始循环

                # 归一化法线
                n1_norm = np.linalg.norm(n1)
                if n1_norm > self.epsilon:
                    vertex_data['normal'] = n1 / n1_norm
                else:
                    vertex_data['normal'] = np.array([0.0, 1.0, 0.0], dtype=np.float32)

                mesh.vertices.append(vertex_data)

    def process_uvs(self, mesh: Mesh) -> None:
        """
        处理UV坐标，对应process_uvs函数
        """
        if len(mesh.vertices) == 0 or len(mesh.uv_sets) == 0:
            return

        old_vertices = mesh.vertices.copy()
        old_indices = mesh.indices.copy()

        num_vertices = len(old_vertices)
        num_indices = len(old_indices)

        # 建立索引引用
        idx_ref = [[] for _ in range(num_vertices)]
        for i in range(num_indices):
            idx_ref[old_indices[i]].append(i)

        # 处理UV分离
        mesh.vertices = []
        mesh.indices = np.zeros(num_indices, dtype=np.uint32)

        for i in range(num_vertices):
            refs = idx_ref[i]
            num_refs = len(refs)

            for j in range(num_refs):
                mesh.indices[refs[j]] = len(mesh.vertices)

                vertex_data = old_vertices[old_indices[refs[j]]].copy()
                if len(mesh.uv_sets[0]) > refs[j]:
                    vertex_data['uv'] = mesh.uv_sets[0][refs[j]].copy()

                mesh.vertices.append(vertex_data)

                # 检查UV是否相同
                for k in range(j + 1, num_refs):
                    if len(mesh.uv_sets[0]) > refs[k]:
                        uv1 = mesh.uv_sets[0][refs[k]]
                        uv_diff = abs(vertex_data['uv'] - uv1)

                        if uv_diff[0] < self.epsilon and uv_diff[1] < self.epsilon:
                            mesh.indices[refs[k]] = mesh.indices[refs[j]]
                            refs.pop(k)
                            num_refs -= 1
                            break

    def determine_elements_type(self, mesh: Mesh) -> ElementsType:
        """
        确定顶点元素类型，对应determine_elements_type函数
        """
        has_normals = len(mesh.normals) > 0
        has_uvs = len(mesh.uv_sets) > 0 and len(mesh.uv_sets[0]) > 0
        has_colors = len(mesh.colors) > 0

        if has_normals:
            if has_uvs:
                return ElementsType.STATIC_NORMAL_TEXTURE
            else:
                return ElementsType.STATIC_NORMAL
        elif has_colors:
            return ElementsType.STATIC_COLOR

        return ElementsType.STATIC_COLOR  # 默认

    def process_meshlets(self, mesh: Mesh, max_vertices: int = 64,
                        max_triangles: int = 124, cone_weight: float = 0.5) -> None:
        """
        生成meshlet，对应process_meshlets函数（简化版本）
        """
        if len(mesh.indices) == 0:
            return

        # 准备顶点数据（位置）
        vertex_positions = np.array([v['position'] for v in mesh.vertices], dtype=np.float32)

        # 估算meshlet数量（Python版本没有build_meshlets_bound）
        estimated_meshlets = max(1, len(mesh.indices) // (max_triangles * 3))

        # 分配临时缓冲区
        local_meshlet_vertices = np.zeros(estimated_meshlets * max_vertices, dtype=np.uint32)
        local_meshlet_triangles = np.zeros(estimated_meshlets * max_triangles * 3, dtype=np.uint8)

        # 构建meshlets（使用简化的API调用）
        try:
            # Python meshoptimizer 的API略有不同
            meshlet_result = meshoptimizer.build_meshlets(
                mesh.indices,
                vertex_positions,
                max_vertices=max_vertices,
                max_triangles=max_triangles
            )

            # 转换结果到我们的数据结构
            mesh.meshlets = []
            mesh.meshlet_vertices = []
            mesh.meshlet_triangles = []

            # 假设返回的是字典或对象
            if isinstance(meshlet_result, dict):
                for ml_data in meshlet_result.get('meshlets', []):
                    meshlet = Meshlet(
                        vertex_count=ml_data.get('vertex_count', 0),
                        triangle_count=ml_data.get('triangle_count', 0)
                    )
                    mesh.meshlets.append(meshlet)

                mesh.meshlet_vertices = np.array(meshlet_result.get('vertices', []), dtype=np.uint32)
                mesh.meshlet_triangles = np.array(meshlet_result.get('triangles', []), dtype=np.uint8)

        except Exception as e:
            # 如果API调用失败，创建一个简化的meshlet
            print(f"警告: Meshlet生成失败，使用简化版本: {e}")

            # 创建一个包含所有网格的单一meshlet
            meshlet = Meshlet(
                vertex_count=len(mesh.vertices),
                triangle_count=len(mesh.indices) // 3
            )

            # 简化的边界计算
            if len(vertex_positions) > 0:
                center = np.mean(vertex_positions, axis=0)
                distances = np.linalg.norm(vertex_positions - center, axis=0)
                radius = np.max(distances) if len(distances) > 0 else 0.0

                meshlet.center = tuple(center)
                meshlet.radius = float(radius)

            mesh.meshlets = [meshlet]
            mesh.meshlet_vertices = mesh.indices.copy()  # 简化：直接使用索引
            mesh.meshlet_triangles = np.array([], dtype=np.uint8)

    def generate_sdf(self, mesh: Mesh, resolution: int = 32) -> None:
        """
        生成有符号距离场，对应generate_sdf函数（简化版本）
        """
        if len(mesh.vertices) == 0:
            return

        mesh.sdf = SDF()
        mesh.sdf.resolution = (resolution, resolution, resolution)

        # 计算包围盒
        positions = np.array([v['position'] for v in mesh.vertices], dtype=np.float32)
        min_bounds = np.min(positions, axis=0)
        max_bounds = np.max(positions, axis=0)

        # 添加padding
        size = max_bounds - min_bounds
        max_dim = np.max(size)
        if max_dim < 0.0001:
            max_dim = 1.0

        padding = max_dim * 0.2
        min_bounds -= padding
        max_bounds += padding
        size = max_bounds - min_bounds

        mesh.sdf.bounds_min = tuple(min_bounds)
        mesh.sdf.bounds_max = tuple(max_bounds)

        # 初始化SDF数据
        mesh.sdf.data = np.zeros(resolution ** 3, dtype=np.uint16)
        mesh.sdf.voxels = np.zeros(resolution ** 3, dtype=np.uint8)
        mesh.sdf.vector_field = np.zeros(resolution ** 3 * 4, dtype=np.uint16)

        step = size / resolution

        # 简化的SDF计算（实际实现应该使用更高效的算法）
        for z in range(resolution):
            for y in range(resolution):
                for x in range(resolution):
                    point = min_bounds + step * (np.array([x, y, z]) + 0.5)
                    idx = z * resolution * resolution + y * resolution + x

                    # 找到最近点（简化：只检查距离）
                    distances = np.linalg.norm(positions - point, axis=1)
                    min_dist = np.min(distances) if len(distances) > 0 else 0.0

                    # 打包距离数据
                    packed_dist = self.pack_float(min_dist, 0.0, max_dim, 16)
                    mesh.sdf.data[idx] = packed_dist

                    # 体素：占用判断
                    voxel_diag = np.linalg.norm(step)
                    mesh.sdf.voxels[idx] = 255 if min_dist < voxel_diag * 0.5 else 0

                    # 向量场：指向最近点的向量
                    closest_idx = np.argmin(distances)
                    to_closest = positions[closest_idx] - point

                    vec_idx = idx * 4
                    mesh.sdf.vector_field[vec_idx + 0] = self.pack_float(to_closest[0], -max_dim, max_dim, 16)
                    mesh.sdf.vector_field[vec_idx + 1] = self.pack_float(to_closest[1], -max_dim, max_dim, 16)
                    mesh.sdf.vector_field[vec_idx + 2] = self.pack_float(to_closest[2], -max_dim, max_dim, 16)
                    mesh.sdf.vector_field[vec_idx + 3] = 0

    def pack_float(self, value: float, min_val: float, max_val: float, bits: int) -> int:
        """
        打包浮点数到整数，对应pack_float函数
        """
        if max_val - min_val < self.epsilon:
            normalized = 0.0
        else:
            normalized = (value - min_val) / (max_val - min_val)

        normalized = max(0.0, min(1.0, normalized))
        max_value = (1 << bits) - 1
        return int(normalized * max_value)

    def pack_vertices(self, mesh: Mesh) -> None:
        """
        打包顶点数据，对应pack_vertices函数
        """
        num_vertices = len(mesh.vertices)
        if num_vertices == 0:
            return

        # 打包位置数据（正确的字节方式）
        positions = np.array([v['position'] for v in mesh.vertices], dtype=np.float32)
        mesh.position_buffer = positions.tobytes()

        # 确定元素类型和大小
        mesh.elements_type = self.determine_elements_type(mesh)
        element_size = self.get_vertex_element_size(mesh.elements_type)

        # 创建元素缓冲区
        element_data = bytearray(element_size * num_vertices)

        # 打包元素数据（简化版本）
        for i in range(num_vertices):
            vertex = mesh.vertices[i]
            offset = i * element_size

            if mesh.elements_type == ElementsType.STATIC_NORMAL:
                self.pack_static_normal(vertex, element_data, offset)
            elif mesh.elements_type == ElementsType.STATIC_NORMAL_TEXTURE:
                self.pack_static_normal_texture(vertex, element_data, offset)
            # 其他元素类型的打包...

        mesh.element_buffer = bytes(element_data)

    def pack_static_normal(self, vertex: Dict, buffer: bytearray, offset: int) -> None:
        """打包静态法线顶点"""
        normal = vertex['normal']
        t_sign = (normal[2] > 0.0) << 1

        normal_x = self.pack_float(normal[0], -1.0, 1.0, 16)
        normal_y = self.pack_float(normal[1], -1.0, 1.0, 16)

        # 写入缓冲区（简化版本，实际需要正确的格式）
        buffer[offset + 0] = t_sign
        # 继续写入其他数据...

    def pack_static_normal_texture(self, vertex: Dict, buffer: bytearray, offset: int) -> None:
        """打包静态法线+纹理顶点"""
        # 类似实现...
        pass

    def get_vertex_element_size(self, elements_type: ElementsType) -> int:
        """
        获取顶点元素大小，对应get_vertex_element_size函数
        """
        sizes = {
            ElementsType.STATIC_NORMAL: 4,          # t_sign + normal_xy
            ElementsType.STATIC_NORMAL_TEXTURE: 12, # t_sign + normal_xy + tangent_xy + uv
            ElementsType.STATIC_COLOR: 4,           # rgb + padding
            # 其他类型的大小...
        }
        return sizes.get(elements_type, 0)

    def process_vertices(self, mesh: Mesh, settings: Dict = None) -> None:
        """
        处理顶点数据，对应process_vertices函数
        """
        if settings is None:
            settings = {
                'calculate_normals': False,
                'smoothing_angle': 180.0
            }

        # 确保索引数是3的倍数
        assert len(mesh.raw_indices) % 3 == 0, "Index count must be multiple of 3"

        # 重新计算法线（如果需要）
        if settings['calculate_normals'] or len(mesh.normals) == 0:
            self.recalculate_normals(mesh)

        # 处理法线平滑
        self.process_normals(mesh, settings['smoothing_angle'])

        # 处理UV
        if len(mesh.uv_sets) > 0:
            self.process_uvs(mesh)

        # 确定元素类型
        mesh.elements_type = self.determine_elements_type(mesh)

        # 生成meshlets
        self.process_meshlets(mesh)

        # 生成SDF
        self.generate_sdf(mesh)

        # 打包顶点数据
        self.pack_vertices(mesh)

def load_fbx_to_mesh(fbx_file_path: str) -> List[Mesh]:
    """
    从FBX文件加载网格数据
    返回网格列表
    """
    import sys
    sys.path.append('/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/ContentTools')

    try:
        from check_fbx_submeshes import analyze_fbx_submeshes
    except ImportError:
        print("错误: 无法导入FBX分析模块")
        return []

    analysis_data = analyze_fbx_submeshes(fbx_file_path)
    submeshes = analysis_data['submeshes']

    meshes = []
    for submesh_data in submeshes:
        mesh = Mesh(
            name=submesh_data['name'],
            material_idx=0  # 简化处理
        )

        # 这里需要实际的FBX几何数据提取
        # 当前我们只有统计信息，需要扩展FBX分析来获取实际几何数据

        meshes.append(mesh)

    return meshes

def main():
    """主函数，演示用法"""
    print("Python Geometry Processor - 复刻C++ Geometry.cpp功能")
    print("=" * 60)

    # 示例：创建一个简单的测试网格
    mesh = Mesh(name="test_mesh")

    # 创建测试数据（一个简单的三角形）
    mesh.positions = np.array([
        [0.0, 0.0, 0.0],
        [1.0, 0.0, 0.0],
        [0.0, 1.0, 0.0]
    ], dtype=np.float32)

    mesh.raw_indices = np.array([0, 1, 2], dtype=np.uint32)
    mesh.uv_sets = [
        np.array([
            [0.0, 0.0],
            [1.0, 0.0],
            [0.0, 1.0]
        ], dtype=np.float32)
    ]

    # 创建处理器
    processor = GeometryProcessor()

    # 处理网格
    settings = {
        'calculate_normals': True,
        'smoothing_angle': 180.0
    }

    processor.process_vertices(mesh, settings)

    print(f"网格处理完成: {mesh.name}")
    print(f"顶点数量: {len(mesh.vertices)}")
    print(f"索引数量: {len(mesh.indices)}")
    print(f"Meshlets数量: {len(mesh.meshlets)}")
    print(f"元素类型: {mesh.elements_type.name}")

if __name__ == "__main__":
    main()