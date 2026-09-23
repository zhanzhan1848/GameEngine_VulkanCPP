#!/usr/bin/env python
"""make_dataset.py — 解析 NeuralCaptureModule 的 take 文件 → 训练就绪 npz + 预览。

用法:
    .venv/bin/python ContentTools/neural/make_dataset.py /tmp/neural_capture [--preview]

take 文件 = <dir>/take_<tag>_<i>.bin + .json。二进制布局 = planes 顺序
连续平铺，每平面 w*h*16B（rgba32f，行主序）：
    [gt_mean, gt_var, depth_raw, normal, noisy_1spp]
深度线性化在读取侧完成（透视反投影，需要 json 的 near/far）。

输出: <dir>/dataset_take_<i>.npz（键: gi_noisy, gt_mean, gt_var, depth_lin,
normal）与 --preview 时的 side-by-side PNG（左噪声右 GT）。
"""
import json
import os
import struct
import sys

import numpy as np

PLANES = ["gt_mean", "gt_var", "depth_raw", "normal", "noisy_1spp"]


def linearize_depth(d, near, far):
    """OpenGL 约定 [0,1] 非线性深度 → 视空间线性深度（正数，单位与 far 一致）。"""
    return (near * far) / (far - d * (far - near))


def load_take(bin_path, json_path):
    meta = json.load(open(json_path))
    w, h = meta["w"], meta["h"]
    texel = meta["plane_texel_bytes"]
    raw = np.fromfile(bin_path, np.uint8)
    expected = w * h * texel * len(PLANES)
    assert raw.size == expected, f"{bin_path}: {raw.size} != {expected}"
    f32 = raw.view("<f4").reshape(len(PLANES), h, w, 4)
    out = {"w": w, "h": h, "meta": meta}
    out["gt_mean"] = f32[0, ..., :3].astype(np.float32)
    out["gt_var"] = f32[1, ..., :3].astype(np.float32)
    d = f32[2, ..., 0].astype(np.float32)
    out["depth_lin"] = linearize_depth(d, meta["near"], meta["far"]).astype(np.float32)
    out["normal"] = f32[3, ..., :3].astype(np.float32)
    out["gi_noisy"] = f32[4, ..., :3].astype(np.float32)
    return out


def save_preview(take, path):
    h, w = take["h"], take["w"]
    def to_srgb(c):
        c = np.clip(1.0 - np.exp(-c * 1.6), 0, 1) ** (1 / 2.2)
        return (c * 255 + 0.5).astype(np.uint8)
    noisy = to_srgb(take["gi_noisy"])
    gt = to_srgb(take["gt_mean"])
    gap = np.full((h, 2, 3), 24, np.uint8)
    open(path, "wb").write(b"P6\n%d %d\n255\n" % (w * 2 + 2, h))
    with open(path, "ab") as f:
        f.write(np.concatenate([noisy, gap, gt], 1).tobytes())


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "/tmp/neural_capture"
    preview = "--preview" in sys.argv
    jsons = sorted(f for f in os.listdir(d) if f.endswith(".json") and f.startswith("take_"))
    if not jsons:
        print(f"no take files in {d}")
        return 1
    for jf in jsons:
        base = jf[:-5]
        take = load_take(os.path.join(d, base + ".bin"), os.path.join(d, jf))
        m = take["meta"]
        noise_std = np.sqrt(np.maximum(take["gt_var"], 0)).mean()
        recon = np.abs(take["gi_noisy"] - take["gt_mean"]).mean()
        print(f"{base}: {m['w']}x{m['h']} frames={m['frames']} resets={m['camera_resets']} "
              f"mean|noisy-gt|={recon:.5f} avg_var_std={noise_std:.5f}")
        npz = os.path.join(d, f"dataset_{base}.npz")
        np.savez_compressed(
            npz,
            gi_noisy=take["gi_noisy"], gt_mean=take["gt_mean"],
            gt_var=take["gt_var"], depth_lin=take["depth_lin"],
            normal=take["normal"])
        print(f"  -> {npz}")
        if preview:
            png = os.path.join(d, f"preview_{base}.ppm")
            save_preview(take, png)
            print(f"  -> {png} (左噪声 | 右GT)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
