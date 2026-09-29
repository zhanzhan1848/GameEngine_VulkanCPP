#!/usr/bin/env python3
"""build_game_pairs.py — 从两次引擎采集（PRIMAL_RENDER_SCALE=0.5 / 1.0）构建 SR 配对。

输入：low_dir 与 ref_dir 各含 reference_<n>.ppm（1280×720；low 为 FinalBlit
双线性上采样后的画面，ref 为原生渲染，同确定性相机同帧号）。
输出：<out>/low_<n+offset>.ppm（640×360，BOX 降回真实低清渲染）与
<out>/ref_<n+offset>.ppm（1280×720 原生），并打印 bilinear baseline PSNR。

用法:
  python build_game_pairs.py /tmp/sr_game_low /tmp/sr_game_ref /tmp/sr_game_pairs
  python build_game_pairs.py /tmp/sr_game_low2 /tmp/sr_game_ref2 /tmp/sr_game_pairs 1000
"""
import sys
from pathlib import Path

import numpy as np
import torch
import torch.nn.functional as FN
from PIL import Image


def read_ppm(p):
    with open(p, "rb") as f:
        assert f.readline().strip() == b"P6", f"不是 P6: {p}"
        w, h = map(int, f.readline().split())
        f.readline()
        return np.frombuffer(f.read(w * h * 3), np.uint8).reshape(h, w, 3)


def write_ppm(p, img_u8):
    h, w = img_u8.shape[:2]
    with open(p, "wb") as f:
        f.write(f"P6\n{w} {h}\n255\n".encode())
        f.write(img_u8.tobytes())


def main():
    low_dir, ref_dir, out_dir = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    offset = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    out_dir.mkdir(parents=True, exist_ok=True)

    refs = sorted(ref_dir.glob("reference_*.ppm"), key=lambda p: int(p.stem.split("_")[1]))
    psnrs = []
    for rp in refs:
        n = int(rp.stem.split("_")[1])
        lp = low_dir / f"reference_{n}.ppm"
        if not lp.exists():
            print(f"[pairs] 缺 low 侧 {n}，跳过")
            continue
        ref = read_ppm(rp).astype(np.float32) / 255.0
        low = read_ppm(lp).astype(np.float32) / 255.0
        lr = np.array(Image.fromarray((low * 255).astype(np.uint8))
                      .resize((ref.shape[1] // 2, ref.shape[0] // 2), Image.BOX),
                      dtype=np.float32) / 255.0
        t = torch.from_numpy(np.ascontiguousarray(lr.transpose(2, 0, 1))).unsqueeze(0)
        up = FN.interpolate(t, size=ref.shape[:2], mode="bilinear",
                            align_corners=False)[0].permute(1, 2, 0).numpy()
        psnrs.append(10 * np.log10(1.0 / max(float(np.mean((up - ref) ** 2)), 1e-10)))
        key = n + offset
        write_ppm(out_dir / f"low_{key}.ppm", (lr * 255).astype(np.uint8))
        write_ppm(out_dir / f"ref_{key}.ppm", (ref * 255).astype(np.uint8))
    print(f"[pairs] {len(psnrs)} 对 → {out_dir}")
    print(f"[pairs] bilinear baseline: mean {np.mean(psnrs):.2f} dB | "
          f"min {np.min(psnrs):.2f} | max {np.max(psnrs):.2f}")


if __name__ == "__main__":
    main()
