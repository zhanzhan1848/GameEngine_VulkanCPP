#!/usr/bin/env python
"""eval_nr_vs_denoiser.py — R1.2 验证：适配后 NR 输出 vs 7ch 降噪器（同一 take）。

链路：
  take npz → nr_adapter.build_nr_features（方案 C：GI tonemap 到 [0,1] 填入
  vendor 16ch 契约）→ MLX-DLSS NeuralRenderingPipeline（torch/MPS 参考图）
  → crop → compose_head（LDR 域残差输出）；
  对照 = train_denoiser.py 的 7ch TinyDenoiser（checkpoint 续用）。
  双方输出统一 tonemap 到 LDR [0,1] 与 GT 并排：PSNR / SSIM / PPM 拼图。

用法:
  .venv/bin/python ContentTools/neural/eval_nr_vs_denoiser.py \
      /tmp/neural_capture_m2 --take 3 [--weights logical.safetensors] [--out /tmp/nr_eval]
"""
import argparse
import os
import sys

import numpy as np
import torch

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MLX_DLSS_PY = "/Users/zhanyuanwei/Desktop/MLX-DLSS/python"
DEFAULT_LOGICAL = f"{REPO}/weights/nr/dlssnr-weights-logical.safetensors"
DEFAULT_CKPT = "/tmp/neural_capture_m2/denoiser_ckpt_7ch.pt"


def load_take(d, idx, tag="sponza"):
    z = np.load(os.path.join(d, f"dataset_take_{tag}_{idx}.npz"))
    return {
        "noisy": z["gi_noisy"].astype(np.float32),
        "gt": z["gt_mean"].astype(np.float32),
        "normal": z["normal"].astype(np.float32),
        "depth": z["depth_lin"].astype(np.float32),
    }


def to_ppm(path, img01):
    img = (np.clip(img01, 0, 1) * 255 + 0.5).astype(np.uint8)
    h, w = img.shape[:2]
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        f.write(img.tobytes())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--take", type=int, default=3)
    ap.add_argument("--tag", default="sponza")
    ap.add_argument("--weights", default=DEFAULT_LOGICAL)
    ap.add_argument("--ckpt", default=DEFAULT_CKPT)
    ap.add_argument("--out", default="/tmp/nr_eval")
    ap.add_argument("--frame-index", type=int, default=0)
    ap.add_argument("--channels", type=int, default=7, choices=(7, 10))
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from nr_adapter import (NetworkGeometry, build_nr_features, compose_head,
                            tonemap_to_ldr)
    from metrics import psnr, ssim

    take = load_take(args.dir, args.take, args.tag)
    noisy, gt = take["noisy"], take["gt"]
    h, w = noisy.shape[:2]
    gt_ldr = tonemap_to_ldr(gt)
    noisy_ldr = tonemap_to_ldr(noisy)
    print(f"[take {args.take}] {w}x{h}")

    # ---- NR 路径（方案 C 适配）----
    sys.path.insert(0, MLX_DLSS_PY)
    from mlxdlss.pipeline import NeuralRenderingPipeline
    print(f"[nr] loading logical weights {args.weights} ...")
    pipe = NeuralRenderingPipeline.from_safetensors(args.weights, device="mps",
                                                    precision="reference")
    features, geo = build_nr_features(noisy, frame_index=args.frame_index)
    print(f"[nr] features {features.shape} netExtent {geo.network_width}x{geo.network_height}")
    head = pipe.run_features(features)
    head_c = geo.crop(head)
    color01 = tonemap_to_ldr(noisy, )  # 网络输入色 = 残差基准
    nr_out = compose_head(head_c, color01)
    p_nr = psnr(nr_out, gt_ldr, 1.0)
    s_nr = ssim(nr_out, gt_ldr)
    print(f"[nr] PSNR={p_nr:.2f} dB SSIM={s_nr:.5f} head[min={head_c.min():.4f} max={head_c.max():.4f}]")

    # ---- 7ch 降噪器对照 ----
    from train_denoiser import TinyDenoiser
    model = TinyDenoiser(cin=args.channels)
    ck = torch.load(args.ckpt, map_location="cpu", weights_only=False)
    model.load_state_dict(ck["model"])
    model.eval()
    feats = np.zeros((h, w, args.channels), np.float32)
    feats[..., 0:3] = np.log1p(noisy)
    if args.channels == 10:
        feats[..., 3:6] = normal = take["normal"]
        feats[..., 7:10] = normal
    else:
        feats[..., 3:6] = take["normal"]
    feats[..., 6] = np.clip(take["depth"] / 16.0, 0, 1)
    with torch.no_grad():
        x = torch.from_numpy(feats.transpose(2, 0, 1)[None])
        pred_log = model(x)[0].numpy().transpose(1, 2, 0)
    den_hdr = np.expm1(np.clip(pred_log, 0, 12)).astype(np.float32)
    den_ldr = tonemap_to_ldr(den_hdr)
    p_noisy_hdr = psnr(noisy, gt, max(float(gt.max()), 1e-3))
    p_den_hdr = psnr(den_hdr, gt, max(float(gt.max()), 1e-3))
    p_den = psnr(den_ldr, gt_ldr, 1.0)
    s_den = ssim(den_ldr, gt_ldr)
    print(f"[denoiser] HDR PSNR {p_noisy_hdr:.2f} -> {p_den_hdr:.2f} dB | "
          f"LDR PSNR={p_den:.2f} dB SSIM={s_den:.5f}")

    # ---- 并排 PPM：noisy | NR | denoiser | GT ----
    gap = np.full((h, 2, 3), 24, np.float32)
    grid = np.concatenate([noisy_ldr, gap, nr_out, gap, den_ldr, gap, gt_ldr], 1)
    out_ppm = os.path.join(args.out, f"side_by_side_take{args.take}.ppm")
    to_ppm(out_ppm, grid)
    print(f"[out] {out_ppm} (noisy | NR(adapter C) | denoiser7ch | GT)")

    print("[summary]")
    print(f"  NR(adapter C)  LDR PSNR {p_nr:6.2f} dB  SSIM {s_nr:.5f}")
    print(f"  denoiser 7ch   LDR PSNR {p_den:6.2f} dB  SSIM {s_den:.5f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
