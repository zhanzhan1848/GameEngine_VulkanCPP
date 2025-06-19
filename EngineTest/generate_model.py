#!/usr/bin/env python3
# -*- coding: utf-8 -*-

'''
正方体模型生成器

此脚本用于生成一个简单的正方体模型的二进制文件，格式与游戏引擎兼容。
生成的文件包含顶点位置、法线和索引数据，使用三角形列表拓扑结构。
'''

import struct
import array
import os

# 枚举定义，与引擎中保持一致
class ElementsType:
    Position = 0x00
    Normals = 0x01
    TSpace = 0x03
    Joints = 0x04
    Colors = 0x08

class PrimitiveTopology:
    PointList = 1
    LineList = 2
    LineStrip = 3
    TriangleList = 4
    TriangleStrip = 5

# 数学工具函数
def align_size_up(size, alignment):
    '''
    将大小向上对齐到指定的边界
    
    参数:
        size: 原始大小
        alignment: 对齐边界，必须是2的幂
    
    返回:
        对齐后的大小
    '''
    mask = alignment - 1
    return (size + mask) & ~mask

def create_cube_model(output_path="model.model"):
    '''
    创建一个正方体模型并保存为二进制文件
    
    参数:
        output_path: 输出文件路径
    '''
    # 创建一个简单的正方体
    # 8个顶点，12个三角形（36个索引）
    cube_vertices = [
        # 前面 (z = 0.5)
        -0.5, -0.5, 0.5,  # 0
        0.5, -0.5, 0.5,   # 1
        0.5, 0.5, 0.5,    # 2
        -0.5, 0.5, 0.5,   # 3
        
        # 后面 (z = -0.5)
        -0.5, -0.5, -0.5, # 4
        0.5, -0.5, -0.5,  # 5
        0.5, 0.5, -0.5,   # 6
        -0.5, 0.5, -0.5   # 7
    ]

    # 法线数据
    cube_normals = [
        # 前面 (z = 0.5) - 法线 (0, 0, 1)
        0.0, 0.0, 1.0,
        0.0, 0.0, 1.0,
        0.0, 0.0, 1.0,
        0.0, 0.0, 1.0,
        
        # 后面 (z = -0.5) - 法线 (0, 0, -1)
        0.0, 0.0, -1.0,
        0.0, 0.0, -1.0,
        0.0, 0.0, -1.0,
        0.0, 0.0, -1.0
    ]

    # 索引数据 - 12个三角形，36个索引
    cube_indices = [
        # 前面 (z = 0.5)
        0, 1, 2, 0, 2, 3,
        
        # 右面 (x = 0.5)
        1, 5, 6, 1, 6, 2,
        
        # 后面 (z = -0.5)
        5, 4, 7, 5, 7, 6,
        
        # 左面 (x = -0.5)
        4, 0, 3, 4, 3, 7,
        
        # 上面 (y = 0.5)
        3, 2, 6, 3, 6, 7,
        
        # 下面 (y = -0.5)
        4, 5, 1, 4, 1, 0
    ]

    # 将顶点和法线数据转换为字节数组
    positions_buffer = array.array('f', cube_vertices).tobytes()
    normals_buffer = array.array('f', cube_normals).tobytes()
    indices_buffer = array.array('H', cube_indices).tobytes()

    # 创建二进制文件
    with open(output_path, 'wb') as f:
        # 写入LOD数量（只有一个LOD）
        f.write(struct.pack('i', 1))
        
        # 写入LOD阈值
        f.write(struct.pack('f', 0.0))
        
        # 写入子网格数量（只有一个子网格）
        f.write(struct.pack('i', 1))
        
        # 记录子网格大小位置
        size_of_submeshes_position = f.tell()
        f.write(struct.pack('i', 0))  # 先写入0，稍后更新
        
        # 写入元素大小（每个顶点的法线大小：3个float）
        f.write(struct.pack('i', 4 * 3))  # 每个顶点的法线大小
        
        # 写入顶点数量
        f.write(struct.pack('i', 8))  # 8个顶点
        
        # 写入索引数量
        f.write(struct.pack('i', 36))  # 36个索引
        
        # 写入元素类型（法线）
        f.write(struct.pack('i', ElementsType.Normals))
        
        # 写入图元拓扑结构（三角形列表）
        f.write(struct.pack('i', PrimitiveTopology.TriangleList))
        
        # 对齐位置缓冲区大小
        aligned_position_buffer = bytearray(align_size_up(len(positions_buffer), 4))
        aligned_position_buffer[:len(positions_buffer)] = positions_buffer
        
        # 对齐元素缓冲区大小
        aligned_element_buffer = bytearray(align_size_up(len(normals_buffer), 4))
        aligned_element_buffer[:len(normals_buffer)] = normals_buffer
        
        # 写入位置缓冲区
        f.write(aligned_position_buffer)
        
        # 写入元素缓冲区（法线）
        f.write(aligned_element_buffer)
        
        # 写入索引缓冲区
        f.write(indices_buffer)
        
        # 计算并更新子网格大小
        end_of_submeshes = f.tell()
        size_of_submeshes = end_of_submeshes - size_of_submeshes_position - 4  # 4是int的大小
        
        f.seek(size_of_submeshes_position)
        f.write(struct.pack('i', size_of_submeshes))
        f.seek(end_of_submeshes)  # 返回到文件末尾

    print(f'已成功生成正方体模型文件：{output_path}')
    return True

if __name__ == "__main__":
    # 在当前目录生成model.model文件
    create_cube_model()
    
    # 如果需要在引擎测试目录生成，可以使用以下路径
    # engine_test_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 
    #                               "EngineTest", "assets", "model.model")
    # create_cube_model(engine_test_path)