#!/usr/bin/env python3
"""sr_train_v2.py — 2× 超分辨率 训练/评估/导出 一条龙（DIV2K + 引擎渲染帧，CUDA/MPS/CPU）。

背景
  引擎要把 640×360 低分辨率渲染帧超分到 1280×720（替代 FinalBlit 双线性，
  M0 已测 bilinear baseline ≈ 24.4 dB）。合成纹理训练的模型反而比 bilinear
  差 0.3 dB（见 Docs/2026-09-24-dlss-sr-capture-experience-and-blockers.md），
  根因是合成纹理与真实图像/渲染帧域差距太大——本脚本用 DIV2K 真实图像 +
  在线退化增强（bicubic/bilinear/box + 噪声，模拟低分辨率渲染的走样与噪点）
  训练 EDSR-lite，并支持用引擎实拍配对帧（PRIMAL_RENDER_SCALE=0.5/1.0 两次
  运行同相机 dump 的 PPM）做微调与游戏域评估。

数据约定
  --data_dir 下需有 DIV2K_train_HR/ 与 DIV2K_valid_HR/（PNG，官方布局）。
  --game_dir 下配对帧命名 low_XXX.(ppm|png)（640×360 低清渲染）+
  ref_XXX.(ppm|png)（1280×720 原生渲染），编号一一对应。

用法
  训练:   python sr_train_v2.py train --data_dir /root/sr_train/data --iters 100000
  微调:   python sr_train_v2.py train --data_dir ... --game_dir ... \
              --finetune_from sr_best.pth --iters 6000 --lr 2e-5
  评估:   python sr_train_v2.py eval --ckpt sr_best.pth --data_dir ... [--game_dir ...]
  导出:   python sr_train_v2.py export --ckpt sr_best.pth --out sr_edsr2x.ts.pt

输出
  <out>/sr_best.pth / sr_last.pth（权重+优化器，可 --resume）
  <out>/train_log.txt 每次评估一行
  <out>/compare_*.png 对比条（bilinear | SR | GT 局部放大）
"""
import argparse
import math
import os
import random
import sys
import time
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
from PIL import Image
from torch.utils.data import DataLoader, Dataset

Image.MAX_IMAGE_PIXELS = None

# ─────────────────────────────────────────────────────────────────────────────
# 图像 IO
# ─────────────────────────────────────────────────────────────────────────────


def read_image(path):
    """读 PNG/PPM 为 float32 RGB [0,1]，HWC。"""
    p = str(path)
    if p.lower().endswith(".ppm"):
        with open(p, "rb") as f:
            assert f.readline().strip() == b"P6", f"不是 P6 PPM: {p}"
            w, h = map(int, f.readline().split())
            assert f.readline().strip() == b"255"
            data = np.frombuffer(f.read(w * h * 3), np.uint8)
        return data.reshape(h, w, 3).astype(np.float32) / 255.0
    return np.array(Image.open(p).convert("RGB"), dtype=np.float32) / 255.0


def psnr(a, b):
    """RGB PSNR，输入 float [0,1]。"""
    mse = float(np.mean((a - b) ** 2))
    return 10.0 * math.log10(1.0 / max(mse, 1e-10))


# ─────────────────────────────────────────────────────────────────────────────
# 模型：EDSR-lite（16 resblock × 64 feat，~1.6M 参数）
# ─────────────────────────────────────────────────────────────────────────────


class ResBlock(nn.Module):
    def __init__(self, ch, res_scale=1.0):
        super().__init__()
        self.res_scale = res_scale
        self.body = nn.Sequential(
            nn.Conv2d(ch, ch, 3, padding=1), nn.ReLU(inplace=True),
            nn.Conv2d(ch, ch, 3, padding=1))

    def forward(self, x):
        return x + self.res_scale * self.body(x)


