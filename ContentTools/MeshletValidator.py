#!/usr/bin/env python3
"""
Meshlet数据验证工具
验证Geometry.cpp生成的meshlet数据的有效性和正确性
"""

import numpy as np
import struct
import sys
from pathlib import Path
from typing import Dict, List, Tuple, Optional
from dataclasses import dataclass
import matplotlib.pyplot as plt
from mpl_toolkits.mplot3d import Axes3D

@dataclass
class MeshletData:
    """Meshlet数据结构"""
    vertex_offset: int
    triangle_offset: int
    vertex_count: int
    triangle_count: int
    center: Tuple[float, float, float]
    radius: float
    cone_apex: Tuple[float, float, float]
    cone_axis: Tuple[float, float, float]
    cone_cutoff: float

@dataclass
class ProcessedMesh:
    """处理后的网格数据"""
    name: str
    vertices: np.ndarray      # [N, 3] float32
    indices: np.ndarray       # [M] uint32
    meshlets: List[MeshletData]
    meshlet_vertices: np.ndarray  # uint32
    meshlet_triangles: np.ndarray  # uint8

class MeshletValidator:
    """Meshlet数据验证器"""

    def __init__(self):
        self.errors = []
        self.warnings = []
        self.validation_results = {}

    def validate_binary_file(self, file_path: str) -> bool:
        """
        验证二进制模型文件中的meshlet数据
        """
        print(f"验证文件: {file_path}")
        print("=" * 60)

        try:
            mesh = self._read_binary_mesh(file_path)
            return self.validate_meshlet_data(mesh)
        except Exception as e:
            print(f"❌ 文件读取失败: {e}")
            return False

    def validate_meshlet_data(self, mesh: ProcessedMesh) -> bool:
        """
        验证meshlet数据的完整性和正确性
        """
        print(f"验证网格: {mesh.name}")
        print(f"顶点数: {len(mesh.vertices)}, 索引数: {len(mesh.indices)}")
        print(f"Meshlet数量: {len(mesh.meshlets)}")

        all_valid = True

        # 1. 基本结构验证
        all_valid &= self._validate_basic_structure(mesh)

        # 2. 几何有效性验证
        all_valid &= self._validate_geometry(mesh)

        # 3. 边界有效性验证
        all_valid &= self._validate_bounds(mesh)

        # 4. 三角形索引有效性验证
        all_valid &= self._validate_triangle_indices(mesh)

        # 5. 背面剔除圆锥验证
        all_valid &= self._validate_backface_cones(mesh)

        # 6. 性能特性验证
        all_valid &= self._validate_performance_characteristics(mesh)

        # 7. 覆盖率和重叠验证
        all_valid &= self._validate_coverage_and_overlap(mesh)

        # 打印验证结果
        self._print_validation_results()

        return all_valid

    def _validate_basic_structure(self, mesh: ProcessedMesh) -> bool:
        """验证基本结构"""
        print("\n📋 基本结构验证:")
        valid = True

        if len(mesh.meshlets) == 0:
            self.errors.append("❌ 没有meshlet数据")
            valid = False
        else:
            print(f"✅ Meshlet数量: {len(mesh.meshlets)}")

        if len(mesh.vertices) == 0:
            self.errors.append("❌ 没有顶点数据")
            valid = False
        else:
            print(f"✅ 顶点数量: {len(mesh.vertices)}")

        if len(mesh.indices) == 0:
            self.errors.append("❌ 没有索引数据")
            valid = False
        else:
            print(f"✅ 索引数量: {len(mesh.indices)}")

        return valid

    def _validate_geometry(self, mesh: ProcessedMesh) -> bool:
        """验证几何有效性"""
        print("\n🔺 几何有效性验证:")
        valid = True

        for i, meshlet in enumerate(mesh.meshlets):
            # 检查顶点数量是否合理
            if meshlet.vertex_count == 0:
                self.errors.append(f"❌ Meshlet {i}: 顶点数量为0")
                valid = False
            elif meshlet.vertex_count > 64:
                self.warnings.append(f"⚠️  Meshlet {i}: 顶点数量过多 ({meshlet.vertex_count} > 64)")

            # 检查三角形数量是否合理
            if meshlet.triangle_count == 0:
                self.errors.append(f"❌ Meshlet {i}: 三角形数量为0")
                valid = False
            elif meshlet.triangle_count > 124:
                self.warnings.append(f"⚠️  Meshlet {i}: 三角形数量过多 ({meshlet.triangle_count} > 124)")

            # 检查三角形数量是否是3的倍数
            if meshlet.triangle_count * 3 != len(mesh.meshlet_triangles[meshlet.triangle_offset:meshlet.triangle_offset + meshlet.triangle_count * 3]):
                self.errors.append(f"❌ Meshlet {i}: 三角形数据长度不匹配")
                valid = False

        print(f"✅ 几何基本结构: {len(mesh.meshlets)} 个meshlet")
        return valid

    def _validate_bounds(self, mesh: ProcessedMesh) -> bool:
        """验证边界数据有效性"""
        print("\n🎯 边界有效性验证:")
        valid = True

        for i, meshlet in enumerate(mesh.meshlets):
            # 检查包围球半径是否合理
            if meshlet.radius < 0:
                self.errors.append(f"❌ Meshlet {i}: 负半径 ({meshlet.radius})")
                valid = False
            elif meshlet.radius > 1000:  # 假设场景单位合理范围
                self.warnings.append(f"⚠️  Meshlet {i}: 半径过大 ({meshlet.radius})")

            # 检查中心点是否在合理范围内
            center = np.array(meshlet.center)
            if np.any(np.isnan(center)) or np.any(np.isinf(center)):
                self.errors.append(f"❌ Meshlet {i}: 中心点包含NaN或Inf")
                valid = False

            # 检查包围球是否包含所有顶点
            vertex_indices = mesh.meshlet_vertices[
                meshlet.vertex_offset:meshlet.vertex_offset + meshlet.vertex_count
            ]

            if len(vertex_indices) > 0:
                vertices = mesh.vertices[vertex_indices]
                distances = np.linalg.norm(vertices - center, axis=1)

                if not np.all(distances <= meshlet.radius * 1.01):  # 允许1%误差
                    self.errors.append(f"❌ Meshlet {i}: 包围球不包含所有顶点")
                    valid = False
                else:
                    max_distance = np.max(distances)
                    coverage = (max_distance / meshlet.radius) * 100
                    if coverage > 95:
                        print(f"✅ Meshlet {i}: 包围球覆盖率 {coverage:.1f}%")
                    else:
                        self.warnings.append(f"⚠️  Meshlet {i}: 包围球覆盖率过低 {coverage:.1f}%")

        return valid

    def _validate_triangle_indices(self, mesh: ProcessedMesh) -> bool:
        """验证三角形索引有效性"""
        print("\n🔢 三角形索引有效性验证:")
        valid = True

        for i, meshlet in enumerate(mesh.meshlets):
            # 获取meshlet的三角形数据
            triangle_start = meshlet.triangle_offset
            triangle_end = triangle_start + meshlet.triangle_count * 3
            triangles = mesh.meshlet_triangles[triangle_start:triangle_end]

            # 检查索引范围
            vertex_indices = mesh.meshlet_vertices[
                meshlet.vertex_offset:meshlet.vertex_offset + meshlet.vertex_count
            ]

            invalid_indices = triangles[triangles >= meshlet.vertex_count]
            if len(invalid_indices) > 0:
                self.errors.append(f"❌ Meshlet {i}: 包含无效顶点索引 {invalid_indices}")
                valid = False

            # 检查是否包含退化三角形
            unique_triangles = set()
            degenerate_count = 0

            for j in range(0, len(triangles), 3):
                tri = tuple(triangles[j:j+3])
                if tri[0] == tri[1] or tri[1] == tri[2] or tri[0] == tri[2]:
                    degenerate_count += 1

                if tri in unique_triangles:
                    self.warnings.append(f"⚠️  Meshlet {i}: 包含重复三角形")
                else:
                    unique_triangles.add(tri)

            if degenerate_count > 0:
                self.warnings.append(f"⚠️  Meshlet {i}: 包含 {degenerate_count} 个退化三角形")

            # 检查三角形法线一致性
            if not self._check_triangle_normals(mesh, meshlet, triangles):
                self.warnings.append(f"⚠️  Meshlet {i}: 三角形法线不一致")

        print(f"✅ 索引范围检查完成")
        return valid

    def _check_triangle_normals(self, mesh: ProcessedMesh, meshlet: MeshletData, triangles: np.ndarray) -> bool:
        """检查三角形法线一致性"""
        if len(triangles) < 6:  # 至少需要2个三角形
            return True

        vertex_indices = mesh.meshlet_vertices[
            meshlet.vertex_offset:meshlet.vertex_offset + meshlet.vertex_count
        ]

        vertices = mesh.vertices[vertex_indices]

        # 计算前几个三角形的法线
        normals = []
        for i in range(0, min(6, len(triangles)), 3):
            i0, i1, i2 = triangles[i], triangles[i+1], triangles[i+2]
            v0, v1, v2 = vertices[i0], vertices[i1], vertices[i2]

            edge1 = v1 - v0
            edge2 = v2 - v0
            normal = np.cross(edge1, edge2)
            norm = np.linalg.norm(normal)

            if norm > 1e-6:
                normals.append(normal / norm)
            else:
                return False  # 退化三角形

        # 检查法线是否大致一致（允许一定误差）
        if len(normals) >= 2:
            mean_normal = np.mean(normals, axis=0)
            for normal in normals:
                dot_product = np.dot(normal, mean_normal)
                if dot_product < 0.5:  # 约60度差异
                    return False

        return True

    def _validate_backface_cones(self, mesh: ProcessedMesh) -> bool:
        """验证背面剔除圆锥"""
        print("\n🔻 背面剔除圆锥验证:")
        valid = True

        for i, meshlet in enumerate(mesh.meshlets):
            cone_axis = np.array(meshlet.cone_axis)
            cone_cutoff = meshlet.cone_cutoff

            # 检查圆锥轴是否归一化
            axis_norm = np.linalg.norm(cone_axis)
            if abs(axis_norm - 1.0) > 0.1:
                self.warnings.append(f"⚠️  Meshlet {i}: 圆锥轴未归一化 (norm={axis_norm})")

            # 检查cutoff值是否合理
            if cone_cutoff < -1.0 or cone_cutoff > 1.0:
                self.errors.append(f"❌ Meshlet {i}: 圆锥cutoff值异常 ({cone_cutoff})")
                valid = False
            elif cone_cutoff > 0.99:  # 接近1表示几乎完全背面朝向
                self.warnings.append(f"⚠️  Meshlet {i}: 圆锥cutoff过高 ({cone_cutoff})")

            # 验证圆锥角度与三角形法线的一致性
            if not self._validate_cone_consistency(mesh, meshlet):
                self.warnings.append(f"⚠️  Meshlet {i}: 圆锥与三角形法线不一致")

        print(f"✅ 圆锥参数检查完成")
        return valid

    def _validate_cone_consistency(self, mesh: ProcessedMesh, meshlet: MeshletData) -> bool:
        """验证圆锥与三角形法线的一致性"""
        triangle_start = meshlet.triangle_offset
        triangle_end = triangle_start + meshlet.triangle_count * 3
        triangles = mesh.meshlet_triangles[triangle_start:triangle_end]

        vertex_indices = mesh.meshlet_vertices[
            meshlet.vertex_offset:meshlet.vertex_offset + meshlet.vertex_count
        ]

        vertices = mesh.vertices[vertex_indices]
        cone_axis = np.array(meshlet.cone_axis)

        # 计算平均三角形法线
        avg_normal = np.zeros(3)
        valid_triangles = 0

        for i in range(0, len(triangles), 3):
            i0, i1, i2 = triangles[i], triangles[i+1], triangles[i+2]
            v0, v1, v2 = vertices[i0], vertices[i1], vertices[i2]

            edge1 = v1 - v0
            edge2 = v2 - v0
            normal = np.cross(edge1, edge2)
            norm = np.linalg.norm(normal)

            if norm > 1e-6:
                avg_normal += normal / norm
                valid_triangles += 1

        if valid_triangles > 0:
            avg_normal /= valid_triangles
            avg_normal /= np.linalg.norm(avg_normal)

            # 检查圆锥轴与平均法线的一致性
            dot_product = np.dot(cone_axis, avg_normal)
            return dot_product > 0.5  # 允许60度偏差

        return True

    def _validate_performance_characteristics(self, mesh: ProcessedMesh) -> bool:
        """验证性能特性"""
        print("\n⚡ 性能特性验证:")
        valid = True

        # 统计meshlet大小分布
        vertex_counts = [ml.vertex_count for ml in mesh.meshlets]
        triangle_counts = [ml.triangle_count for ml in mesh.meshlets]

        avg_vertices = np.mean(vertex_counts)
        avg_triangles = np.mean(triangle_counts)

        print(f"平均顶点数: {avg_vertices:.1f}")
        print(f"平均三角形数: {avg_triangles:.1f}")

        # 检查是否有明显的不平衡
        if max(vertex_counts) / (avg_vertices + 1) > 3:
            self.warnings.append(f"⚠️  Meshlet大小不平衡: 最大{max(vertex_counts)}, 平均{avg_vertices:.1f}")

        # 检查利用率
        vertex_utilization = np.mean([ml.vertex_count / 64 for ml in mesh.meshlets])
        triangle_utilization = np.mean([ml.triangle_count / 124 for ml in mesh.meshlets])

        print(f"顶点利用率: {vertex_utilization * 100:.1f}%")
        print(f"三角形利用率: {triangle_utilization * 100:.1f}%")

        if vertex_utilization < 0.5:
            self.warnings.append(f"⚠️  顶点利用率较低: {vertex_utilization * 100:.1f}%")

        if triangle_utilization < 0.5:
            self.warnings.append(f"⚠️  三角形利用率较低: {triangle_utilization * 100:.1f}%")

        return valid

    def _validate_coverage_and_overlap(self, mesh: ProcessedMesh) -> bool:
        """验证覆盖率和重叠情况"""
        print("\n📐 覆盖率和重叠验证:")
        valid = True

        # 统计三角形引用次数
        triangle_references = {}
        total_triangles = len(mesh.indices) // 3

        for i, meshlet in enumerate(mesh.meshlets):
            triangle_start = meshlet.triangle_offset
            triangle_end = triangle_start + meshlet.triangle_count * 3
            triangles = mesh.meshlet_triangles[triangle_start:triangle_end]

            for j in range(0, len(triangles), 3):
                # 创建三角形哈希值用于比较
                tri_hash = tuple(triangles[j:j+3])
                if tri_hash in triangle_references:
                    triangle_references[tri_hash].append(i)
                else:
                    triangle_references[tri_hash] = [i]

        # 检查未覆盖的三角形
        coverage = len(triangle_references) / total_triangles if total_triangles > 0 else 0
        print(f"三角形覆盖率: {coverage * 100:.1f}%")

        if coverage < 0.95:
            self.errors.append(f"❌ 三角形覆盖率过低: {coverage * 100:.1f}%")
            valid = False

        # 检查重复三角形
        duplicate_triangles = {k: v for k, v in triangle_references.items() if len(v) > 1}
        if len(duplicate_triangles) > 0:
            duplicate_ratio = len(duplicate_triangles) / total_triangles
            print(f"重复三角形: {len(duplicate_triangles)} ({duplicate_ratio * 100:.1f}%)")

            if duplicate_ratio > 0.05:
                self.warnings.append(f"⚠️  重复三角形比例较高: {duplicate_ratio * 100:.1f}%")

        return valid

    def _print_validation_results(self):
        """打印验证结果摘要"""
        print("\n" + "=" * 60)
        print("📊 验证结果摘要")
        print("=" * 60)

        if self.errors:
            print(f"❌ 错误 ({len(self.errors)}):")
            for error in self.errors:
                print(f"  {error}")

        if self.warnings:
            print(f"\n⚠️  警告 ({len(self.warnings)}):")
            for warning in self.warnings[:10]:  # 只显示前10个
                print(f"  {warning}")
            if len(self.warnings) > 10:
                print(f"  ... 还有 {len(self.warnings) - 10} 个警告")

        if not self.errors and not self.warnings:
            print("✅ 所有验证通过！Meshlet数据完全有效")
        elif not self.errors:
            print("✅ 验证通过，但有一些优化建议")
        else:
            print("❌ 验证失败，需要修复错误")

    def _read_binary_mesh(self, file_path: str) -> ProcessedMesh:
        """读取二进制模型文件"""
        # 这里需要根据实际的二进制格式实现
        # 简化版本：假设我们知道文件格式

        with open(file_path, 'rb') as f:
            # 读取基本信息
            name_length = struct.unpack('I', f.read(4))[0]
            name = f.read(name_length).decode('utf-8')

            # 读取顶点和索引数据
            vertex_count = struct.unpack('I', f.read(4))[0]
            index_count = struct.unpack('I', f.read(4))[0]

            vertices = np.fromfile(f, dtype=np.float32, count=vertex_count * 3)
            vertices = vertices.reshape(vertex_count, 3)

            indices = np.fromfile(f, dtype=np.uint32, count=index_count)

            # 寻找MSHL标记（meshlet数据开始）
            current_pos = f.tell()
            while True:
                f.seek(current_pos)
                magic = f.read(4)
                if magic == b'MSHL' or magic == b'':
                    break
                current_pos += 1

            if magic == b'MSHL':
                # 读取meshlet数据
                meshlet_count = struct.unpack('I', f.read(4))[0]

                meshlets = []
                for _ in range(meshlet_count):
                    ml = MeshletData(
                        vertex_offset=struct.unpack('I', f.read(4))[0],
                        triangle_offset=struct.unpack('I', f.read(4))[0],
                        vertex_count=struct.unpack('I', f.read(4))[0],
                        triangle_count=struct.unpack('I', f.read(4))[0],
                        center=tuple(struct.unpack('3f', f.read(12))),
                        radius=struct.unpack('f', f.read(4))[0],
                        cone_apex=tuple(struct.unpack('3f', f.read(12))),
                        cone_axis=tuple(struct.unpack('3f', f.read(12))),
                        cone_cutoff=struct.unpack('f', f.read(4))[0]
                    )
                    meshlets.append(ml)

                # 读取meshlet顶点和三角形数据
                meshlet_vertex_count = struct.unpack('I', f.read(4))[0]
                meshlet_vertices = np.fromfile(f, dtype=np.uint32, count=meshlet_vertex_count)

                meshlet_triangle_count = struct.unpack('I', f.read(4))[0]
                meshlet_triangles = np.fromfile(f, dtype=np.uint8, count=meshlet_triangle_count)

                return ProcessedMesh(
                    name=name,
                    vertices=vertices,
                    indices=indices,
                    meshlets=meshlets,
                    meshlet_vertices=meshlet_vertices,
                    meshlet_triangles=meshlet_triangles
                )

        raise ValueError("无法找到有效的meshlet数据")

def main():
    """主函数"""
    if len(sys.argv) < 2:
        print("用法: python MeshletValidator.py <model_file>")
        print("示例: python MeshletValidator.py Sponza.model")
        sys.exit(1)

    validator = MeshletValidator()
    is_valid = validator.validate_binary_file(sys.argv[1])

    if is_valid:
        print("\n✅ Meshlet数据验证通过！")
        sys.exit(0)
    else:
        print("\n❌ Meshlet数据验证失败！")
        sys.exit(1)

if __name__ == "__main__":
    main()