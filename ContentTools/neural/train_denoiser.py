#!/usr/bin/env python
"""train_denoiser.py — M1 神经降噪器离线训练（PyTorch/MPS）。

数据：NeuralCaptureModule 采集的 take（make_dataset.py 产出的 npz）。
  输入 10ch = noisy GI(3, log1p) + sqrt(var)(3) + depth_lin 归一化(1) + normal(3)
  目标     = GT mean GI(3, log1p)；网络预测残差（out = noisy_log + net(x)）。

模型：4 层 3×3 conv（10→24→24→16→3），~11k 参数 —— NRC 量级，为后续
引擎内 compute kernel 推理预留（.pnn v2 conv 层）。

用法:
  .venv/bin/python ContentTools/neural/train_denoiser.py \
      /tmp/neural_capture --train 0,1,2 --test 3 --steps 1200
支持断点续训（checkpoint 存 /tmp/neural_capture/denoiser_ckpt.pt）。
"""
import argparse
import os
import struct
import sys

import numpy as np
import torch
import torch.nn as nn

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def load_take(d, idx, tag, channels=10):
    z = np.load(os.path.join(d, f"dataset_take_{tag}_{idx}.npz"))
    noisy = z["gi_noisy"].astype(np.float32)
    var = z["gt_var"].astype(np.float32)
    depth = z["depth_lin"].astype(np.float32)
    normal = z["normal"].astype(np.float32)
    gt = z["gt_mean"].astype(np.float32)
    h, w = noisy.shape[:2]
    feats = np.zeros((h, w, channels), np.float32)
    feats[..., 0:3] = np.log1p(noisy)
    # 2026-09-22 修复：7ch 分支此前漏写 normal（ch3..5 训练时恒 0），而引擎
    # pack 喂真实法线——训练/运行时输入分布错配（NeuralMLXGate 实测同权重
    # 下真实法线比零法线低 ~3dB）。现与引擎 pack 语义对齐：ch3..5 = normal。
    feats[..., 3:6] = normal
    if channels == 10:
        # 真方差通道：运行时不可得（需历史累积），仅离线上限参考。
        feats[..., 3:6] = np.sqrt(np.maximum(var, 0.0))
        feats[..., 7:10] = normal
    # 深度归一化到 ~[0,1]（Sponza 室内 99 分位 ~13，固定 16 归一化保持
    # 训练/引擎 kernel 端一致——引擎侧直接 depth/16，无需分位数统计）。
    feats[..., 6] = np.clip(depth / 16.0, 0, 1)
    target = np.log1p(gt)
    dmax = max(float(gt.max()), 1e-3)
    return feats, target, noisy, gt, dmax


def psnr(a, b, dr):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return 10.0 * np.log10(dr * dr / max(mse, 1e-12))


