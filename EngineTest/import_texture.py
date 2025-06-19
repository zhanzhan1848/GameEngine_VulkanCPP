#!/usr/bin/env python3
# -*- coding: utf-8 -*-

'''
纹理导入工具

此脚本用于导入纹理文件并转换为引擎兼容格式。
支持常见图像格式(PNG, JPG, TGA等)的导入，并可生成mipmap和压缩纹理。
'''

import struct
import array
import os
import sys
import zlib
from enum import IntEnum
from PIL import Image
import numpy as np

class MTLPixelFormat(IntEnum):
    MTLPixelFormatInvalid = 0
    MTLPixelFormatA8Unorm = 1
    MTLPixelFormatR8Unorm = 10
    MTLPixelFormatR8Unorm_sRGB = 11
    MTLPixelFormatR8Snorm = 12
    MTLPixelFormatR8Uint = 13
    MTLPixelFormatR8Sint = 14
    MTLPixelFormatR16Unorm = 20
    MTLPixelFormatR16Snorm = 22
    MTLPixelFormatR16Uint = 23
    MTLPixelFormatR16Sint = 24
    MTLPixelFormatR16Float = 25
    MTLPixelFormatRG8Unorm = 30
    MTLPixelFormatRG8Unorm_sRGB = 31
    MTLPixelFormatRG8Snorm = 32
    MTLPixelFormatRG8Uint = 33
    MTLPixelFormatRG8Sint = 34
    MTLPixelFormatB5G6R5Unorm = 40
    MTLPixelFormatA1BGR5Unorm = 41
    MTLPixelFormatABGR4Unorm = 42
    MTLPixelFormatBGR5A1Unorm = 43
    MTLPixelFormatR32Uint = 53
    MTLPixelFormatR32Sint = 54
    MTLPixelFormatR32Float = 55
    MTLPixelFormatRG16Unorm = 60
    MTLPixelFormatRG16Snorm = 62
    MTLPixelFormatRG16Uint = 63
    MTLPixelFormatRG16Sint = 64
    MTLPixelFormatRG16Float = 65
    MTLPixelFormatRGBA8Unorm = 70
    MTLPixelFormatRGBA8Unorm_sRGB = 71
    MTLPixelFormatRGBA8Snorm = 72
    MTLPixelFormatRGBA8Uint = 73
    MTLPixelFormatRGBA8Sint = 74
    MTLPixelFormatBGRA8Unorm = 80
    MTLPixelFormatBGRA8Unorm_sRGB = 81
    MTLPixelFormatRGB10A2Unorm = 90
    MTLPixelFormatRGB10A2Uint = 91
    MTLPixelFormatRG11B10Float = 92
    MTLPixelFormatRGB9E5Float = 93
    MTLPixelFormatBGR10A2Unorm = 94
    MTLPixelFormatRG32Uint = 103
    MTLPixelFormatRG32Sint = 104
    MTLPixelFormatRG32Float = 105
    MTLPixelFormatRGBA16Unorm = 110
    MTLPixelFormatRGBA16Snorm = 112
    MTLPixelFormatRGBA16Uint = 113
    MTLPixelFormatRGBA16Sint = 114
    MTLPixelFormatRGBA16Float = 115
    MTLPixelFormatRGBA32Uint = 123
    MTLPixelFormatRGBA32Sint = 124
    MTLPixelFormatRGBA32Float = 125
    MTLPixelFormatBC1_RGBA = 130
    MTLPixelFormatBC1_RGBA_sRGB = 131
    MTLPixelFormatBC2_RGBA = 132
    MTLPixelFormatBC2_RGBA_sRGB = 133
    MTLPixelFormatBC3_RGBA = 134
    MTLPixelFormatBC3_RGBA_sRGB = 135
    MTLPixelFormatBC4_RUnorm = 140
    MTLPixelFormatBC4_RSnorm = 141
    MTLPixelFormatBC5_RGUnorm = 142
    MTLPixelFormatBC5_RGSnorm = 143
    MTLPixelFormatBC6H_RGBFloat = 150
    MTLPixelFormatBC6H_RGBUfloat = 151
    MTLPixelFormatBC7_RGBAUnorm = 152
    MTLPixelFormatBC7_RGBAUnorm_sRGB = 153
    MTLPixelFormatPVRTC_RGB_2BPP = 160
    MTLPixelFormatPVRTC_RGB_2BPP_sRGB = 161
    MTLPixelFormatPVRTC_RGB_4BPP = 162
    MTLPixelFormatPVRTC_RGB_4BPP_sRGB = 163
    MTLPixelFormatPVRTC_RGBA_2BPP = 164
    MTLPixelFormatPVRTC_RGBA_2BPP_sRGB = 165
    MTLPixelFormatPVRTC_RGBA_4BPP = 166
    MTLPixelFormatPVRTC_RGBA_4BPP_sRGB = 167
    MTLPixelFormatEAC_R11Unorm = 170
    MTLPixelFormatEAC_R11Snorm = 172
    MTLPixelFormatEAC_RG11Unorm = 174
    MTLPixelFormatEAC_RG11Snorm = 176
    MTLPixelFormatEAC_RGBA8 = 178
    MTLPixelFormatEAC_RGBA8_sRGB = 179
    MTLPixelFormatETC2_RGB8 = 180
    MTLPixelFormatETC2_RGB8_sRGB = 181
    MTLPixelFormatETC2_RGB8A1 = 182
    MTLPixelFormatETC2_RGB8A1_sRGB = 183
    MTLPixelFormatASTC_4x4_sRGB = 186
    MTLPixelFormatASTC_5x4_sRGB = 187
    MTLPixelFormatASTC_5x5_sRGB = 188
    MTLPixelFormatASTC_6x5_sRGB = 189
    MTLPixelFormatASTC_6x6_sRGB = 190
    MTLPixelFormatASTC_8x5_sRGB = 192
    MTLPixelFormatASTC_8x6_sRGB = 193
    MTLPixelFormatASTC_8x8_sRGB = 194
    MTLPixelFormatASTC_10x5_sRGB = 195
    MTLPixelFormatASTC_10x6_sRGB = 196
    MTLPixelFormatASTC_10x8_sRGB = 197
    MTLPixelFormatASTC_10x10_sRGB = 198
    MTLPixelFormatASTC_12x10_sRGB = 199
    MTLPixelFormatASTC_12x12_sRGB = 200
    MTLPixelFormatASTC_4x4_LDR = 204
    MTLPixelFormatASTC_5x4_LDR = 205
    MTLPixelFormatASTC_5x5_LDR = 206
    MTLPixelFormatASTC_6x5_LDR = 207
    MTLPixelFormatASTC_6x6_LDR = 208
    MTLPixelFormatASTC_8x5_LDR = 210
    MTLPixelFormatASTC_8x6_LDR = 211
    MTLPixelFormatASTC_8x8_LDR = 212
    MTLPixelFormatASTC_10x5_LDR = 213
    MTLPixelFormatASTC_10x6_LDR = 214
    MTLPixelFormatASTC_10x8_LDR = 215
    MTLPixelFormatASTC_10x10_LDR = 216
    MTLPixelFormatASTC_12x10_LDR = 217
    MTLPixelFormatASTC_12x12_LDR = 218
    MTLPixelFormatASTC_4x4_HDR = 222
    MTLPixelFormatASTC_5x4_HDR = 223
    MTLPixelFormatASTC_5x5_HDR = 224
    MTLPixelFormatASTC_6x5_HDR = 225
    MTLPixelFormatASTC_6x6_HDR = 226
    MTLPixelFormatASTC_8x5_HDR = 228
    MTLPixelFormatASTC_8x6_HDR = 229
    MTLPixelFormatASTC_8x8_HDR = 230
    MTLPixelFormatASTC_10x5_HDR = 231
    MTLPixelFormatASTC_10x6_HDR = 232
    MTLPixelFormatASTC_10x8_HDR = 233
    MTLPixelFormatASTC_10x10_HDR = 234
    MTLPixelFormatASTC_12x10_HDR = 235
    MTLPixelFormatASTC_12x12_HDR = 236
    MTLPixelFormatGBGR422 = 240
    MTLPixelFormatBGRG422 = 241
    MTLPixelFormatDepth16Unorm = 250
    MTLPixelFormatDepth32Float = 252
    MTLPixelFormatStencil8 = 253
    MTLPixelFormatDepth24Unorm_Stencil8 = 255
    MTLPixelFormatDepth32Float_Stencil8 = 260
    MTLPixelFormatX32_Stencil8 = 261
    MTLPixelFormatX24_Stencil8 = 262
    MTLPixelFormatBGRA10_XR = 552
    MTLPixelFormatBGRA10_XR_sRGB = 553
    MTLPixelFormatBGR10_XR = 554
    MTLPixelFormatBGR10_XR_sRGB = 555

