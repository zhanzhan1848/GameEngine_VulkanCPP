#!/usr/bin/env python
"""M0 neural-rendering toy: hash-encoding + MLP fits a procedural 2D image.

This is the offline half of the PRIMAL neural stack (Layer A). It produces:
  EngineTest/assets/Neural/test_image.pnn   — trained weights (.pnn v1, f32)
  EngineTest/assets/Neural/test_parity.bin  — tri-end parity vectors
  /tmp/neural_preview_{gt,nn}.ppm           — visual previews

Everything numeric here is mirrored bit-consistently in four places:
  - this script (training + expected outputs)
  - Engine/Graphics/Neural/NeuralWeights.{h,cpp}   (CPU reference forward)
  - Engine/Graphics/Metal/shaders/Neural/MLPInferDebug.metal (MSL)
  - Engine/Graphics/Vulkan/shaders/Neural/MLPInfer.comp      (GLSL)

=== PRIMAL NEURAL SPEC v1 (do not edit one copy alone) ===
Encoding: multiresolution grid hash (InstantNGP-style, 2D)
  L = 16 levels, F = 2 features per level, T = 2^14 entries per level
  (one shared table laid out table[level][entry][F], f32).
  RES[l] = max(1, floor(BASE * GROWTH^l)), BASE = 4.0, GROWTH = 1.5,
  computed here in float64 and STORED as u32 in the .pnn — consumers read
  the integers, never recompute pow().
  For level l and input (u, v) in [0,1]^2:
      qx = u * RES[l];  qy = v * RES[l]
      i0 = min(floor(qx), RES-1);  j0 = min(floor(qy), RES-1)
      fx = qx - floor(qx);         fy = qy - floor(qy)
      i1 = min(i0+1, RES-1);       j1 = min(j0+1, RES-1)
      corner index (i, j, l):
          h  = (l * 0x9E3779B9)              mod 2^32
          h ^= (i * 0x85EBCA6B)              mod 2^32
          h ^= (j * 0xC2B2AE35)              mod 2^32
          h  = (h * 0x27D4EB2F)              mod 2^32
          h ^= (h >> 15)
          idx = h & (T-1)
      feature = bilinear interpolation of table[l][corner][0..1]
  Output = 32 interpolated features + raw (u, v) appended -> in_dim = 34.
MLP: 34 -> 64 (ReLU) -> 64 (ReLU) -> 64 (ReLU) -> 3 (sigmoid)
Dot products accumulate over i ascending in every implementation.
"""

import math
import os
import struct
import sys

import numpy as np
import torch

L, F, LOG2T = 16, 2, 14
T = 1 << LOG2T
BASE_RES, GROWTH = 4.0, 1.5
RES = [max(1, int(BASE_RES * (GROWTH ** l))) for l in range(L)]
HIDDEN, OUT_DIM = 64, 3
TRAIN_STEPS, LR = 2500, 2e-2
IMG = 256
N_PARITY = 64

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ASSET_DIR = os.path.join(REPO, "EngineTest", "assets", "Neural")

# ---------------------------------------------------------------------------
# Reference GT image (the function the network must learn). Mirrored in the
# two debug shaders and NeuralWeights.cpp — only clamps/smoothstep/exp/sin.
# ---------------------------------------------------------------------------
def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)

def gt_image(u, v):
    """u, v: float arrays in [0,1]. Returns (..., 3) in [0,1]."""
    sky_lo = np.array([0.06, 0.12, 0.38], np.float32)
    sky_hi = np.array([1.00, 0.50, 0.18], np.float32)
    sky = sky_lo + (sky_hi - sky_lo) * smoothstep(0.12, 0.88, v)[..., None]
    d = np.sqrt((u - 0.62) ** 2 + (v - 0.40) ** 2)
    sun = np.exp(-d * 14.0) * 1.6
    ridge_h = 0.30 + 0.08 * np.sin(u * 9.0) + 0.05 * np.sin(u * 23.0 + 1.7) \
        + 0.02 * np.sin(u * 57.0 + 0.5)
    ridge = smoothstep(ridge_h + 0.004, ridge_h - 0.004, v)
    mtn_lo = np.array([0.05, 0.07, 0.10], np.float32)
    mtn_hi = np.array([0.16, 0.13, 0.17], np.float32)
    mtn = mtn_lo + (mtn_hi - mtn_lo) * smoothstep(0.0, 0.25, v)[..., None]
    col = sky * (1.0 - ridge[..., None]) + mtn * ridge[..., None]
    col = col + (sun * (1.0 - ridge) + sun * 0.25 * ridge)[..., None]
    return np.clip(col, 0.0, 1.0)

# ---------------------------------------------------------------------------
# Hash encoding (numpy, uint32 wrap via & 0xFFFFFFFF)
# ---------------------------------------------------------------------------
_U32 = np.uint32(0xFFFFFFFF)

def _mul(a, b):
    return ((a * b) & _U32).astype(np.uint32)

def _xor(a, b):
    return (a ^ b).astype(np.uint32)

