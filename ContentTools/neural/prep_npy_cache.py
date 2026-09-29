#!/usr/bin/env python3
"""prep_npy_cache.py — 把 DIV2K PNG 批量转成 uint8 .npy（训练免反复 PNG 解码）。"""
import os
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
from PIL import Image

Image.MAX_IMAGE_PIXELS = None


def convert(args):
    src, dst = args
    if dst.exists():
        return 0
    a = np.array(Image.open(src).convert("RGB"), dtype=np.uint8)
    np.save(dst, a)
    return 1


def main(src_dir, cache_dir, workers):
    src, cache = Path(src_dir), Path(cache_dir)
    cache.mkdir(parents=True, exist_ok=True)
    jobs = [(f, cache / (f.stem + ".npy"))
            for f in sorted(src.glob("*.png"))]
    print(f"[cache] {len(jobs)} pngs → {cache}", flush=True)
    n = 0
    with ProcessPoolExecutor(workers) as ex:
        for i, r in enumerate(ex.map(convert, jobs, chunksize=8)):
            n += r
            if (i + 1) % 100 == 0:
                print(f"[cache] {i+1}/{len(jobs)}", flush=True)
    print(f"[cache] converted {n}, total {len(list(cache.glob('*.npy')))}", flush=True)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 16)
