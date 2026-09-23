#!/usr/bin/env python
"""metrics.py — 评估指标（PSNR / SSIM），R1.2 对比与 R3.3 门禁共用。

SSIM：单层高斯窗口（11×11 σ=1.5）标准实现，灰度域（ITU-R 601 luma），
data_range 显式传入（LDR=1.0 / HDR 自定义）。
"""
from __future__ import annotations

import numpy as np


def psnr(a, b, dr):
    a = np.asarray(a, np.float64)
    b = np.asarray(b, np.float64)
    mse = np.mean((a - b) ** 2)
    return 10.0 * np.log10(dr * dr / max(mse, 1e-12))


def _gaussian_kernel11():
    r = np.arange(11, dtype=np.float64) - 5
    k = np.exp(-(r * r) / (2 * 1.5 * 1.5))
    return k / k.sum()


_KERNEL = _gaussian_kernel11()


def _conv2_valid(img, kernel):
    c = kernel.shape[0]
    h, w = img.shape
    oh, ow = h - c + 1, w - c + 1
    out = np.zeros((oh, ow))
    for i in range(c):
        for j in range(c):
            out += kernel[i, j] * img[i:i + oh, j:j + ow]
    return out


def _to_gray(img):
    img = np.asarray(img, np.float64)
    if img.ndim == 2:
        return img
    return img[..., 0] * 0.299 + img[..., 1] * 0.587 + img[..., 2] * 0.114


def ssim(a, b, data_range: float = 1.0):
    """SSIM（灰度域，11×11 高斯窗，K1=0.01 K2=0.03）。"""
    x = _to_gray(a)
    y = _to_gray(b)
    k2 = _KERNEL[:, None] * _KERNEL[None, :]
    C1 = (0.01 * data_range) ** 2
    C2 = (0.03 * data_range) ** 2
    mu_x = _conv2_valid(x, k2)
    mu_y = _conv2_valid(y, k2)
    mu_x2, mu_y2, mu_xy = mu_x * mu_x, mu_y * mu_y, mu_x * mu_y
    sigma_x = _conv2_valid(x * x, k2) - mu_x2
    sigma_y = _conv2_valid(y * y, k2) - mu_y2
    sigma_xy = _conv2_valid(x * y, k2) - mu_xy
    num = (2 * mu_xy + C1) * (2 * sigma_xy + C2)
    den = (mu_x2 + mu_y2 + C1) * (sigma_x + sigma_y + C2)
    return float(np.mean(num / den))