def hash_encode(uv, table):
    """uv: (N,2) float32. table: (L*T*F,) float32. Returns (N, L*F+2) float32.

    All grid math in float32 — the GPU kernels are float32, and the spec's
    tri-end parity holds only if the trainer matches (floor/mul round the
    same way). Do NOT promote to float64 here.
    """
    n = uv.shape[0]
    u = uv[:, 0]
    v = uv[:, 1]
    feat = np.zeros((n, L * F + 2), np.float32)
    for l in range(L):
        res32 = np.float32(RES[l])
        qx, qy = u * res32, v * res32
        i0 = np.minimum(np.floor(qx), np.float32(RES[l] - 1))
        j0 = np.minimum(np.floor(qy), np.float32(RES[l] - 1))
        fx, fy = qx - np.floor(qx), qy - np.floor(qy)
        i1 = np.minimum(i0 + np.float32(1), np.float32(RES[l] - 1))
        j1 = np.minimum(j0 + np.float32(1), np.float32(RES[l] - 1))
        base = l * T
        # table layout is [level][entry][F]: linear index = (l*T + idx)*F + comp
        def fetch(i, j, l=l, base=base):
            h = np.full(n, np.uint32(l), np.uint32)
            h = _mul(h, np.uint32(0x9E3779B9))
            h = _xor(h, _mul(i.astype(np.uint32), np.uint32(0x85EBCA6B)))
            h = _xor(h, _mul(j.astype(np.uint32), np.uint32(0xC2B2AE35)))
            h = _mul(h, np.uint32(0x27D4EB2F))
            h = _xor(h, (h >> np.uint32(15)).astype(np.uint32))
            return (h & np.uint32(T - 1)).astype(np.int64)
        h00, h10 = fetch(i0, j0), fetch(i1, j0)
        h01, h11 = fetch(i0, j1), fetch(i1, j1)
        w00 = (1 - fx) * (1 - fy); w10 = fx * (1 - fy)
        w01 = (1 - fx) * fy;       w11 = fx * fy
        for comp in range(F):
            f00, f10 = table[(base + h00) * F + comp], table[(base + h10) * F + comp]
            f01, f11 = table[(base + h01) * F + comp], table[(base + h11) * F + comp]
            feat[:, l * F + comp] = (w00 * f00 + w10 * f10 + w01 * f01 + w11 * f11).astype(np.float32)
    feat[:, L * F + 0] = uv[:, 0]
    feat[:, L * F + 1] = uv[:, 1]
    return feat

# ---------------------------------------------------------------------------
# Torch model (mirrors the spec)
# ---------------------------------------------------------------------------
class Net(torch.nn.Module):
    def __init__(self):
        super().__init__()
        self.table = torch.nn.Parameter(torch.empty(L * T * F, dtype=torch.float32).uniform_(-0.1, 0.1))
        dims = [L * F + 2] + [HIDDEN] * 3 + [OUT_DIM]
        self.linears = torch.nn.ModuleList(
            torch.nn.Linear(dims[i], dims[i + 1]) for i in range(len(dims) - 1))
        for lin in self.linears:
            torch.nn.init.kaiming_uniform_(lin.weight, a=math.sqrt(5.0))

    def forward(self, uv):
        # torch-side encoding, float32 on device (MPS has no float64; the spec
        # is float32 everywhere for GPU parity).
        n = uv.shape[0]
        feats = []
        u, v = uv[:, 0], uv[:, 1]
        tbl = self.table.reshape(L, T, F)
        for l in range(L):
            res32 = torch.tensor(float(RES[l]), device=uv.device)
            resm1 = torch.tensor(float(RES[l] - 1), device=uv.device)
            qx, qy = u * res32, v * res32
            i0 = torch.minimum(torch.floor(qx), resm1)
            j0 = torch.minimum(torch.floor(qy), resm1)
            fx, fy = qx - torch.floor(qx), qy - torch.floor(qy)
            i0l, j0l = i0.to(torch.int64), j0.to(torch.int64)
            i1l = torch.minimum(i0l + 1, torch.tensor(RES[l] - 1))
            j1l = torch.minimum(j0l + 1, torch.tensor(RES[l] - 1))
            lt = torch.tensor(l, dtype=torch.int64)
            corners = []
            for (i, j) in ((i0l, j0l), (i1l, j0l), (i0l, j1l), (i1l, j1l)):
                h = (lt * torch.tensor(0x9E3779B9, dtype=torch.int64))
                h = h ^ (i.to(torch.int64) * torch.tensor(0x85EBCA6B, dtype=torch.int64))
                h = h ^ (j.to(torch.int64) * torch.tensor(0xC2B2AE35, dtype=torch.int64))
                h = h & 0xFFFFFFFF
                h = (h * torch.tensor(0x27D4EB2F, dtype=torch.int64)) & 0xFFFFFFFF
                h = h ^ (h >> 15)
                idx = h & (T - 1)
                corners.append(tbl[l][idx])
            w00 = ((1 - fx) * (1 - fy)).unsqueeze(-1)
            w10 = (fx * (1 - fy)).unsqueeze(-1)
            w01 = ((1 - fx) * fy).unsqueeze(-1)
            w11 = (fx * fy).unsqueeze(-1)
            feats.append(w00 * corners[0] + w10 * corners[1] + w01 * corners[2] + w11 * corners[3])
        x = torch.cat(feats + [uv], dim=1)
        for k, lin in enumerate(self.linears):
            x = lin(x) if k == len(self.linears) - 1 else torch.relu(lin(x))
        return torch.sigmoid(x)