class EDSRLite(nn.Module):
    """全卷积 + PixelShuffle，任意输入尺寸（Tile 推理/TorchScript 友好）。"""

    def __init__(self, n_resblocks=16, n_feats=64, scale=2, in_ch=3, out_ch=3):
        super().__init__()
        self.scale = scale
        self.head = nn.Conv2d(in_ch, n_feats, 3, padding=1)
        self.body = nn.Sequential(*[ResBlock(n_feats) for _ in range(n_resblocks)])
        self.tail = nn.Sequential(
            nn.Conv2d(n_feats, out_ch * scale * scale, 3, padding=1),
            nn.PixelShuffle(scale))

    def forward(self, x):
        x = self.head(x)
        x = x + self.body(x)
        return self.tail(x)


def make_model(scale=2, n_resblocks=16, n_feats=64):
    return EDSRLite(n_resblocks=n_resblocks, n_feats=n_feats, scale=scale)


# ─────────────────────────────────────────────────────────────────────────────
# 数据：在线裁剪 + 在线退化（模拟低分辨率渲染）
# ─────────────────────────────────────────────────────────────────────────────

_DEGRADE_FILTERS = {
    "bicubic": Image.BICUBIC,   # DIV2K 标准协议
    "bilinear": Image.BILINEAR,
    "box": Image.BOX,           # 近似面积采样走样
}


