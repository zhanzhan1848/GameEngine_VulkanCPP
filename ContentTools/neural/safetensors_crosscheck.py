#!/usr/bin/env python
"""safetensors_crosscheck.py — 部署格式的 torch / python-MLX 双端交叉验证。

M2 前置基建（Docs/2026-09-15-neural-mlx-backend-plan.md §1.3"训练侧不变"的
格式桥）：safetensors 成为训练 ↔ 部署的共同格式后，训练侧必须能无损读回。
本脚本证明三端一致：
  torch(TinyDenoiser, safetensors 权重)  ==  denoiser_parity.bin 的期望
  python-MLX(同权重)                      ==  torch
  （第三端 Swift shim 已由 TestNeuralMLXShim 锁定 == torch）

用法: .venv/bin/python ContentTools/neural/safetensors_crosscheck.py \
          EngineTest/assets/Neural/denoiser.safetensors \
          EngineTest/assets/Neural/denoiser.pnn
"""
import json
import struct
import sys

import numpy as np
import torch
import mlx.core as mx
from safetensors.torch import load_file


def load_parity(path):
    blob = open(path, "rb").read()
    magic, ph, pw, cin, cout = struct.unpack_from("<5I", blob, 0)
    assert magic == 0x32504E50, hex(magic)   # "PNP2"
    pos = 20
    pin = np.frombuffer(blob, "<f4", cin * ph * pw, pos).reshape(cin, ph, pw)
    pos += cin * ph * pw * 4
    expect = np.frombuffer(blob, "<f4", cout * ph * pw, pos).reshape(cout, ph, pw)
    return pin, expect, ph, pw, cin, cout


def forward_numpy_reference(weights, meta, pin):
    """numpy 全前向（zero-pad conv + relu + log 残差 + expm1），第四端对照。"""
    layers = []
    for i in range(int(meta["layer_count"])):
        w = weights[f"layer{i}.weight"].numpy()   # [out,in,k,k]
        b = weights[f"layer{i}.bias"].numpy()
        layers.append((w, b))
    x = pin.astype(np.float32)
    for i, (w, b) in enumerate(layers):
        oc, ic, k, _ = w.shape
        pad = np.zeros((ic, x.shape[1] + 2, x.shape[2] + 2), np.float32)
        pad[:, 1:-1, 1:-1] = x
        out = np.zeros((oc, x.shape[1], x.shape[2]), np.float32)
        for o in range(oc):
            acc = np.zeros_like(out[o])
            for c in range(ic):
                for ky in range(k):
                    for kx in range(k):
                        acc += pad[c, ky:ky + x.shape[1], kx:kx + x.shape[2]] * w[o, c, ky, kx]
            out[o] = acc + b[o]
        x = np.maximum(out, 0) if i < len(layers) - 1 else out
    # log 残差 + expm1（parity 语义）
    x[:3] += pin[:3]
    return np.expm1(x)


def main():
    st_path, pnn_parity = sys.argv[1], sys.argv[2]
    weights = load_file(st_path)          # torch tensors
    with open(st_path, "rb") as f:
        n = struct.unpack("<Q", f.read(8))[0]
        meta = json.loads(f.read(n))["__metadata__"]
    pin, expect, ph, pw, cin, cout = load_parity(pnn_parity)

    # ---- torch 端 ----
    x = torch.from_numpy(pin[None])
    with torch.no_grad():
        acc = x
        for i in range(int(meta["layer_count"])):
            w = weights[f"layer{i}.weight"]
            b = weights[f"layer{i}.bias"]
            acc = torch.nn.functional.conv2d(acc, w, b, padding=1)
            if i < int(meta["layer_count"]) - 1:
                acc = torch.relu(acc)
        out = acc[0, 0:3] + x[0, 0:3]       # log 域残差
        out = torch.expm1(out)              # parity 语义 expm1
    t = out.numpy()
    err_torch = float(np.abs(t - expect).max())
    print(f"[torch]  max_err vs parity = {err_torch:.3e}")

    # ---- python MLX 端 ----
    # torch NCHW/权重 [O,I,k,k] → MLX NHWC/权重 [O,k,k,I]
    mxw = {k: mx.array(v.numpy()) for k, v in weights.items()}
    mxin = mx.array(pin)                                   # [C,H,W]
    acc = mxin.transpose(1, 2, 0)[None]                    # [1,H,W,C]
    nl = int(meta["layer_count"])
    for i in range(nl):
        w = mxw[f"layer{i}.weight"].transpose(0, 2, 3, 1)  # torch [O,I,k,k] → mlx [O,k,k,I]
        b = mxw[f"layer{i}.bias"]
        acc = mx.conv2d(acc, w, stride=1, padding=1) + b.reshape(1, 1, 1, -1)
        if i < nl - 1:
            acc = mx.maximum(acc, 0)
    outm = acc[0].transpose(2, 0, 1)                       # [3,H,W] log 域
    outm = outm[:3] + mxin[:3]
    outm = mx.expm1(outm)
    err_mlx = float(np.abs(np.array(outm) - expect).max())
    print(f"[mlx  ]  max_err vs parity = {err_mlx:.3e}")
    err_cross = float(np.abs(np.array(outm) - t).max())
    print(f"[cross]  torch vs mlx      = {err_cross:.3e}")

    ok = err_torch < 2e-4 and err_mlx < 2e-4 and err_cross < 2e-4
    print("CROSSCHECK", "PASSED" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
