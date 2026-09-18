#!/usr/bin/env python3
"""GI 能量验收统计（Docs/2026-09-18-gi-energy-root-cure-plan.md §4 验收表口径）。

指标（与 2026-09-18 indirect-brightening-investigation 实采对表校准，
三组 capture（current/gain1/scsupply0）meanY/dark/五 ROI 全部复现到 0.1）：
  meanY      全图 Rec.709 亮度均值（0-255 显示空间）
  dark       Y<32 像素占比
  arch       上拱前景拱腹 ROI（应最暗；UE 参考 18.9）
  alcove     右暗龛 ROI
  floor      中央地面（一跳反弹接收面，UE 参考 75.5）
  mlwall     中左墙（阳光直射立面，UE 参考 165.4）
  farwall    远墙（阳光直射立面，UE 参考 176.7）
  hierarchy  层次不变量：直射立面(max(mlwall,farwall)) > floor > arch

用法:
  python3 scripts/gi_roi_stats.py frame.png [frame2.png ...]
  python3 scripts/gi_roi_stats.py --json frame.png
"""
import argparse
import json
import sys

from PIL import Image
import numpy as np

# 1280x720 锁定参考机位像素盒（经 09-18 investigation 三组实采反演校准）。
ROIS = {
    "arch":    (40, 20, 440, 80),
    "alcove":  (240, 228, 540, 408),
    "floor":   (400, 390, 520, 720),
    "mlwall":  (480, 318, 660, 378),
    "farwall": (120, 168, 660, 468),
}

# UE Lumen 参考锚（audit 口径）。
UE_REF = {
    "meanY": 42.7, "dark": 0.613, "arch": 18.9, "alcove": 18.6,
    "floor": 75.5, "mlwall": 165.4, "farwall": 176.7,
}


def luminance(path):
    img = np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)
    return 0.2126 * img[:, :, 0] + 0.7152 * img[:, :, 1] + 0.0722 * img[:, :, 2]


def stats(path):
    Y = luminance(path)
    out = {
        "meanY": round(float(Y.mean()), 1),
        "dark": round(float((Y < 32.0).mean()), 3),
    }
    for name, (x0, y0, x1, y1) in ROIS.items():
        out[name] = round(float(Y[y0:y1, x0:x1].mean()), 1)
    facade = max(out["mlwall"], out["farwall"])
    out["hierarchy"] = "OK" if (facade > out["floor"] > out["arch"]) else "INVERTED"
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true")
    ap.add_argument("frames", nargs="+")
    args = ap.parse_args()
    results = {p: stats(p) for p in args.frames}
    if args.json:
        print(json.dumps(results, indent=2, ensure_ascii=False))
        return
    hdr = f"{'capture':<28}{'meanY':>7}{'dark':>7}{'arch':>7}{'alcove':>8}{'floor':>7}{'mlwall':>8}{'farwall':>9}  hierarchy"
    print(hdr)
    for p, s in results.items():
        print(f"{p:<28}{s['meanY']:>7}{s['dark']:>7}{s['arch']:>7}{s['alcove']:>8}"
              f"{s['floor']:>7}{s['mlwall']:>8}{s['farwall']:>9}  {s['hierarchy']}")
    print(f"{'UE_ref':<28}{UE_REF['meanY']:>7}{UE_REF['dark']:>7}{UE_REF['arch']:>7}"
          f"{UE_REF['alcove']:>8}{UE_REF['floor']:>7}{UE_REF['mlwall']:>8}{UE_REF['farwall']:>9}  OK")


if __name__ == "__main__":
    main()