class TextureFlags(IntEnum):
    IsHDR = 0x01
    HasAlpha = 0x02
    IsPremultipliedAlpha = 0x04
    IsNormalMap = 0x08
    IsCubeMap = 0x10
    IsVolumeMap = 0x20
    IsSRGB = 0x40

class TextureDimension(IntEnum):
    Texture2D = 0
    TextureCube = 1
    Texture3D = 2

class TextureImportError(IntEnum):
    Succeeded = 0
    Unknown = 1
    Compress = 2
    Decompress = 3
    Load = 4
    MipmapGeneration = 5
    MaxSizeExceeded = 6
    SizeMismatch = 7
    FormatMismatch = 8
    FileNotFound = 9
    NeedSixImages = 10

class Slice:
    '''
    表示纹理的一个切片（可以是一个mipmap级别或一个数组元素）
    '''
    def __init__(self, width=0, height=0, raw_content=None):
        self.width = width
        self.height = height
        self.raw_content = raw_content if raw_content is not None else bytearray()
        self.row_pitch = width * 4  # 默认RGBA格式，每像素4字节
        self.slice_pitch = self.row_pitch * height
    
    def from_image(self, image):
        '''
        从PIL图像创建切片
        '''
        self.width = image.width
        self.height = image.height
        
        # 确保图像是RGBA格式
        if image.mode != 'RGBA':
            image = image.convert('RGBA')
        
        # 获取原始像素数据
        self.raw_content = bytearray(image.tobytes())
        self.row_pitch = self.width * 4
        self.slice_pitch = self.row_pitch * self.height
        
        return self
    
    def to_image(self):
        '''
        将切片转换为PIL图像
        '''
        return Image.frombytes('RGBA', (self.width, self.height), bytes(self.raw_content))

