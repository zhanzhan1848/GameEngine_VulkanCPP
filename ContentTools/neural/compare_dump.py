#!/usr/bin/env python
"""compare_dump.py — GPU 推理输出对拍门禁（PRIMAL_NEURAL_DUMP 产物）。

用法:
    .venv/bin/python ContentTools/neural/compare_dump.py /tmp/nd.raw 1280 720

读取 raw RGBA16F dump（左半屏 = MLP 重建，右半屏 = GT 参考函数），与 .pnn
权重逐像素重算的期望值比对（fp16 存储精度决定阈值），超阈值退出码 1。
"""
import struct
import sys

import numpy as np

REPO = __file__[: __file__.rfind("/ContentTools/")]
PNN = f"{REPO}/EngineTest/assets/Neural/test_image.pnn"

L, F, T = 16, 2, 1 << 14
ATOL = 2e-2          # fp16 存储粒度 ~1e-3，留余量
GT_ATOL = 5e-3       # GT 侧只有 sin/exp 库差异


def load_pnn(path):
    blob = open(path, "rb").read()
    magic, ver, layers, table_n, in_dim, out_dim, param_n, _ = struct.unpack_from("<8I", blob, 0)
    assert magic == 0x31574E50 and ver == 1
    off = 32
    res = struct.unpack_from(f"<{param_n}I", blob, off)
    off += param_n * 4
    descs = []
    for _ in range(layers):
        i, o, act, q, _pad = struct.unpack_from("<IIBBH", blob, off)
        off += 12
        descs.append((i, o, act))
    table = np.frombuffer(blob, np.float32, table_n, off)
    off += table_n * 4
    weights = []
    for (i, o, act) in descs:
        w = np.frombuffer(blob, np.float32, i * o, off).reshape(o, i)
        off += i * o * 4
        b = np.frombuffer(blob, np.float32, o, off)
        off += o * 4
        weights.append((w, b, act))
    assert off == len(blob)
    return res, table, weights, in_dim, out_dim


def encode(uv, res, table):
    n = uv.shape[0]
    feat = np.zeros((n, L * F + 2), np.float32)
    u, v = uv[:, 0], uv[:, 1]
    for l in range(L):
        r32 = np.float32(res[l])
        rm1 = np.float32(res[l] - 1)
        qx, qy = u * r32, v * r32
        flx, fly = np.floor(qx), np.floor(qy)
        i0 = np.minimum(flx, rm1)
        j0 = np.minimum(fly, rm1)
        fx, fy = qx - flx, qy - fly
        i1 = np.minimum(i0 + np.float32(1), rm1)
        j1 = np.minimum(j0 + np.float32(1), rm1)
        base = l * T * F

        def hsh(i, j):
            h = np.full(uv.shape[0], np.uint32(l), np.uint32)
            h = (h * np.uint32(0x9E3779B9)).astype(np.uint32)
            h = (h ^ (i.astype(np.uint32) * np.uint32(0x85EBCA6B))).astype(np.uint32)
            h = (h ^ (j.astype(np.uint32) * np.uint32(0xC2B2AE35))).astype(np.uint32)
            h = (h * np.uint32(0x27D4EB2F)).astype(np.uint32)
            h = (h ^ (h >> np.uint32(15))).astype(np.uint32)
            return (h & np.uint32(T - 1)).astype(np.int64)

        h00, h10, h01, h11 = hsh(i0, j0), hsh(i1, j0), hsh(i0, j1), hsh(i1, j1)
        w00 = ((1 - fx) * (1 - fy)).astype(np.float32)
        w10 = (fx * (1 - fy)).astype(np.float32)
        w01 = ((1 - fx) * fy).astype(np.float32)
        w11 = (fx * fy).astype(np.float32)
        for c in range(F):
            f00 = table[base + h00 * F + c]
            f10 = table[base + h10 * F + c]
            f01 = table[base + h01 * F + c]
            f11 = table[base + h11 * F + c]
            feat[:, l * F + c] = w00 * f00 + w10 * f10 + w01 * f01 + w11 * f11
    feat[:, L * F] = u
    feat[:, L * F + 1] = v
    return feat


def gt(u, v):
    def sstep(a, b, x):
        t = np.clip((x - a) / (b - a), 0, 1)
        return t * t * (3 - 2 * t)
    sky_lo = np.array([0.06, 0.12, 0.38], np.float32)
    sky_hi = np.array([1.00, 0.50, 0.18], np.float32)
    sky = sky_lo + (sky_hi - sky_lo) * sstep(0.12, 0.88, v)[..., None]
    d = np.sqrt((u - 0.62) ** 2 + (v - 0.40) ** 2)
    sun = np.exp(-d * 14.0) * 1.6
    rh = 0.30 + 0.08 * np.sin(u * 9.0) + 0.05 * np.sin(u * 23.0 + 1.7) + 0.02 * np.sin(u * 57.0 + 0.5)
    ridge = sstep(rh + 0.004, rh - 0.004, v)
    mtn_lo = np.array([0.05, 0.07, 0.10], np.float32)
    mtn_hi = np.array([0.16, 0.13, 0.17], np.float32)
    mtn = mtn_lo + (mtn_hi - mtn_lo) * sstep(0.0, 0.25, v)[..., None]
    col = sky * (1 - ridge[..., None]) + mtn * ridge[..., None]
    col = col + (sun * (1 - ridge) + sun * 0.25 * ridge)[..., None]
    return np.clip(col, 0, 1)


def main():
    raw, w, h = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    data = np.fromfile(raw, np.uint16)
    assert data.size == w * h * 4, f"size mismatch: {data.size} != {w*h*4}"
    # fp16 → f32（标准 IEEE half 位解码）
    img = data.reshape(h, w, 4).view(np.uint16).astype(np.uint32)
    sign = (img >> 15) & 1
    exp = (img >> 10) & 0x1F
    mant = img & 0x3FF
    norm = np.ldexp(np.float32(1.0) + mant.astype(np.float32) / np.float32(1024.0),
                    (exp.astype(np.int32) - 15))
    sub = (mant.astype(np.float32) * np.float32(2.0 ** -24))
    val = np.where(exp == 0, sub, norm).astype(np.float32)
    val = np.where(exp == 0x1F, np.float32(np.nan), val)
    f32 = np.where(sign == 1, -val, val)[..., :3]

    res, table, weights, in_dim, out_dim = load_pnn(PNN)

    xs = (np.arange(w) + 0.5) / w
    ys = (np.arange(h) + 0.5) / h
    uu, vv = np.meshgrid(xs, ys)
    uv = np.stack([uu.reshape(-1), vv.reshape(-1)], 1).astype(np.float32)

    feat = encode(uv, res, table)
    x = feat
    for (Wm, b, act) in weights:
        x = x @ Wm.T + b
        if act == 1:
            x = np.maximum(x, 0)
        else:
            x = 1 / (1 + np.exp(-x))
    nn_expect = x.reshape(h, w, 3).astype(np.float32)
    gt_expect = gt(uu, vv).astype(np.float32)

    half = w // 2
    nn_err = np.abs(f32[:, :half] - nn_expect[:, :half])
    gt_err = np.abs(f32[:, half:] - gt_expect[:, half:])
    print(f"NN  side: max={nn_err.max():.4f} mean={nn_err.mean():.5f}  (atol {ATOL})")
    print(f"GT  side: max={gt_err.max():.4f} mean={gt_err.mean():.5f}  (atol {GT_ATOL})")
    ok = nn_err.max() < ATOL and gt_err.max() < GT_ATOL
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
