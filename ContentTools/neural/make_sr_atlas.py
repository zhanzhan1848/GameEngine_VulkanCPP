#!/usr/bin/env python
"""make_sr_atlas.py — 低分辨率渲染 × DLSS-NR 放大 vs 原生高分辨率 对比图集。

背景：NVIDIA DLSS-SR/RTX-VSR 的提取路径在本机不可用（dlss.so 为 unknown
checkpoint；package-sr 需 CUDA parameter capture；extract-vsr 需 SHA 锁定的
VSR 驱动库）。改用已集成的 DLSS-NR transformer（dlss-dlssnr 恢复模型）作
放大增强器：它是以残差形式重画细节的 restoration 网络。

输入（引擎两次运行、同一锁定相机、同一帧号）：
  low  = PRIMAL_RENDER_SCALE=0.5（640×360 渲染）回读的 1280×720 PPM
         （引擎 FinalBlit 双线性上采样后的画面）
  ref  = PRIMAL_RENDER_SCALE=1.0（1280×720 原生渲染）同规格 PPM

处理：
  raw_low = low 双线性降回 640×360（近似原始低分辨率渲染）
  候选（全部 1280×720）：
    nearest / bilinear = 经典上采样
    dlss-nr            = bilinear 结果喂 DLSS-NR transformer（LDR 直入，
                         残差 compose），细节重画增强
  指标：各候选 vs ref 的 RGB PSNR + 灰度 SSIM。

输出：atlas.png（整帧行 + 细节放大行 + 指标标签）与各候选单帧 PNG。

用法:
  .venv/bin/python ContentTools/neural/make_sr_atlas.py \
      /tmp/sr_atlas/low/reference_420.ppm /tmp/sr_atlas/ref/reference_420.ppm \
      --out /tmp/sr_atlas
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MLX_DLSS_PY = "/Users/zhanyuanwei/Desktop/MLX-DLSS/python"


def read_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        wh = f.readline().split()
        w, h = int(wh[0]), int(wh[1])
        assert f.readline().strip() == b"255"
        data = np.frombuffer(f.read(w * h * 3), np.uint8)
    return data.reshape(h, w, 3).astype(np.float32) / 255.0


def psnr(a, b):
    a = a.astype(np.float64)
    b = b.astype(np.float64)
    mse = np.mean((a - b) ** 2)
    return 10.0 * np.log10(1.0 / max(mse, 1e-12))


def ssim(a, b):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from metrics import ssim as _ssim
    return _ssim(a, b, 1.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("low_ppm")
    ap.add_argument("ref_ppm")
    ap.add_argument("--out", default="/tmp/sr_atlas")
    ap.add_argument("--low-w", type=int, default=640)
    ap.add_argument("--low-h", type=int, default=360)
    ap.add_argument("--crops", default="0.30,0.25;0.62,0.55",
                    help="细节放大行裁剪中心（归一化 x,y，分号分隔）")
    ap.add_argument("--crop-w", type=int, default=240)
    ap.add_argument("--crop-h", type=int, default=120)
    ap.add_argument("--skip-nr", action="store_true")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    low_up = read_ppm(args.low_ppm)     # 1280×720（引擎已双线性放大的低分辨率渲染）
    ref = read_ppm(args.ref_ppm)        # 1280×720 原生
    h, w = ref.shape[:2]
    assert low_up.shape == ref.shape, (low_up.shape, ref.shape)

    # 原始低分辨率渲染：双线性降回 640×360
    low_img = Image.fromarray((low_up * 255 + 0.5).astype(np.uint8))
    raw_low = low_img.resize((args.low_w, args.low_h), Image.BILINEAR)

    def up(img, method):
        return np.asarray(img.resize((w, h), method), np.float32) / 255.0

    nearest = up(raw_low, Image.NEAREST)
    bilinear = up(raw_low, Image.BILINEAR)

    candidates = [("Nearest x2", nearest), ("Bilinear x2", bilinear)]

    # ---- DLSS-NR 增强 ----
    if not args.skip_nr:
        sys.path.insert(0, MLX_DLSS_PY)
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from mlxdlss.pipeline import NeuralRenderingPipeline
        from nr_adapter import (NetworkGeometry, compose_head, deterministic_noise,
                                half, scaled_color)
        print("[nr] loading DLSS-NR transformer ...")
        pipe = NeuralRenderingPipeline.from_safetensors(
            f"{REPO}/weights/nr/dlssnr-weights-logical.safetensors",
            device="mps", precision="reference")
        color01 = bilinear.astype(np.float32)   # LDR 直入（已显示域）
        geo = NetworkGeometry.vendor_aligned(w, h)
        ext = geo.extend(color01)
        feats = np.zeros((geo.network_height, geo.network_width, 16), np.float32)
        feats[..., 0:3] = deterministic_noise(geo.network_height, geo.network_width, 0)
        feats[..., 3] = 1
        sc = scaled_color(ext)
        feats[..., 4:7] = sc
        feats[..., 7:10] = sc
        feats[..., 10], feats[..., 11], feats[..., 12] = 0, 1, 1
        feats[..., 13], feats[..., 14], feats[..., 15] = -1, -1, 0
        print(f"[nr] forward {feats.shape} ...")
        head = pipe.run_features(feats)
        dlss_nr = compose_head(geo.crop(head), color01)
        candidates.append(("DLSS-NR x2", dlss_nr))

    # ---- 指标 ----
    print(f"[metrics] vs native {w}x{h} reference:")
    for name, img in candidates:
        print(f"  {name:12s} PSNR={psnr(img, ref):6.2f} dB  SSIM={ssim(img, ref):.4f}")

    # ---- 图集 ----
    def to_u8(img):
        return (np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8)

    tile_w, tile_h = w // 2, h // 2          # 整帧行每格 640×360
    zoom = 2                                  # 细节行放大倍数
    crop_w, crop_h = args.crop_w, args.crop_h
    centers = [(float(a), float(b)) for a, b in
               (pair.split(",") for pair in args.crops.split(";"))]

    label_h = 26
    col_gap, row_gap = 6, 10
    cols = len(candidates) + 1                # + reference 列
    atlas_w = cols * tile_w + (cols - 1) * col_gap
    atlas_h = label_h + tile_h + row_gap + \
        len(centers) * (label_h + crop_h * zoom) + (len(centers) - 1) * row_gap

    atlas = Image.new("RGB", (atlas_w, atlas_h), (24, 24, 24))
    from PIL import ImageDraw
    draw = ImageDraw.Draw(atlas)

    def paste_column(col_x, y_top, name, img, crop_info=None):
        draw.text((col_x + 4, y_top + 6), name, fill=(240, 240, 240))
        if crop_info is None:
            atlas.paste(Image.fromarray(to_u8(img)).resize((tile_w, tile_h),
                                                           Image.BILINEAR),
                        (col_x, y_top + label_h))
        else:
            cx, cy = crop_info
            x0 = int(cx * w - crop_w / 2)
            y0 = int(cy * h - crop_h / 2)
            x0 = max(0, min(x0, w - crop_w))
            y0 = max(0, min(y0, h - crop_h))
            crop = img[y0:y0 + crop_h, x0:x0 + crop_w]
            atlas.paste(Image.fromarray(to_u8(crop)).resize(
                (crop_w * zoom, crop_h * zoom), Image.NEAREST),
                (col_x, y_top + label_h))

    # 整帧行（列标签带 vs 原生的指标）
    top_y = 0
    for k, (name, img) in enumerate(candidates):
        paste_column(k * (tile_w + col_gap), top_y,
                     f"{name}  PSNR={psnr(img, ref):.2f} SSIM={ssim(img, ref):.3f}", img)
    paste_column(len(candidates) * (tile_w + col_gap), top_y,
                 "Native Reference", ref)

    # 细节放大行
    y = label_h + tile_h + row_gap
    for ci, (cx, cy) in enumerate(centers):
        draw.text((8, y + 4),
                  f"detail crop {ci + 1} ({crop_w}x{crop_h} @2x)", fill=(160, 200, 240))
        y += label_h
        for k, (name, img) in enumerate(candidates):
            paste_column(k * (tile_w + col_gap), y, name, img, (cx, cy))
        paste_column(len(candidates) * (tile_w + col_gap), y,
                     "Native Reference", ref, (cx, cy))
        y += crop_h * zoom + row_gap

    out_png = os.path.join(args.out, "atlas.png")
    atlas.save(out_png)
    print(f"[out] {out_png} ({atlas_w}x{atlas_h})")
    for name, img in candidates:
        tag = name.split()[0].lower()
        Image.fromarray(to_u8(img)).save(os.path.join(args.out, f"cand_{tag}.png"))
    Image.fromarray(to_u8(ref)).save(os.path.join(args.out, "reference.png"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