class TinyDenoiser(nn.Module):
    def __init__(self, cin=10, width=24):
        super().__init__()
        # R4.2 架构搜索：width=32（7→32→32→32→3）。注意引擎 kernel 路径
        # kMaxConvChannels=24 只吃 width<=24；width=32 仅 MLX shim 路径可用
        # （shim 按元数据重建层图）。
        self.dims = [cin, width, width, max(width * 2 // 3, 8), 3]
        layers = []
        for i in range(3):
            layers += [nn.Conv2d(self.dims[i], self.dims[i + 1], 3, padding=1), nn.ReLU()]
        layers += [nn.Conv2d(self.dims[3], 3, 3, padding=1)]
        self.net = nn.Sequential(*layers)
        for m in self.net:
            if isinstance(m, nn.Conv2d):
                nn.init.kaiming_normal_(m.weight, nonlinearity="relu")
                nn.init.zeros_(m.bias)
        nn.init.zeros_(self.net[-1].weight)   # 残差零初始化：起点 = 输入本身
        nn.init.zeros_(self.net[-1].bias)

    def forward(self, x):
        return x[:, 0:3] + self.net(x)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--tag", default="sponza")
    ap.add_argument("--channels", type=int, default=7, choices=(7, 10))
    ap.add_argument("--width", type=int, default=24, help="R4.2 架构搜索：隐层宽度（引擎 kernel 上限 24）")
    ap.add_argument("--export-pnn", default="")
    ap.add_argument("--train", default="0,1,2")
    ap.add_argument("--test", default="3")
    ap.add_argument("--steps", type=int, default=1200)
    ap.add_argument("--crop", type=int, default=128)
    ap.add_argument("--batch", type=int, default=8)
    ap.add_argument("--lr", type=float, default=2e-3)
    args = ap.parse_args()

    dev = "mps" if torch.backends.mps.is_available() else "cpu"
    torch.manual_seed(7)

    train_ids = [int(x) for x in args.train.split(",") if x != ""]
    test_ids = [int(x) for x in args.test.split(",") if x != ""]
    feats = {}
    for i in sorted(set(train_ids + test_ids)):
        feats[i] = load_take(args.dir, i, args.tag, args.channels)
        print(f"[data] take {i}: feats {feats[i][0].shape}")

    model = TinyDenoiser(cin=args.channels, width=args.width).to(dev)
    nparam = sum(p.numel() for p in model.parameters())
    print(f"[model] params={nparam}")
    opt = torch.optim.Adam(model.parameters(), lr=args.lr)
    ckpt = os.path.join(args.dir, f"denoiser_ckpt_{args.channels}ch_w{args.width}.pt")
    start = 0
    if os.path.exists(ckpt):
        ck = torch.load(ckpt, map_location=dev)
        model.load_state_dict(ck["model"])
        opt.load_state_dict(ck["opt"])
        start = ck["step"] + 1
        print(f"[ckpt] resumed at step {start}")

    crop = args.crop
    h, w = feats[train_ids[0]][0].shape[:2]

    def rand_batch():
        xs, ys = [], []
        for _ in range(args.batch):
            i = train_ids[np.random.randint(len(train_ids))]
            f, tgt, _, _, _ = feats[i]
            y = np.random.randint(0, h - crop)
            x = np.random.randint(0, w - crop)
            xs.append(f[y:y + crop, x:x + crop])
            ys.append(tgt[y:y + crop, x:x + crop])
        x = torch.from_numpy(np.stack(xs).transpose(0, 3, 1, 2)).to(dev)
        y = torch.from_numpy(np.stack(ys).transpose(0, 3, 1, 2)).to(dev)
        return x, y

    model.train()
    for step in range(start, start + args.steps):
        x, y = rand_batch()
        pred = model(x)
        loss = torch.mean((pred - y) ** 2)
        opt.zero_grad(set_to_none=True)
        loss.backward()
        opt.step()
        if step % 100 == 0 or step == start + args.steps - 1:
            print(f"[train] step {step:5d} loss {loss.item():.5f}", flush=True)
    torch.save({"model": model.state_dict(), "opt": opt.state_dict(),
                "step": start + args.steps - 1}, ckpt)

    # ---- 评测：留出 take 的 PSNR（线性 HDR 域）----
    model.eval()
    print("[eval] held-out takes:")
    for i in test_ids:
        f, _, noisy, gt, dmax = feats[i]
        x = torch.from_numpy(f.transpose(2, 0, 1)[None]).to(dev)
        with torch.no_grad():
            pred_log = model(x)[0].cpu().numpy().transpose(1, 2, 0)
        den = np.expm1(np.clip(pred_log, 0, 12)).astype(np.float32)
        p_noisy = psnr(noisy, gt, dmax)
        p_den = psnr(den, gt, dmax)
        rel = np.mean(np.abs(noisy - gt)) / max(np.mean(gt), 1e-6)
        rel_d = np.mean(np.abs(den - gt)) / max(np.mean(gt), 1e-6)
        print(f"  take {i}: PSNR noisy={p_noisy:.2f} dB -> denoised={p_den:.2f} dB "
              f"(+{p_den - p_noisy:.2f}); relL1 {rel:.3f} -> {rel_d:.3f}")

    # ---- .pnn v2 导出（conv 网络给引擎 NeuralDenoiseModule）----
    if args.export_pnn:
        if args.width != 24:
            print(f"[export] SKIP pnn（width={args.width} 引擎 kernel 上限 24；仅离线对比用）")
        else:
            export_pnn_v2(args.export_pnn, model, args.channels)


def export_pnn_v2(path, model, cin):
    """v2：头 32B 同 v1；LayerDesc 24B/条 {kind,in,out,act,k,stride,pad,flags,res}；
    conv 权重 = W[out][in][k*k] row-major + b[out]（PyTorch (out,in,kH,kW) 展平）。
    flags bit0 = 末层 expm1（引擎 kernel 直接输出线性 HDR）。"""
    model = model.to("cpu").eval()
    lins = [m for m in model.net if isinstance(m, torch.nn.Conv2d)]
    dims = [cin, 24, 24, 16, 3]
    blob = bytearray(struct.pack("<8I", 0x31574E50, 2, 4, 0, cin, 3, 0, 1))   # flags bit0 = residual_rgb
    for i in range(4):
        act = 1 if i < 3 else 0          # 隐层 relu；末层线性 + expm1 flag
        flags = 1 if i == 3 else 0
        blob += struct.pack("<IIIBBBBII", 1, dims[i], dims[i + 1],
                            act, 3, 1, 1, flags, 0)
    with torch.no_grad():
        for i, l in enumerate(lins):
            w = l.weight.detach().cpu().numpy().astype(np.float32)   # (out,in,k,k)
            b = l.bias.detach().cpu().numpy().astype(np.float32)
            blob += np.ascontiguousarray(w).tobytes()
            blob += np.ascontiguousarray(b).tobytes()
    open(path, "wb").write(blob)
    print(f"[export] {path} ({len(blob)} bytes, .pnn v2 conv)")

    # parity 向量：随机 16×16×cin 输入 + torch 全前向期望（CPU f32）。
    rng = np.random.default_rng(11)
    ph = pw = 16
    pin = rng.random((ph, pw, cin)).astype(np.float32) * np.float32(2.0)
    with torch.no_grad():
        # flags bit0 告知消费者做 expm1 —— 期望值同样必须是线性 HDR 域。
        out = np.expm1(model(torch.from_numpy(pin.transpose(2, 0, 1)[None]))[0].numpy().transpose(1, 2, 0))
    ppath = os.path.splitext(path)[0] + "_parity.bin"
    pb = bytearray(struct.pack("<4I", 0x32504E50, ph, pw, cin))   # "PNP2"
    pb += struct.pack("<I", 3)
    pb += pin.transpose(2, 0, 1).astype(np.float32).tobytes()   # CHW 平面布局
    pb += out.transpose(2, 0, 1).astype(np.float32).tobytes()   # CHW

    open(ppath, "wb").write(pb)
    print(f"[export] {ppath} ({len(pb)} bytes, conv parity)")



if __name__ == "__main__":
    sys.exit(main())