class TextureImportSettings:
    '''
    纹理导入设置
    '''
    def __init__(self):
        self.sources = []  # 源文件路径列表
        self.dimension = TextureDimension.Texture2D  # 纹理维度
        self.mip_levels = 0  # 0表示生成完整的mipmap链
        self.alpha_threshold = 0.5  # Alpha阈值，用于确定是否有Alpha通道
        self.prefer_bc7 = True  # 是否优先使用BC7压缩格式
        self.output_format = MTLPixelFormat.MTLPixelFormatRGBA8Unorm  # 输出格式
        self.compress = True  # 是否压缩

class Texture:
    '''
    表示一个纹理资源
    '''
    MAX_MIP_LEVELS = 14  # 最大mipmap级别数
    MAX_ARRAY_SIZE = 2048  # 最大数组大小
    MAX_3D_SIZE = 2048  # 3D纹理的最大尺寸
    
    def __init__(self):
        self.width = 0
        self.height = 0
        self.array_size = 1
        self.flags = 0
        self.mip_levels = 1
        self.format = MTLPixelFormat.MTLPixelFormatRGBA8Unorm
        self.slices = []  # 三维列表：[array_slice][mip_level][depth_slice]
        self.import_settings = TextureImportSettings()
    
    def has_valid_dimensions(self, file_path):
        '''
        检查纹理尺寸是否有效
        '''
        width = self.width
        height = self.height
        array_or_depth = self.array_size
        is_3d = bool(self.flags & TextureFlags.IsVolumeMap)
        
        result = True
        
        if width > (1 << self.MAX_MIP_LEVELS) or height > (1 << self.MAX_MIP_LEVELS):
            print(f"Error: Image dimensions greater than {1 << self.MAX_MIP_LEVELS}! (file: {file_path})")
            result = False
        
        if width % 4 != 0 or height % 4 != 0:
            print(f"Error: Image dimensions not a multiple of 4! (file: {file_path})")
            result = False
        
        if is_3d and (width > self.MAX_3D_SIZE or height > self.MAX_3D_SIZE or array_or_depth > self.MAX_3D_SIZE):
            print(f"Error: 3D texture dimensions greater than {self.MAX_3D_SIZE}! (file: {file_path})")
            result = False
        elif array_or_depth > self.MAX_ARRAY_SIZE:
            print(f"Error: 2D texture array size greater than {self.MAX_ARRAY_SIZE}! (file: {file_path})")
            result = False
        
        if width != height:
            print(f"Warning: Non-square image (width and height not equal)! (file: {file_path})")
        
        if not is_power_of_2(width) or not is_power_of_2(height):
            print(f"Warning: Image dimensions not power of 2! (file: {file_path})")
        
        return result
    
    def import_texture(self, file_path):
        '''
        从文件导入纹理
        '''
        if not os.path.exists(file_path):
            print(f"Error: File not found: {file_path}")
            return False
        
        try:
            print(f"Importing image file {file_path}")
            
            # 添加到源文件列表
            self.import_settings.sources.append(file_path)
            
            # 加载图像
            image = Image.open(file_path)
            
            # 检查是否有Alpha通道
            has_alpha = 'A' in image.getbands()
            if has_alpha:
                self.flags |= TextureFlags.HasAlpha
            
            # 检查是否是HDR图像
            is_hdr = file_path.lower().endswith(('.hdr', '.exr'))
            if is_hdr:
                self.flags |= TextureFlags.IsHDR
            
            # 设置基本属性
            self.width = image.width
            self.height = image.height
            
            # 创建切片
            base_slice = Slice().from_image(image)
            
            # 生成mipmap链
            mip_chain = self.generate_mip_chain(base_slice)
            
            # 设置mipmap级别数
            self.mip_levels = len(mip_chain)
            
            # 构建切片数组
            self.slices = [[mip_chain]]
            
            # 检查尺寸是否有效
            if not self.has_valid_dimensions(file_path):
                return False
            
            return True
        except Exception as e:
            print(f"Failed to read {file_path} for import: {str(e)}")
            return False
    
    def generate_mip_chain(self, base_slice):
        '''
        生成mipmap链
        '''
        mip_chain = [base_slice]
        width = base_slice.width
        height = base_slice.height
        
        # 计算可以生成的最大mipmap级别数
        max_levels = min(self.MAX_MIP_LEVELS, 1 + int(np.log2(max(width, height))))
        
        # 如果设置了特定的mipmap级别数，使用它
        target_levels = self.import_settings.mip_levels if self.import_settings.mip_levels > 0 else max_levels
        target_levels = min(target_levels, max_levels)
        
        # 生成mipmap链
        current_image = base_slice.to_image()
        
        for i in range(1, target_levels):
            new_width = max(1, width >> i)
            new_height = max(1, height >> i)
            
            # 调整图像大小
            resized_image = current_image.resize((new_width, new_height), Image.LANCZOS)
            
            # 创建新的切片
            mip_slice = Slice().from_image(resized_image)
            mip_chain.append(mip_slice)
            
            # 更新当前图像
            current_image = resized_image
        
        return mip_chain
    
    def save(self, output_path):
        '''
        将纹理保存为引擎兼容的二进制格式
        '''
        try:
            # 压缩内容
            compressed_data = self.compress_content()
            
            with open(output_path, 'wb') as f:
                # 写入文件头
                f.write(b'TEXR')  # 文件标识符
                f.write(struct.pack('I', 1))  # 版本号
                
                # 写入导入设置
                sources_str = ';'.join(self.import_settings.sources)
                f.write(struct.pack('I', len(sources_str)))  # 源文件路径字符串长度
                f.write(sources_str.encode('utf-8'))  # 源文件路径
                f.write(struct.pack('I', len(self.import_settings.sources)))  # 源文件数量
                f.write(struct.pack('I', int(self.import_settings.dimension)))  # 纹理维度
                f.write(struct.pack('I', self.import_settings.mip_levels))  # Mipmap级别数
                f.write(struct.pack('f', self.import_settings.alpha_threshold))  # Alpha阈值
                f.write(struct.pack('I', 1 if self.import_settings.prefer_bc7 else 0))  # 是否优先使用BC7
                f.write(struct.pack('I', int(self.import_settings.output_format)))  # 输出格式
                f.write(struct.pack('I', 1 if self.import_settings.compress else 0))  # 是否压缩
                
                # 写入纹理信息
                f.write(struct.pack('I', self.width))  # 宽度
                f.write(struct.pack('I', self.height))  # 高度
                f.write(struct.pack('I', self.array_size))  # 数组大小
                f.write(struct.pack('I', int(self.flags)))  # 标志
                f.write(struct.pack('I', self.mip_levels))  # Mipmap级别数
                f.write(struct.pack('I', int(self.format)))  # 格式
                
                # 写入压缩数据
                f.write(struct.pack('I', len(compressed_data)))  # 压缩数据长度
                f.write(compressed_data)  # 压缩数据
            
            print(f"Saved texture to {output_path}")
            return True
        except Exception as e:
            print(f"Failed to save texture to {output_path}: {str(e)}")
            return False
    
    def compress_content(self):
        '''
        压缩纹理内容
        '''
        # 将切片转换为二进制数据
        binary_data = self.slices_to_binary()
        
        # 使用zlib压缩
        compressed_data = zlib.compress(binary_data)
        
        return compressed_data
    
    def decompress_content(self, compressed_data):
        '''
        解压缩纹理内容
        '''
        # 使用zlib解压缩
        decompressed_data = zlib.decompress(compressed_data)
        
        # 从二进制数据恢复切片
        self.slices = self.slices_from_binary(decompressed_data)
    
    def slices_to_binary(self):
        '''
        将切片转换为二进制数据
        '''
        data = bytearray()
        
        for array_slice in self.slices:
            for mip_level in array_slice:
                for slice_obj in mip_level:
                    # 写入切片信息
                    data.extend(struct.pack('I', slice_obj.width))  # 宽度
                    data.extend(struct.pack('I', slice_obj.height))  # 高度
                    data.extend(struct.pack('I', slice_obj.row_pitch))  # 行间距
                    data.extend(struct.pack('I', slice_obj.slice_pitch))  # 切片间距
                    data.extend(slice_obj.raw_content)  # 原始内容
        
        return data
    
    def slices_from_binary(self, binary_data):
        '''
        从二进制数据恢复切片
        '''
        slices = []
        offset = 0
        
        # 计算每个mip级别的深度
        depth_per_mip_level = [1] * self.mip_levels
        
        if self.flags & TextureFlags.IsVolumeMap:
            depth = self.array_size
            array_size = 1
            for i in range(self.mip_levels):
                depth_per_mip_level[i] = depth
                depth = max(depth >> 1, 1)
        else:
            array_size = self.array_size
        
        for i in range(array_size):
            array_slice = []
            for j in range(self.mip_levels):
                mip_slice = []
                for k in range(depth_per_mip_level[j]):
                    # 读取切片信息
                    width = struct.unpack('I', binary_data[offset:offset+4])[0]
                    offset += 4
                    height = struct.unpack('I', binary_data[offset:offset+4])[0]
                    offset += 4
                    row_pitch = struct.unpack('I', binary_data[offset:offset+4])[0]
                    offset += 4
                    slice_pitch = struct.unpack('I', binary_data[offset:offset+4])[0]
                    offset += 4
                    
                    # 创建切片对象
                    slice_obj = Slice(width, height)
                    slice_obj.row_pitch = row_pitch
                    slice_obj.slice_pitch = slice_pitch
                    slice_obj.raw_content = binary_data[offset:offset+slice_pitch]
                    offset += slice_pitch
                    
                    mip_slice.append(slice_obj)
                array_slice.append(mip_slice)
            slices.append(array_slice)
        
        return slices
    
    def load(self, file_path):
        '''
        从引擎格式文件加载纹理
        '''
        if not os.path.exists(file_path):
            print(f"Error: File not found: {file_path}")
            return False
        
        try:
            with open(file_path, 'rb') as f:
                # 读取文件头
                file_id = f.read(4)
                if file_id != b'TEXR':
                    print(f"Error: Invalid texture file format: {file_path}")
                    return False
                
                version = struct.unpack('I', f.read(4))[0]
                if version != 1:
                    print(f"Error: Unsupported texture file version: {version}")
                    return False
                
                # 读取导入设置
                sources_str_len = struct.unpack('I', f.read(4))[0]
                sources_str = f.read(sources_str_len).decode('utf-8')
                self.import_settings.sources = sources_str.split(';') if sources_str else []
                
                self.import_settings.sources_count = struct.unpack('I', f.read(4))[0]
                self.import_settings.dimension = TextureDimension(struct.unpack('I', f.read(4))[0])
                self.import_settings.mip_levels = struct.unpack('I', f.read(4))[0]
                self.import_settings.alpha_threshold = struct.unpack('f', f.read(4))[0]
                self.import_settings.prefer_bc7 = bool(struct.unpack('I', f.read(4))[0])
                self.import_settings.output_format = DXGI_FORMAT(struct.unpack('I', f.read(4))[0])
                self.import_settings.compress = bool(struct.unpack('I', f.read(4))[0])
                
                # 读取纹理信息
                self.width = struct.unpack('I', f.read(4))[0]
                self.height = struct.unpack('I', f.read(4))[0]
                self.array_size = struct.unpack('I', f.read(4))[0]
                self.flags = TextureFlags(struct.unpack('I', f.read(4))[0])
                self.mip_levels = struct.unpack('I', f.read(4))[0]
                self.format = DXGI_FORMAT(struct.unpack('I', f.read(4))[0])
                
                # 读取压缩数据
                compressed_len = struct.unpack('I', f.read(4))[0]
                compressed_data = f.read(compressed_len)
                
                # 解压缩内容
                self.decompress_content(compressed_data)
                
                # 检查尺寸是否有效
                self.has_valid_dimensions(file_path)
                
                return True
        except Exception as e:
            print(f"Failed to load texture from {file_path}: {str(e)}")
            return False

# 工具函数
def is_power_of_2(n):
    '''
    检查一个数是否是2的幂
    '''
    return n > 0 and (n & (n - 1)) == 0

def import_texture(input_path, output_path):
    '''
    导入纹理文件
    
    参数:
        input_path: 输入纹理文件路径
        output_path: 输出文件路径
    '''
    # 创建纹理对象
    texture = Texture()
    
    # 导入纹理
    if not texture.import_texture(input_path):
        print(f"Failed to import texture from {input_path}")
        return False
    
    # 保存纹理
    if not texture.save(output_path):
        print(f"Failed to save texture to {output_path}")
        return False
    
    print(f"Successfully imported texture from {input_path} and saved to {output_path}")
    return True

def main():
    '''
    主函数
    '''
    
    input_path = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/fbx_textures/Lion_Albedo.png"
    output_path = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/fbx_textures_encode/Lion_Albedo.asset"
    
    import_texture(input_path, output_path)

if __name__ == "__main__":
    main()