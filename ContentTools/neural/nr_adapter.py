#!/usr/bin/env python
"""nr_adapter.py — R1.2：引擎 7ch GI → NVIDIA NR transformer 16ch 输入适配。

16ch 契约（Docs/2026-09-22-neural-rendering-inference-training-plan.md 附录 A，
源 = MLX-DLSS python/mlxdlss/features.py）：
    0-2  确定性高斯噪声（网络像素坐标 + frame_index hash 流，half 舍入）
    3    常数 1
    4-6  缩放颜色 ((half(c) - 0.5) * 0.125)，c ∈ [0,1]
    7-9  同 4-6 副本
    10-15 控制位（standard 档：0, 1, 1, -1, -1, 0）

GI 映射：gi_noisy（线性 HDR）→ tonemap `1-exp(-k*c)` 到 [0,1] → scaled_color；
方案 C（零训练）丢弃 normal/depth；方案 B（微调）在前置 7→16 投影层学习注入。
网络 extent ≥320 且为 64 的倍数：镜像延伸（不重复边缘）后运行、裁回输出。

本文件自包含（噪声/缩放/几何逻辑从 MLX-DLSS features.py 等价移植，BSD 风格
参考实现），供离线评估与 R3.2 微调共用。
"""
from __future__ import annotations

import numpy as np

MINIMUM_EXTENT = 320
EXTENT_MULTIPLE = 64
FEATURE_CHANNELS = 16

# 与采集预览一致的曝光（sponza_reference.py / make_dataset.py 预览同款曲线）
DEFAULT_EXPOSURE = 1.6


def half(value) -> np.ndarray:
    return np.asarray(value, dtype=np.float32).astype(np.float16).astype(np.float32)


# ---- 确定性噪声（features.py 等价移植）----

def _dynamic_shift_mix(value: np.ndarray) -> np.ndarray:
    value = value.astype(np.uint32, copy=False)
    shift = (value >> np.uint32(28)) + np.uint32(4)
    mixed = value ^ (value >> shift)
    return (mixed * np.uint32(0x108EF2D9)).astype(np.uint32)


def _uniform24(value: np.ndarray) -> np.ndarray:
    mixed = _dynamic_shift_mix(value)
    bits = (mixed >> np.uint32(30)) ^ (mixed >> np.uint32(8))
    return (bits + np.uint32(1)).astype(np.float32) * np.float32(5.960464477539063e-8)


def deterministic_noise(height: int, width: int, frame_index: int = 0) -> np.ndarray:
    yy, xx = np.indices((height, width), dtype=np.uint32)
    seed = yy * np.uint32(0xD8163841)
    seed ^= xx * np.uint32(0x8DA6B343)
    seed ^= np.uint32((int(frame_index) * 0x9E3779B9) & 0xFFFFFFFF)
    seed ^= np.uint32(0x243F6A88)
    multiplied = _dynamic_shift_mix(seed)
    mixed = multiplied ^ (multiplied >> np.uint32(22))
    radius_a_uniform = _uniform24(mixed * np.uint32(0xCAA5B80D) + np.uint32(0x21DD796B))
    angle_b_uniform = _uniform24(mixed * np.uint32(0x83232C31) + np.uint32(0x3463E0AC))
    radius_b_uniform = _uniform24(mixed * np.uint32(0x2C9277B5) + np.uint32(0xAC564B05))
    angle_a_uniform = _uniform24(mixed * np.uint32(0xFA6DC5F9) + np.uint32(0x4712A88E))
    radius_a = np.sqrt(np.float32(-2) * np.log(radius_a_uniform)).astype(np.float32)
    radius_b = np.sqrt(np.float32(-2) * np.log(radius_b_uniform)).astype(np.float32)
    tau = np.float32(6.2831854820251465)
    angle_a = (tau * angle_a_uniform).astype(np.float32)
    angle_b = (tau * angle_b_uniform).astype(np.float32)
    return np.stack(
        (
            half(radius_b * np.cos(angle_a)),
            half(radius_b * np.sin(angle_a)),
            half(radius_a * np.cos(angle_b)),
        ),
        axis=-1,
    )


# ---- 几何（vendor 对齐 + 镜像延伸）----

