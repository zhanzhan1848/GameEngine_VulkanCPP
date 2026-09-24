#!/usr/bin/env python3
"""sr_train_local.py — 离线 SR 训练（合成纹理，零下载，Mac MPS）。

python ContentTools/neural/sr_train_local.py [--epochs 100]
"""
import argparse
import math
import os
import time

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as FN

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sr_output")


def make_texture(size, rng):
    h = w = size
    img = np.zeros((h, w, 3), np.float32)
    gx = np.linspace(0, 1, w, dtype=np.float32)[None, :]
    gy = np.linspace(0, 1, h, dtype=np.float32)[:, None]
    for c in range(3):
        img[..., c] = 0.25 + 0.4 * gx * rng.random() + 0.3 * gy * rng.random()
    for _ in range(6):
        freq = rng.uniform(8, 50)
        angle = rng.uniform(0, 3.14)
        amp = rng.uniform(0.04, 0.12)
        ph = rng.uniform(0, 6.28)
        proj = (gx * np.cos(angle) + gy * np.sin(angle)).astype(np.float32)
        col = rng.random(3).astype(np.float32)
        for c in range(3):
            img[..., c] += (amp * np.sin(2 * 3.14159 * freq * proj + ph) * col[c]).astype(np.float32)
    img += rng.normal(0, 0.03, img.shape).astype(np.float32)
    for _ in range(5):
        rx, ry = rng.integers(0, w - 20), rng.integers(0, h - 20)
        rw, rh = rng.integers(10, w // 3), rng.integers(10, h // 3)
        c = rng.random(3).astype(np.float32) * 0.5 + 0.3
        a = rng.uniform(0.4, 0.8)
        img[ry:ry+rh, rx:rx+rw] = img[ry:ry+rh, rx:rx+rw] * (1-a) + c * a
    return np.clip(img, 0, 1).astype(np.float32)


def downsample(img, scale):
    t = torch.from_numpy(np.ascontiguousarray(img)).permute(2, 0, 1).unsqueeze(0)
    lr = FN.interpolate(t, scale_factor=1.0/scale, mode="bilinear", align_corners=False)
    return lr.squeeze(0).permute(1, 2, 0).numpy()


class ResBlock(nn.Module):
    def __init__(self, ch):
        super().__init__()
        self.body = nn.Sequential(nn.Conv2d(ch, ch, 3, padding=1), nn.ReLU(True),
                                  nn.Conv2d(ch, ch, 3, padding=1))
    def forward(self, x):
        return x + self.body(x)


class EDSR(nn.Module):
    def __init__(self, n_blocks=12, n_feats=48, scale=2):
        super().__init__()
        self.head = nn.Conv2d(3, n_feats, 3, padding=1)
        self.body = nn.Sequential(*[ResBlock(n_feats) for _ in range(n_blocks)])
        self.up = nn.Sequential(nn.Conv2d(n_feats, 3 * scale * scale, 3, padding=1),
                                nn.PixelShuffle(scale))
    def forward(self, x):
        x = self.head(x)
        x = x + self.body(x)
        return self.up(x)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--epochs", type=int, default=100)
    ap.add_argument("--lr", type=float, default=2e-4)
    ap.add_argument("--out", default="sr_output")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
    print(f"[sr] device={device}")

    torch.manual_seed(42)
    model = EDSR().to(device)
    print(f"[sr] params={sum(p.numel() for p in model.parameters()):,}")

    opt = torch.optim.Adam(model.parameters(), lr=args.lr)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, args.epochs, 1e-6)
    l1 = nn.L1Loss()

    # 固定验证集
    vrng = np.random.default_rng(999)
    val_hr = make_texture(128, vrng)
    val_lr = downsample(val_hr, 2)
    val_lr_t = torch.from_numpy(np.ascontiguousarray(val_lr)).permute(2,0,1).unsqueeze(0)
    val_hr_t = torch.from_numpy(np.ascontiguousarray(val_hr)).permute(2,0,1).unsqueeze(0)

    best_psnr = 0.0
    t0 = time.time()

    for epoch in range(args.epochs):
        model.train()
        # 生成随机训练 batch
        batch_lr, batch_hr = [], []
        for _ in range(8):
            rng = np.random.default_rng(epoch * 100 + np.random.randint(10000))
            hr = make_texture(96, rng)
            lr = downsample(hr, 2)
            # y/x 在 LR 空间：LR 是 48×48，裁 24×24 的 LR patch
            y = np.random.randint(0, 48 - 24)
            x = np.random.randint(0, 48 - 24)
            batch_lr.append(lr[y:y+24, x:x+24])
            # 对应 HR 区域（2× 坐标变换）
            batch_hr.append(hr[y*2:y*2+48, x*2:x*2+48])
        lr_t = torch.from_numpy(np.ascontiguousarray(np.stack(batch_lr))).permute(0,3,1,2).to(device)
        hr_t = torch.from_numpy(np.ascontiguousarray(np.stack(batch_hr))).permute(0,3,1,2).to(device)

        pred = model(lr_t)
        loss = l1(pred, hr_t)
        opt.zero_grad()
        loss.backward()
        opt.step()

        # 验证
        model.eval()
        with torch.no_grad():
            vp = model(val_lr_t.to(device)).clamp(0, 1).squeeze(0).permute(1,2,0).cpu().numpy()
            mse = np.mean((vp - val_hr) ** 2)
            p = 10 * math.log10(1.0 / max(mse, 1e-10))
        model.train()

        sched.step()
        improved = ""
        if p > best_psnr:
            best_psnr = p
            improved = " ★"
            torch.save(model.state_dict(), os.path.join(args.out, "sr_best.pth"))
        if (epoch + 1) % 10 == 0 or epoch == 0:
            print(f"  [{epoch+1:3d}/{args.epochs}] valPSNR={p:.2f}dB{improved}  {time.time()-t0:.0f}s", flush=True)

    print(f"\n[sr] 训练完成 {time.time()-t0:.0f}s | best PSNR={best_psnr:.2f} dB")
    lr_up_check = FN.interpolate(
        torch.from_numpy(np.ascontiguousarray(val_lr)).permute(2,0,1).unsqueeze(0),
        size=(val_hr.shape[0], val_hr.shape[1]), mode="bilinear", align_corners=False
    ).squeeze(0).permute(1,2,0).numpy()
    base_psnr = 10 * math.log10(1.0 / max(np.mean((lr_up_check - val_hr) ** 2), 1e-10))
    print(f"[sr] bilinear baseline: {base_psnr:.2f} dB | SR 提升: {best_psnr - base_psnr:+.2f} dB")

    # 保存最终模型
    torch.save(model.state_dict(), os.path.join(args.out, "sr_final.pth"))

    # 保存对比图
    from PIL import Image as PImage
    model.eval()
    with torch.no_grad():
        lr_t = torch.from_numpy(np.ascontiguousarray(val_lr)).permute(2,0,1).unsqueeze(0).to(device)
        sr_out = model(lr_t).clamp(0,1).squeeze(0).permute(1,2,0).cpu().numpy()
        lr_up = FN.interpolate(val_lr_t.to(device), size=(128,128), mode="bilinear",
                               align_corners=False).squeeze(0).permute(1,2,0).cpu().numpy()

    gap = np.full((128, 4, 3), 255, np.float32)
    row = np.concatenate([lr_up, gap, sr_out, gap, val_hr], axis=1)
    comp = PImage.fromarray((np.clip(row, 0, 1) * 255).astype(np.uint8))
    comp_path = os.path.join(args.out, "sr_comparison.png")
    comp.save(comp_path)
    print(f"[sr] 对比图: {comp_path}  (LR bilinear | SR model | GT)")


if __name__ == "__main__":
    main()
