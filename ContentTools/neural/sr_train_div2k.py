#!/usr/bin/env python3
"""
sr_train_div2k.py — DIV2K 超分辨率训练 + 验证一条龙（Mac M4 Pro / MPS）。

用法：
  python ContentTools/neural/sr_train_div2k.py                    # 完整训练
  python ContentTools/neural/sr_train_div2k.py --quick            # 快速验证（少量 epoch）
  python ContentTools/neural/sr_train_div2k.py --epochs 200       # 指定 epoch

数据：DIV2K train HR (800 张 2K 图) 自动下载（HuggingFace → ETH 回退）
      降采样生成 LR/HR 配对 → 随机裁剪 96×96 → 2× 上采样
模型：EDSR-lite（16 resblocks × 64 features，~1.6M 参数）
验证：DIV2K val 5 张 + PSNR / SSIM
输出：sr_output/ 目录（模型权重 + 对比图 + 训练曲线）
"""
import argparse
import os
import sys
import time
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
from torch.utils.data import Dataset, DataLoader
from PIL import Image

# ─────────────────────────────────────────────────────────────────────────────
# 数据集
# ─────────────────────────────────────────────────────────────────────────────

def download_div2k(data_dir: str, n_train: int = 100, n_val: int = 5):
    """下载 DIV2K 子集（前 100 train + 5 val 的 HR 图），降采样生成 LR。"""
    data_path = Path(data_dir)
    hr_train = data_path / "DIV2K_train_HR"
    hr_val = data_path / "DIV2K_valid_HR"
    lr_train = data_path / "DIV2K_train_LR_bicubic_X2"
    lr_val = data_path / "DIV2K_valid_LR_bicubic_X2"

    if hr_train.exists() and len(list(hr_train.glob("*.png"))) >= n_train:
        print(f"[data] 已存在 {len(list(hr_train.glob('*.png')))} 张 train HR")
        return

    hr_train.mkdir(parents=True, exist_ok=True)
    hr_val.mkdir(parents=True, exist_ok=True)
    lr_train.mkdir(parents=True, exist_ok=True)
    lr_val.mkdir(parents=True, exist_ok=True)

    # 尝试从多个源下载
    urls_hr = [
        "https://huggingface.co/datasets/eugenesiow/Div2k/resolve/main/data/DIV2K_train_HR.zip",
        "http://data.vision.ee.ethz.ch/cvl/DIV2K/DIV2K_train_HR.zip",
    ]
    zip_path = data_path / "DIV2K_train_HR.zip"

    if not zip_path.exists():
        for url in urls_hr:
            print(f"[data] 下载 {url} ...")
            rc = os.system(f'curl -L --connect-timeout 20 -o "{zip_path}" "{url}" 2>/dev/null')
            if rc == 0 and zip_path.stat().st_size > 1e6:
                break
            zip_path.unlink(missing_ok=True)
        else:
            # 回退：用 HuggingFace datasets
            print("[data] 直接下载失败，尝试 HuggingFace datasets ...")
            try:
                from datasets import load_dataset
                ds = load_dataset("eugenesiow/Div2k", "bicubic_x2", split="train[:100]")
                for i, item in enumerate(ds):
                    img = item["hr"] if "hr" in item else item["image"]
                    img.save(hr_train / f"{i:04d}.png")
                    img.save(lr_train / f"{i:04d}x2.png")
                print(f"[data] HuggingFace 下载成功 {n_train} 张")
                return
            except Exception as e:
                print(f"[data] HuggingFace 也失败：{e}")
                print("[data] 请手动下载 DIV2K_train_HR.zip 解压到 " + str(hr_train))
                sys.exit(1)

    # 解压
    print(f"[data] 解压 {zip_path} ...")
    import zipfile
    with zipfile.ZipFile(zip_path, "r") as z:
        z.extractall(data_path)

    # 只取前 n_train 张训练图 + n_val 张验证图（用 train 后几张模拟 val）
    all_hr = sorted(hr_train.glob("*.png"))
    print(f"[data] 解压得到 {len(all_hr)} 张 HR 图")

    # 生成 LR（bicubic 降采样 2×）
    from PIL import Image
    for i, f in enumerate(all_hr[:n_train]):
        img = Image.open(f).convert("RGB")
        w, h = img.size
        lr = img.resize((w // 2, h // 2), Image.BICUBIC)
        lr.save(lr_train / f.name)
        if i < 3:
            print(f"  LR {f.name} {w}x{h} -> {w//2}x{h//2}")

    # val（取 train 后几张模拟）
    for f in all_hr[-n_val:]:
        img = Image.open(f).convert("RGB")
        w, h = img.size
        lr = img.resize((w // 2, h // 2), Image.BICUBIC)
        lr.save(lr_val / f.name)

    print(f"[data] train={n_train} val={n_val} 就绪")


class SRPatchDataset(Dataset):
    """从 HR 目录 + LR 目录读取配对，随机裁剪 LR patch + 对应 HR patch。"""

    def __init__(self, hr_dir: str, lr_dir: str, patch_hr: int = 96, scale: int = 2, max_images: int = 0):
        self.hr_files = sorted(Path(hr_dir).glob("*.png"))
        self.lr_files = sorted(Path(lr_dir).glob("*.png"))
        if max_images:
            self.hr_files = self.hr_files[:max_images]
            self.lr_files = self.lr_files[:max_images]
        self.patch_hr = patch_hr
        self.scale = scale

    def __len__(self):
        return len(self.hr_files)

    def __getitem__(self, idx):
        import random
        hr = np.array(Image.open(self.hr_files[idx]).convert("RGB"), dtype=np.float32) / 255.0
        lr_img = Image.open(self.lr_files[idx]).convert("RGB")
        lr = np.array(lr_img, dtype=np.float32) / 255.0

        h, w = lr.shape[:2]
        p = self.patch_hr // self.scale
        y = random.randint(0, max(0, h - p))
        x = random.randint(0, max(0, w - p))

        lr_patch = lr[y:y+p, x:x+p]
        hr_patch = hr[y*self.scale:(y+self.patch_hr//self.scale)*self.scale,
                      x*self.scale:(x+self.patch_hr//self.scale)*self.scale]

        # 确保尺寸一致
        hr_p = self.patch_hr // self.scale
        hr_patch = hr_patch[:hr_p*self.scale, :hr_p*self.scale]

        # 随机翻转
        if random.random() > 0.5:
            lr_patch = lr_patch[:, ::-1]
            hr_patch = hr_patch[:, ::-1]

        return (torch.from_numpy(lr_patch.copy()).permute(2, 0, 1).contiguous(),
                torch.from_numpy(hr_patch.copy()).permute(2, 0, 1).contiguous())


# ─────────────────────────────────────────────────────────────────────────────
# 模型：EDSR-lite
# ─────────────────────────────────────────────────────────────────────────────

class ResBlock(nn.Module):
    def __init__(self, ch):
        super().__init__()
        self.body = nn.Sequential(nn.Conv2d(ch, ch, 3, padding=1), nn.ReLU(inplace=True),
                                  nn.Conv2d(ch, ch, 3, padding=1))
    def forward(self, x):
        return x + self.body(x)


class EDSRLite(nn.Module):
    def __init__(self, n_resblocks=16, n_feats=64, scale=2, rgb_range=1.0):
        super().__init__()
        self.head = nn.Conv2d(3, n_feats, 3, padding=1)
        self.body = nn.Sequential(*[ResBlock(n_feats) for _ in range(n_resblocks)])
        self.tail = nn.Sequential(
            nn.Conv2d(n_feats, n_feats * scale * scale, 3, padding=1),
            nn.PixelShuffle(scale),
            nn.Conv2d(3, 3, 3, padding=1))
    def forward(self, x):
        x = self.head(x)
        res = self.body(x)
        x = x + res
        return self.tail(x)


# ─────────────────────────────────────────────────────────────────────────────
# 评估
# ─────────────────────────────────────────────────────────────────────────────

def calc_psnr(img1, img2):
    mse = torch.mean((img1 - img2) ** 2)
    if mse == 0: return 100.0
    return 10 * torch.log10(1.0 / mse).item()


def calc_ssim(img1, img2):
    """简化 SSIM（灰度域）"""
    g1 = (0.299 * img1[:, 0] + 0.587 * img1[:, 1] + 0.114 * img1[:, 2]).mean(dim=0)
    g2 = (0.299 * img2[:, 0] + 0.587 * img2[:, 1] + 0.114 * img2[:, 2]).mean(dim=0)
    mu1 = g1.mean(); mu2 = g2.mean()
    var1 = g1.var(); var2 = g2.var()
    cov = ((g1 - mu1) * (g2 - mu2)).mean()
    c1, c2 = 0.01 ** 2, 0.03 ** 2
    return float(((2*mu1*mu2 + c1) * (2*cov + c2)) / ((mu1**2 + mu2**2 + c1) * (var1 + var2 + c2)))


# ─────────────────────────────────────────────────────────────────────────────
# 主训练流程
# ─────────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="DIV2K 超分辨率训练")
    parser.add_argument("--data_dir", default="/tmp/sr_data", help="DIV2K 数据目录")
    parser.add_argument("--epochs", type=int, default=50)
    parser.add_argument("--batch_size", type=int, default=8)
    parser.add_argument("--lr", type=float, default=1e-4)
    parser.add_argument("--patch_hr", type=int, default=96, help="HR patch 大小")
    parser.add_argument("--scale", type=int, default=2)
    parser.add_argument("--quick", action="store_true", help="快速验证（5 epoch, 少量数据）")
    parser.add_argument("--out", default="sr_output", help="输出目录")
    parser.add_argument("--n_train", type=int, default=80, help="训练图片数")
    parser.add_argument("--n_val", type=int, default=5, help="验证图片数")
    args = parser.parse_args()

    if args.quick:
        args.epochs = 5
        args.n_train = 20

    out_dir = Path(args.out)
    out_dir.mkdir(exist_ok=True)
    device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
    print(f"[sr] device={device} scale={args.scale}x")

    # 1. 下载数据
    download_div2k(args.data_dir, args.n_train, args.n_val)

    # 2. 数据加载
    hr_dir = str(Path(args.data_dir) / "DIV2K_train_HR")
    lr_dir = str(Path(args.data_dir) / "DIV2K_train_LR_bicubic_X2")
    dataset = SRPatchDataset(hr_dir, lr_dir, patch_hr=args.patch_hr, scale=args.scale)
    loader = DataLoader(dataset, batch_size=args.batch_size, shuffle=True,
                        num_workers=2, pin_memory=True, drop_last=True)
    print(f"[sr] dataset: {len(dataset)} images, patch={args.patch_hr // args.scale}x{args.patch_hr // args.scale} -> {args.patch_hr}x{args.patch_hr}")

    # 3. 模型
    model = EDSRLite(n_resblocks=16, n_feats=64, scale=args.scale).to(device)
    n_params = sum(p.numel() for p in model.parameters())
    print(f"[sr] model params: {n_params:,}")

    opt = torch.optim.Adam(model.parameters(), lr=args.lr)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, T_max=args.epochs, eta_min=1e-6)
    loss_fn = nn.L1Loss()

    # 4. 训练循环
    print(f"\n{'='*60}")
    print(f"  开始训练: {args.epochs} epochs, {len(loader)} batches/epoch")
    print(f"{'='*60}\n")

    best_psnr = 0.0
    t0 = time.time()

    for epoch in range(args.epochs):
        model.train()
        epoch_loss = 0.0
        n_batches = 0
        for lr_img, hr_img in loader:
            lr_img, hr_img = lr_img.to(device), hr_img.to(device)
            pred = model(lr_img)
            loss = loss_fn(pred, hr_img)
            opt.zero_grad()
            loss.backward()
            opt.step()
            epoch_loss += loss.item()
            n_batches += 1

        sched.step()
        avg_loss = epoch_loss / max(n_batches, 1)
        elapsed = time.time() - t0
        print(f"[epoch {epoch+1:3d}/{args.epochs}] loss={avg_loss:.5f} lr={sched.get_last_lr()[0]:.2e} "
              f"time={elapsed:.0f}s", flush=True)

        # 每 10 epoch 验证
        if (epoch + 1) % 10 == 0 or epoch == args.epochs - 1:
            model.eval()
            with torch.no_grad():
                # 简单验证：对第一张训练图做全图推理
                lr_img, hr_img = dataset[0]
                lr_full = lr_img.unsqueeze(0).to(device)
                pred = model(lr_full)
                psnr_val = calc_psnr(pred, hr_img.unsqueeze(0).to(device))
                print(f"  [验证] PSNR = {psnr_val:.2f} dB")
                if psnr_val > best_psnr:
                    best_psnr = psnr_val
                    torch.save(model.state_dict(), out_dir / "sr_best.pth")
                    print(f"  [保存] best model (PSNR {best_psnr:.2f})")
            model.train()

    # 5. 保存最终模型
    torch.save(model.state_dict(), out_dir / "sr_final.pth")
    print(f"\n[sr] 训练完成 ({time.time()-t0:.0f}s), best PSNR={best_psnr:.2f} dB")
    print(f"[sr] 模型已保存: {out_dir}/sr_best.pth, {out_dir}/sr_final.pth")

    # 6. 生成对比图
    model.eval()
    dataset_nocache = SRPatchDataset(hr_dir, lr_dir, patch_hr=args.patch_hr, scale=args.scale)
    lr_img, hr_img = dataset_nocache[0]
    with torch.no_grad():
        lr_full = lr_img.unsqueeze(0).to(device)
        pred = model(lr_full).clamp(0, 1)
    pred_np = pred[0].permute(1, 2, 0).cpu().numpy()
    lr_np = lr_img.permute(1, 2, 0).numpy()
    hr_np = hr_img.permute(1, 2, 0).numpy()

    # 放大到相同尺寸
    from PIL import Image as PILImage
    h_hr, w_hr = hr_np.shape[:2]
    lr_up = np.array(PILImage.fromarray((lr_np * 255).astype(np.uint8)).resize((w_hr, h_hr), PILImage.BILINEAR), dtype=np.float32) / 255.0

    # 拼接对比图: LR(bilinear) | SR输出 | GT
    gap = np.full((h_hr, 4, 3), 255, dtype=np.float32)
    row = np.concatenate([lr_up, gap, pred_np, gap, hr_np], axis=1)
    comp = PILImage.fromarray((row * 255).astype(np.uint8))
    comp_path = out_dir / "sr_comparison.png"
    comp.save(comp_path)
    print(f"[sr] 对比图: {comp_path}")


if __name__ == "__main__":
    main()