class NetworkGeometry:
    def __init__(self, output_width: int, output_height: int,
                 network_width: int, network_height: int):
        self.output_width = output_width
        self.output_height = output_height
        self.network_width = network_width
        self.network_height = network_height

    @staticmethod
    def aligned_extent(extent: int) -> int:
        minimum = max(MINIMUM_EXTENT, extent)
        return (minimum + EXTENT_MULTIPLE - 1) // EXTENT_MULTIPLE * EXTENT_MULTIPLE

    @classmethod
    def vendor_aligned(cls, width: int, height: int) -> "NetworkGeometry":
        return cls(width, height, cls.aligned_extent(width), cls.aligned_extent(height))

    @staticmethod
    def _extended_indices(count: int, extent: int) -> np.ndarray:
        index = np.arange(count)
        return np.where(index < extent, index, np.maximum(0, 2 * extent - 2 - index))

    @property
    def source_rows(self) -> np.ndarray:
        return self._extended_indices(self.network_height, self.output_height)

    @property
    def source_columns(self) -> np.ndarray:
        return self._extended_indices(self.network_width, self.output_width)

    def extend(self, image: np.ndarray) -> np.ndarray:
        """(H,W,C) 图像贴原点、右/下镜像延伸到网络 extent。"""
        return image[self.source_rows[:, None], self.source_columns[None, :], :]

    def crop(self, array: np.ndarray) -> np.ndarray:
        return array[: self.output_height, : self.output_width]


# ---- GI → 16ch 特征 ----

def tonemap_to_ldr(gi_linear_hdr: np.ndarray, exposure: float = DEFAULT_EXPOSURE) -> np.ndarray:
    """线性 HDR GI → [0,1] LDR（与采集预览同款 filmic 简化曲线）。"""
    return (1.0 - np.exp(-np.asarray(gi_linear_hdr, np.float32) * np.float32(exposure))
            ).clip(0.0, 1.0).astype(np.float32)


def scaled_color(color01: np.ndarray) -> np.ndarray:
    sampled = half(color01)
    centered = half(sampled - np.float32(0.5))
    return half(centered * np.float32(0.125))


def build_nr_features(gi_noisy_linear_hdr: np.ndarray, *,
                      frame_index: int = 0,
                      exposure: float = DEFAULT_EXPOSURE,
                      geometry: NetworkGeometry | None = None) -> tuple[np.ndarray, NetworkGeometry]:
    """7ch GI 的颜色平面 → (nH, nW, 16) vendor 特征（方案 C 适配）。

    返回 (features, geometry)；调用方负责运行后 geometry.crop() 裁回。
    """
    color01 = tonemap_to_ldr(gi_noisy_linear_hdr, exposure)
    h, w = color01.shape[:2]
    if geometry is None:
        geometry = NetworkGeometry.vendor_aligned(w, h)
    extended = geometry.extend(color01)
    features = np.zeros(
        (geometry.network_height, geometry.network_width, FEATURE_CHANNELS), np.float32)
    features[..., 0:3] = deterministic_noise(geometry.network_height, geometry.network_width,
                                             frame_index)
    features[..., 3] = 1
    scaled = scaled_color(extended)
    features[..., 4:7] = scaled
    features[..., 7:10] = scaled
    features[..., 10] = np.float32(0)   # standard 档控制位
    features[..., 11] = np.float32(1)
    features[..., 12] = np.float32(1)
    features[..., 13] = np.float32(-1)
    features[..., 14] = np.float32(-1)
    features[..., 15] = np.float32(0)
    return features, geometry


def compose_head(head: np.ndarray, color01: np.ndarray, intensity: float = 1.0) -> np.ndarray:
    """NR 输出语义：predicted = clip(color + 0.25*half(head_rgb), 0, 1)。

    （MLX-DLSS composition.compose_head 等价；head 已裁回输出分辨率）
    """
    head = np.asarray(head, np.float32)
    color01 = np.asarray(color01, np.float32)
    residual = half(head[..., :3]) * np.float32(0.25)
    predicted = (color01 + residual).clip(0.0, 1.0)
    blend = np.float32(np.clip(intensity, 0.0, 1.0))
    return (color01 + blend * (predicted - color01)).clip(0.0, 1.0).astype(np.float32)