# ---------------------------------------------------------------------------
# Export
# ---------------------------------------------------------------------------
def export_pnn(path, net):
    header = struct.pack(
        "<7I", 0x31574E50, 1, 4, L * T * F, L * F + 2, OUT_DIM, L, )  # magic,ver,layers,table,in,out,params
    header += struct.pack("<I", 0)  # reserved -> 32B total
    blob = bytearray(header)
    blob += struct.pack("<16I", *RES)
    lin = list(net.linears)
    dims = [L * F + 2, HIDDEN, HIDDEN, HIDDEN, OUT_DIM]
    for i, d_in in enumerate(dims[:-1]):
        d_out = dims[i + 1]
        act = 1 if i < len(dims) - 2 else 2  # relu hidden, sigmoid out
        blob += struct.pack("<IIBBH", d_in, d_out, act, 0, 0)
    tbl = net.table.detach().cpu().numpy().astype(np.float32)
    blob += tbl.tobytes()
    for i, l in enumerate(lin):
        w = l.weight.detach().cpu().numpy().astype(np.float32)  # (out, in)
        b = l.bias.detach().cpu().numpy().astype(np.float32)
        blob += np.ascontiguousarray(w).tobytes()
        blob += np.ascontiguousarray(b).tobytes()
    with open(path, "wb") as f:
        f.write(blob)
    print(f"[export] {path} ({len(blob)} bytes)")

def export_parity(path, uv, encoded, out_rgb):
    blob = bytearray(struct.pack("<4I", 0x56504E50, 1, len(uv), L * F + 2))  # "PNPV" LE
    blob += struct.pack("<I", OUT_DIM)
    blob += uv.astype(np.float32).tobytes()
    blob += encoded.astype(np.float32).tobytes()
    blob += out_rgb.astype(np.float32).tobytes()
    with open(path, "wb") as f:
        f.write(blob)
    print(f"[export] {path} ({len(blob)} bytes)")

def write_ppm(path, img):
    h, w = img.shape[:2]
    data = (np.clip(img, 0, 1) ** (1.0 / 2.2) * 255 + 0.5).astype(np.uint8)
    with open(path, "wb") as f:
        f.write(f"P6\n{w} {h}\n255\n".encode())
        f.write(data.tobytes())

# ---------------------------------------------------------------------------
def main():
    os.makedirs(ASSET_DIR, exist_ok=True)
    dev = "mps" if torch.backends.mps.is_available() else "cpu"
    torch.manual_seed(1234)
    net = Net().to(dev)
    opt = torch.optim.Adam(net.parameters(), lr=LR)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, TRAIN_STEPS)

    ys, xs = np.meshgrid(np.arange(IMG) + 0.5, np.arange(IMG) + 0.5, indexing="ij")
    uv = np.stack([xs.reshape(-1) / IMG, ys.reshape(-1) / IMG], 1).astype(np.float32)
    target = torch.from_numpy(gt_image(uv[:, 0], uv[:, 1])).to(dev)

    uv_t = torch.from_numpy(uv).to(dev)
    for step in range(TRAIN_STEPS):
        opt.zero_grad(set_to_none=True)
        pred = net(uv_t)
        loss = torch.mean((pred - target) ** 2)
        loss.backward()
        opt.step()
        sched.step()
        if step % 250 == 0 or step == TRAIN_STEPS - 1:
            print(f"[train] step {step:5d}  loss {loss.item():.6f}", flush=True)

    # Parity vectors. Expected network output = full torch forward (MPS/CPU
    # f32) — accumulation order differs from the C++ reference, hence the
    # 1e-4 tolerance in TestNeuralWeights; encoding vectors are exact ops
    # (atol 1e-5).
    rng = np.random.default_rng(42)
    puv = rng.random((N_PARITY, 2)).astype(np.float32)
    table = net.table.detach().cpu().numpy().astype(np.float32)
    encoded = hash_encode(puv, table)
    with torch.no_grad():
        expected = net(torch.from_numpy(puv).to(dev)).cpu().numpy().astype(np.float32)

    export_pnn(os.path.join(ASSET_DIR, "test_image.pnn"), net)
    export_parity(os.path.join(ASSET_DIR, "test_parity.bin"), puv, encoded, expected)

    with torch.no_grad():
        final = net(uv_t).cpu().numpy().reshape(IMG, IMG, 3)
    write_ppm("/tmp/neural_preview_gt.ppm", gt_image(uv[:, 0], uv[:, 1]).reshape(IMG, IMG, 3))
    write_ppm("/tmp/neural_preview_nn.ppm", final)
    print("[done] previews: /tmp/neural_preview_gt.ppm /tmp/neural_preview_nn.ppm")

if __name__ == "__main__":
    main()