class TrainDataset(Dataset):
    """DIV2K HR 图（可叠加游戏配对）→ 随机 patch，在线降采样退化。"""

    def __init__(self, hr_files, patch_hr=96, scale=2,
                 noise_prob=0.3, noise_max=2.5, game_pairs=None):
        self.hr_files = [str(f) for f in hr_files]
        self.patch_hr = patch_hr
        self.scale = scale
        self.noise_prob = noise_prob
        self.noise_max = noise_max / 255.0
        # 游戏配对：列表元素为 (lr_path, hr_path)，作为独立样本混入
        self.game_pairs = game_pairs or []
        self.cache_dir = Path(hr_files[0]).parent / "npy_cache" if hr_files else None

    def __len__(self):
        return len(self.hr_files) + len(self.game_pairs)

    def _load_hr_u8(self, path):
        """uint8 HWC；优先 npy 缓存（免 2K PNG 反复解码，训练吞吐瓶颈所在）。"""
        if self.cache_dir:
            cached = self.cache_dir / (Path(path).stem + ".npy")
            if cached.exists():
                return np.load(cached, mmap_mode="r")
        return np.array(Image.open(path).convert("RGB"), dtype=np.uint8)

    def _degrade(self, hr_patch):
        """HR HWC float → LR HWC float（在线退化 + 噪声）。"""
        h, w = hr_patch.shape[:2]
        pim = Image.fromarray((np.clip(hr_patch, 0, 1) * 255).astype(np.uint8))
        filt = random.choices(list(_DEGRADE_FILTERS.values()),
                              weights=[0.5, 0.25, 0.25])[0]
        lr = np.array(pim.resize((w // self.scale, h // self.scale), filt),
                      dtype=np.float32) / 255.0
        if random.random() < self.noise_prob:
            # python random 已被 DataLoader 按 worker 播种，numpy 不会——经它取种
            rng = np.random.default_rng(random.randint(0, 2**31 - 1))
            sigma = random.uniform(0.5, self.noise_max * 255.0)
            lr = lr + rng.normal(0.0, sigma / 255.0, size=lr.shape).astype(np.float32)
        return np.clip(lr, 0.0, 1.0)

    def __getitem__(self, idx):
        n_div = len(self.hr_files)
        p = self.patch_hr
        if idx < n_div:
            img = self._load_hr_u8(self.hr_files[idx])
            H, W = img.shape[:2]
            y = random.randint(0, H - p)
            x = random.randint(0, W - p)
            hr = img[y:y + p, x:x + p].astype(np.float32) / 255.0
            lr = self._degrade(hr)
        else:
            lr_p, hr_p = self.game_pairs[idx - n_div]
            lr_img = read_image(lr_p)
            hr_img = read_image(hr_p)
            ph, pw = lr_img.shape[:2]
            if ph > p // self.scale and pw > p // self.scale:
                y = random.randint(0, ph - p // self.scale)
                x = random.randint(0, pw - p // self.scale)
                lr = lr_img[y:y + p // self.scale, x:x + p // self.scale]
                hr = hr_img[y * self.scale:(y + p // self.scale) * self.scale,
                            x * self.scale:(x + p // self.scale) * self.scale]
            else:
                lr, hr = lr_img, hr_img
            lr = np.clip(lr, 0.0, 1.0)

        # 翻转 + 90° 旋转增广（LR/HR 同步）
        if random.random() < 0.5:
            lr, hr = lr[:, ::-1], hr[:, ::-1]
        k = random.randint(0, 3)
        if k:
            lr, hr = np.rot90(lr, k), np.rot90(hr, k)

        lr = np.ascontiguousarray(lr.transpose(2, 0, 1))
        hr = np.ascontiguousarray(hr.transpose(2, 0, 1))
        return torch.from_numpy(lr), torch.from_numpy(hr)


def list_div2k(data_dir, split):
    d = Path(data_dir) / split
    files = sorted(d.glob("*.png")) + sorted(d.glob("*.jpg"))
    if not files:
        raise FileNotFoundError(f"{d} 下没有图片")
    return files


def list_game_pairs(game_dir):
    """low_XXX.(ppm|png) 与 ref_XXX.(ppm|png) 编号配对。"""
    d = Path(game_dir)
    pairs = []
    lows = sorted(list(d.glob("low_*.ppm")) + list(d.glob("low_*.png")))
    for lo in lows:
        key = lo.stem.split("_", 1)[1]
        refs = list(d.glob(f"ref_{key}.ppm")) + list(d.glob(f"ref_{key}.png"))
        if refs:
            pairs.append((str(lo), str(refs[0])))
    return pairs


# ─────────────────────────────────────────────────────────────────────────────
# 瓦片推理与评估
# ─────────────────────────────────────────────────────────────────────────────


@torch.no_grad()
def infer_tiled(model, lr_np, device, scale=2, tile=192, margin=8):
    """LR HWC float → SR HWC float。瓦片 + 边缘裁剪避免拼接伪影。"""
    h, w = lr_np.shape[:2]
    out = np.zeros((h * scale, w * scale, 3), np.float32)
    lr_t = torch.from_numpy(np.ascontiguousarray(lr_np.transpose(2, 0, 1))
                            ).unsqueeze(0).to(device)
    step = tile - 2 * margin
    ys = list(range(0, max(h - 2 * margin, 1), step)) if h > 2 * margin else [0]
    xs = list(range(0, max(w - 2 * margin, 1), step)) if w > 2 * margin else [0]
    for y0 in ys:
        for x0 in xs:
            y1, x1 = min(y0 + tile, h), min(x0 + tile, w)
            patch = lr_t[:, :, y0:y1, x0:x1]
            sr = model(patch).clamp(0, 1)[0].permute(1, 2, 0).float().cpu().numpy()
            # 输出有效区（去掉 margin 对应的 HR 边）
            my0, mx0 = (margin if y0 > 0 else 0) * scale, (margin if x0 > 0 else 0) * scale
            my1 = (y1 - y0) * scale - (margin * scale if y1 < h else 0)
            mx1 = (x1 - x0) * scale - (margin * scale if x1 < w else 0)
            oy, ox = y0 * scale + my0, x0 * scale + mx0
            out[oy:oy + (my1 - my0), ox:ox + (mx1 - mx0)] = sr[my0:my1, mx0:mx1]
    return np.clip(out, 0.0, 1.0)


def upsample_bilinear(lr_np, out_h, out_w):
    t = torch.from_numpy(np.ascontiguousarray(lr_np.transpose(2, 0, 1))
                         ).unsqueeze(0)
    up = torch.nn.functional.interpolate(t, size=(out_h, out_w),
                                         mode="bilinear", align_corners=False)
    return up[0].permute(1, 2, 0).numpy()


def eval_image_pair(model, lr_np, hr_np, device, scale=2, tile=192):
    """返回 (sr_psnr, bilinear_psnr, sr_img)。"""
    sr = infer_tiled(model, lr_np, device, scale=scale, tile=tile)
    bi = upsample_bilinear(lr_np, hr_np.shape[0], hr_np.shape[1])
    return psnr(sr, hr_np), psnr(bi, hr_np), sr


def save_compare(lr_np, hr_np, sr, path, crop=None):
    """对比条：bilinear | SR | GT，可选 HR 坐标局部裁剪放大。"""
    bi = upsample_bilinear(lr_np, hr_np.shape[0], hr_np.shape[1])
    if crop:
        cy, cx, s = crop
        bi = bi[cy:cy + s, cx:cx + s]
        sr = sr[cy:cy + s, cx:cx + s]
        hr_np = hr_np[cy:cy + s, cx:cx + s]
    gap = np.full((bi.shape[0], 4, 3), 255.0, np.float32)
    row = np.concatenate([bi, gap, sr, gap, hr_np], axis=1)
    Image.fromarray((row * 255).astype(np.uint8)).save(path)


# ─────────────────────────────────────────────────────────────────────────────
# train 子命令
# ─────────────────────────────────────────────────────────────────────────────


def build_val_images(data_dir, n_val, scale):
    """DIV2K valid 前 n_val 张 → [(name, lr_np, hr_np)]（标准 bicubic 降采样）。"""
    vals = []
    for f in list_div2k(data_dir, "DIV2K_valid_HR")[:n_val]:
        hr = read_image(f)
        H, W = hr.shape[:2]
        pim = Image.fromarray((hr * 255).astype(np.uint8))
        lr = np.array(pim.resize((W // scale, H // scale), Image.BICUBIC),
                      dtype=np.float32) / 255.0
        vals.append((Path(f).stem, lr, hr))
    return vals


def run_validation(model, val_images, device, scale):
    ps = []
    for _, lr_np, hr_np in val_images:
        p, _, _ = eval_image_pair(model, lr_np, hr_np, device, scale=scale)
        ps.append(p)
    return float(np.mean(ps))


def cmd_train(args):
    device = torch.device(args.device) if args.device != "auto" else torch.device(
        "cuda" if torch.cuda.is_available()
        else "mps" if torch.backends.mps.is_available() else "cpu")
    if device.type == "cuda":
        torch.backends.cuda.matmul.allow_tf32 = True
        torch.backends.cudnn.allow_tf32 = True
        torch.backends.cudnn.benchmark = True
    print(f"[sr] device={device.type} iters={args.iters} batch={args.batch}", flush=True)

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    log_f = open(out / "train_log.txt", "a")
    # ckpt 只存基本类型（weights_only=True 可加载；vars(args) 含函数引用会炸）
    args_dict = {k: v for k, v in vars(args).items()
                 if isinstance(v, (int, float, str, bool, type(None)))}

    scale = args.scale
    hr_files = list_div2k(args.data_dir, "DIV2K_train_HR")
    if args.n_train:
        hr_files = hr_files[:args.n_train]
    game_pairs = list_game_pairs(args.game_dir) if args.game_dir else []
    game_pairs = game_pairs * args.game_repeat  # 提高游戏域采样配比
    ds = TrainDataset(hr_files, patch_hr=args.patch_hr, scale=scale,
                      noise_prob=args.noise_prob, game_pairs=game_pairs)
    loader = DataLoader(ds, batch_size=args.batch, shuffle=True,
                        num_workers=args.workers, pin_memory=(device.type == "cuda"),
                        drop_last=True, persistent_workers=args.workers > 0)
    print(f"[sr] train: {len(hr_files)} DIV2K + {len(game_pairs)} game pairs; "
          f"val: {args.n_val} imgs × tile {args.tile}", flush=True)

    model = make_model(scale=scale).to(device)
    n_params = sum(p.numel() for p in model.parameters())
    print(f"[sr] model params: {n_params:,}", flush=True)

    if args.finetune_from:
        sd = torch.load(args.finetune_from, map_location="cpu")
        model.load_state_dict(sd["model"] if "model" in sd else sd)
        print(f"[sr] finetune init from {args.finetune_from}", flush=True)

    opt = torch.optim.Adam(model.parameters(), lr=args.lr)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, args.iters, args.lr_min)
    crit = nn.L1Loss()

    start_iter = 0
    best_psnr = -1.0
    ckpt_last = out / "sr_last.pth"
    if args.resume and ckpt_last.exists():
        ck = torch.load(ckpt_last, map_location="cpu")
        model.load_state_dict(ck["model"])
        opt.load_state_dict(ck["opt"])
        sched.load_state_dict(ck["sched"])
        start_iter = ck["iter"]
        best_psnr = ck.get("best_psnr", -1.0)
        print(f"[sr] resume @ iter {start_iter} (best {best_psnr:.2f})", flush=True)

    val_images = build_val_images(args.data_dir, args.n_val, scale)

    model.train()
    t0 = time.time()
    it = start_iter
    data_iter = iter(loader)
    loss_acc = []
    while it < args.iters:
        it += 1
        try:
            lr_b, hr_b = next(data_iter)
        except StopIteration:
            data_iter = iter(loader)
            lr_b, hr_b = next(data_iter)
        lr_b, hr_b = lr_b.to(device, non_blocking=True), hr_b.to(device, non_blocking=True)
        loss = crit(model(lr_b), hr_b)
        opt.zero_grad(set_to_none=True)
        loss.backward()
        opt.step()
        sched.step()
        loss_acc.append(loss.item())

        if it % args.eval_every == 0 or it == args.iters:
            model.eval()
            v = run_validation(model, val_images, device, scale)
            dt = time.time() - t0
            line = (f"iter {it} loss {np.mean(loss_acc):.5f} valPSNR {v:.2f} "
                    f"lr {sched.get_last_lr()[0]:.2e} {dt:.0f}s")
            print(f"[sr] {line}", flush=True)
            log_f.write(line + "\n")
            log_f.flush()
            loss_acc = []
            torch.save({"model": model.state_dict(), "opt": opt.state_dict(),
                        "sched": sched.state_dict(), "iter": it,
                        "best_psnr": best_psnr, "args": args_dict},
                       ckpt_last)
            if v > best_psnr:
                best_psnr = v
                torch.save({"model": model.state_dict(), "iter": it,
                            "val_psnr": v, "args": args_dict},
                           out / "sr_best.pth")
                print(f"[sr]   ★ new best {v:.2f}", flush=True)
            model.train()

    print(f"[sr] 训练完成 best valPSNR={best_psnr:.2f} → {out}/sr_best.pth", flush=True)


# ─────────────────────────────────────────────────────────────────────────────
# eval / export 子命令
# ─────────────────────────────────────────────────────────────────────────────


def load_model(ckpt, device):
    ck = torch.load(ckpt, map_location="cpu")
    model = make_model(scale=ck.get("args", {}).get("scale", 2))
    model.load_state_dict(ck["model"] if "model" in ck else ck)
    model.eval().to(device)
    return model


def cmd_eval(args):
    device = torch.device(args.device) if args.device != "auto" else torch.device(
        "cuda" if torch.cuda.is_available()
        else "mps" if torch.backends.mps.is_available() else "cpu")
    model = load_model(args.ckpt, device)
    out = Path(args.out_dir or Path(args.ckpt).parent)

    sr_ps, bi_ps = [], []
    if args.data_dir:
        for name, lr_np, hr_np in build_val_images(args.data_dir, args.n_val, args.scale):
            p_sr, p_bi, sr = eval_image_pair(model, lr_np, hr_np, device,
                                             scale=args.scale, tile=args.tile)
            sr_ps.append(p_sr)
            bi_ps.append(p_bi)
            print(f"  {name}: SR {p_sr:.2f} dB | bilinear {p_bi:.2f} dB "
                  f"({p_sr - p_bi:+.2f})", flush=True)
            if args.compare:
                save_compare(lr_np, hr_np, sr, str(out / f"compare_{name}.png"))
        print(f"[eval] DIV2K val{args.n_val}: SR {np.mean(sr_ps):.2f} dB | "
              f"bilinear {np.mean(bi_ps):.2f} dB | gain {np.mean(sr_ps)-np.mean(bi_ps):+.2f}",
              flush=True)

    if args.game_dir:
        pairs = list_game_pairs(args.game_dir)
        sr_ps_g, bi_ps_g = [], []
        for lo, hr_p in pairs:
            lr_np, hr_np = read_image(lo), read_image(hr_p)
            p_sr, p_bi, sr = eval_image_pair(model, lr_np, hr_np, device,
                                             scale=args.scale, tile=args.tile)
            sr_ps_g.append(p_sr)
            bi_ps_g.append(p_bi)
            print(f"  game {Path(lo).stem}: SR {p_sr:.2f} dB | bilinear {p_bi:.2f} dB "
                  f"({p_sr - p_bi:+.2f})", flush=True)
            if args.compare:
                save_compare(lr_np, hr_np, sr,
                             str(out / f"compare_game_{Path(lo).stem}.png"))
        if pairs:
            print(f"[eval] game({len(pairs)}): SR {np.mean(sr_ps_g):.2f} dB | "
                  f"bilinear {np.mean(bi_ps_g):.2f} dB | "
                  f"gain {np.mean(sr_ps_g)-np.mean(bi_ps_g):+.2f}", flush=True)


def cmd_export(args):
    device = torch.device("cpu")
    model = load_model(args.ckpt, device)
    example = torch.rand(1, 3, 48, 48)
    traced = torch.jit.trace(model, example)
    traced.save(args.out)
    # 往返校验：不同尺寸都要一致
    for hw in [(48, 48), (360, 640), (128, 320)]:
        x = torch.rand(1, 3, *hw)
        with torch.no_grad():
            d = (traced(x) - model(x)).abs().max().item()
        assert d < 1e-5, f"trace mismatch @ {hw}: {d}"
    print(f"[export] TorchScript → {args.out} "
          f"(params {sum(p.numel() for p in model.parameters()):,}) 往返校验通过")


# ─────────────────────────────────────────────────────────────────────────────


def main():
    ap = argparse.ArgumentParser(description="2× SR 训练/评估/导出")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("--scale", type=int, default=2)
        p.add_argument("--device", default="auto", help="cuda|mps|cpu|auto")
        p.add_argument("--tile", type=int, default=192, help="全图推理瓦片 LR 尺寸")

    t = sub.add_parser("train")
    common(t)
    t.add_argument("--data_dir", required=True, help="含 DIV2K_train_HR/ DIV2K_valid_HR/")
    t.add_argument("--game_dir", default=None, help="low_XXX/ref_XXX 配对帧目录")
    t.add_argument("--game_repeat", type=int, default=1,
                   help="游戏配对重复采样倍数（提高域配比）")
    t.add_argument("--out", default="sr_output")
    t.add_argument("--iters", type=int, default=100000)
    t.add_argument("--batch", type=int, default=16)
    t.add_argument("--lr", type=float, default=2e-4)
    t.add_argument("--lr_min", type=float, default=1e-7)
    t.add_argument("--patch_hr", type=int, default=96)
    t.add_argument("--n_train", type=int, default=0, help="0=全部")
    t.add_argument("--n_val", type=int, default=10)
    t.add_argument("--eval_every", type=int, default=2000)
    t.add_argument("--noise_prob", type=float, default=0.3)
    t.add_argument("--workers", type=int, default=8)
    t.add_argument("--finetune_from", default=None)
    t.add_argument("--resume", action="store_true")
    t.set_defaults(fn=cmd_train)

    e = sub.add_parser("eval")
    common(e)
    e.add_argument("--ckpt", required=True)
    e.add_argument("--data_dir", default=None)
    e.add_argument("--game_dir", default=None)
    e.add_argument("--n_val", type=int, default=10)
    e.add_argument("--compare", action="store_true", help="输出对比条 PNG")
    e.add_argument("--out_dir", default=None)
    e.set_defaults(fn=cmd_eval)

    x = sub.add_parser("export")
    x.add_argument("--ckpt", required=True)
    x.add_argument("--out", default="sr_edsr2x.ts.pt")
    x.set_defaults(fn=cmd_export)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
