#!/usr/bin/env python3
"""Region luminance table for the Sponza reference viewpoint (C-series gate).

Measures the physical-relation regions defined in
Docs/2026-09-09-gi-physical-relation-calibration-plan.md §8 on a captured
frame (PPM/PNG) of the locked reference camera:

    direct_patch / interior ratio  (2026-09-12 rebaseline: band [2.3, 4.6],
                                    mean 3.3 over 5 locked runs — the old
                                    ">= 8" gate died in the B-series
                                    tonemap/sky rebalance, commit 0090b2e6;
                                    see §8 rebaseline note)
    interior floor < sunlit wall   (plan gate: the reported symptom, inverted)

Usage:
    python3 scripts/sponza_region_lum.py frame.png [frame2.ppm ...]
    python3 scripts/sponza_region_lum.py --json frame.png
    python3 scripts/sponza_region_lum.py --arcade hardvis.ppm [...]
        Arcade-camera (SPGI_CAM=0,5,0,0,0 + SPGI_CAM_LOCK) shadow-chain
        sentinel: feed PRIMAL_SHADOW_DBG_HARD=1 + PRIMAL_SHADOW_VIS_DEBUG=1
        captures (grayscale ≈ native vis below ~0.7; vis=1 saturates ~199
        post-rebalance). Gates: pure-soffit box must read ~0 (mean <= 25/255,
        sits under the B16 leak band 0.1-0.3; 2026-09-12 measured 14.3),
        sun band 0.51-0.65, interior box <= 0.12. Baselines: Docs plan §8.1.
"""
import argparse
import json
import sys

from PIL import Image
import numpy as np

# Normalized boxes (x0, x1, y0, y1) on the 1280x720 reference view.
REGIONS = {
    # C4.1 (2026-09-10): direct_patch added — the real sun band on the far
    # courtyard wall, measured from the GI_ALL_OFF capture (lum>60 threshold
    # map, y>=0.24 to exclude arch sky). The legacy sunlit_wall box sits
    # BELOW the band and contains NO direct light (GI_ALL_OFF = 0.0 there),
    # so "direct÷interior" gates now reference direct_patch; sunlit_wall is
    # kept as an indirect-bounce diagnostic.
    "direct_patch":   (0.46, 0.60, 0.26, 0.44),
    "L_arcade":       (0.00, 0.22, 0.42, 0.78),
    "R_arcade":       (0.80, 1.00, 0.42, 0.78),
    "sunlit_wall":    (0.42, 0.58, 0.50, 0.75),
    "sky":            (0.44, 0.55, 0.10, 0.22),
    "floor_near":     (0.30, 0.70, 0.88, 1.00),
}

# 2026-09-09 diagnosis baseline (Docs plan §8) for quick A/B reading.
# 2026-09-10: Metal_0910 = post miss-split + ×2.5-withdrawal equilibrium.
# 2026-09-12: Vulkan_0912 = current gate baseline — 5 locked-camera runs at
# frame 4800 on HEAD+B17 working tree (post B-series rebalance). Vulkan_0910
# numbers (direct 113 / interior 13, ratio 8.7) are OBSOLETE for gating: the
# rebalance lifted interior ~2.7x and sky, direct unchanged (see §8).
BASELINE = {
    "UE_ref":        {"L_arcade": 22.5, "R_arcade": 8.9,  "sunlit_wall": 120.5, "sky": 87.0,  "floor_near": 59.5},
    "Vulkan_0909":   {"L_arcade": 10.7, "R_arcade": 16.8, "sunlit_wall": 25.8,  "sky": 140.2, "floor_near": 15.8},
    "Metal_0909":    {"L_arcade": 10.2, "R_arcade": 40.4, "sunlit_wall": 153.1, "sky": 162.0, "floor_near": 195.6},
    "Metal_0910":    {"direct_patch": 129.9, "L_arcade": 7.2, "R_arcade": 26.7, "sunlit_wall": 66.4, "sky": 148.7, "floor_near": 108.0},
    "Vulkan_0912":   {"direct_patch": 117.0, "L_arcade": 41.1, "R_arcade": 29.2, "sunlit_wall": 23.2, "sky": 178.3, "floor_near": 16.3},
}

# 2026-09-12 rebaseline (Docs plan §8): display-space ratio band around the
# measured mean 3.3. Run spread over 5 captures was 3.26-3.52 (±5%); the band
# is deliberately wider (±40%) to absorb the SC↔probe multistability (T8.13)
# and ±5/255 GI noise while still catching sign-level moves — interior blowup
# (leak-like) collapses the ratio below ~2, interior starvation (dead GI)
# pushes it above ~5.
GATE_RATIO_BAND = (2.3, 4.6)

# Arcade-camera shadow-chain sentinel (Docs plan §8.1, 2026-09-12). Pixel
# boxes on the 1280x720 locked view (SPGI_CAM=0,5,0,0,0); the soffit box
# avoids the real-lit inner arc curve (B17: that arc is true sunlight).
ARCADE_BOXES = {
    "arc_interior": (480, 140, 570, 240),
    "arc_soffit":   (500, 180, 560, 230),
    "arc_sunband":  (580, 20, 780, 110),
}
# DBG_HARD vis gates (means in 0-255 display units):
#   soffit must stay ~0 — above ~25 (0.098) means the thin-occluder leak
#     (B16 band vis 0.1-0.3) is back; measured floor 14.3 (PCSS penumbra +
#     half-res upsample noise, uniform, not leak — B17 pure-compare proved
#     frac(recv<map)=0);
#   sun band is the production compare sanity anchor (2026-09-12: 141.7,
#     B17 band 0.57-0.64 with GI wobble);
#   interior box is the coarse backstop above the 0.082 measured floor.
ARCADE_GATES = {
    "arc_soffit":  (0.0, 25.0),
    "arc_sunband": (130.0, 165.0),
    "arc_interior": (0.0, 31.0),
}


def arcade_lums(path):
    img = np.asarray(Image.open(path).convert("RGB"), dtype=float)
    h, w, _ = img.shape
    if (w, h) != (1280, 720):
        raise SystemExit(f"{path}: arcade boxes expect 1280x720, got {w}x{h}")
    out = {}
    for name, (x0, y0, x1, y1) in ARCADE_BOXES.items():
        b = img[y0:y1, x0:x1].reshape(-1, 3).mean(axis=0)
        out[name] = 0.2126 * b[0] + 0.7152 * b[1] + 0.0722 * b[2]
    return out


def region_lums(path):
    img = np.asarray(Image.open(path).convert("RGB"), dtype=float)
    h, w, _ = img.shape
    out = {}
    for name, (x0, x1, y0, y1) in REGIONS.items():
        b = img[int(y0 * h):int(y1 * h), int(x0 * w):int(x1 * w)]
        m = b.reshape(-1, 3).mean(axis=0)
        out[name] = 0.2126 * m[0] + 0.7152 * m[1] + 0.0722 * m[2]
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("frames", nargs="+")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    ap.add_argument("--arcade", action="store_true",
                    help="shadow-chain sentinel mode (DBG_HARD vis captures, "
                         "arcade camera — see module docstring)")
    args = ap.parse_args()

    rows = {}
    for path in args.frames:
        rows[path] = arcade_lums(path) if args.arcade else region_lums(path)

    if args.json:
        print(json.dumps(rows, indent=2))
        return 0

    names = ARCADE_BOXES if args.arcade else REGIONS
    hdr = f"{'region':<14}" + "".join(f"{p.split('/')[-1]:>18.18}" for p in rows)
    print(hdr)
    for name in names:
        line = f"{name:<14}"
        for p in rows:
            line += f"{rows[p][name]:>18.1f}"
        print(line)

    if args.arcade:
        fail = False
        for p, r in rows.items():
            for name, (lo, hi) in ARCADE_GATES.items():
                if not (lo <= r[name] <= hi):
                    print(f"FAIL {p}: {name} {r[name]:.1f} outside "
                          f"[{lo:.0f}, {hi:.0f}] (vis sentinel; leak band "
                          f"0.1-0.3 x255 = 25-77, see Docs §8.1)")
                    fail = True
        return 1 if fail else 0
    line = f"{'sunlit/int':<14}"
    for p in rows:
        r = rows[p]
        interior = (r["L_arcade"] + r["R_arcade"]) / 2.0
        ratio = r["sunlit_wall"] / interior if interior > 1e-3 else float("inf")
        line += f"{ratio:>18.2f}"
    print(line)

    fail = False
    for p, r in rows.items():
        interior = (r["L_arcade"] + r["R_arcade"]) / 2.0
        # Gate 1 (2026-09-12 rebaseline): display-space direct/interior ratio
        # band. Replaces the pre-rebalance ">= 8" plan gate — the B-series
        # tonemap/sky rebalance (0090b2e6) moved interior 13→35 while direct
        # stayed ~117 (see module docstring and Docs §8 rebaseline note).
        # Gate 2 pins the physical symptom relation, Gate 3 the display-chain
        # absolute level (unchanged by the rebalance: direct ~117 ∈ [40,170]).
        ratio = r["direct_patch"] / interior if interior > 1e-3 else float("inf")
        if r["direct_patch"] > 1e-3 and r["floor_near"] > r["direct_patch"]:
            print(f"FAIL {p}: floor_near {r['floor_near']:.1f} > direct_patch "
                  f"{r['direct_patch']:.1f} (symptom: interior brighter than direct)")
            fail = True
        if not (GATE_RATIO_BAND[0] <= ratio <= GATE_RATIO_BAND[1]):
            print(f"FAIL {p}: direct_patch/interior ratio {ratio:.2f} outside "
                  f"rebaselined band {GATE_RATIO_BAND} (2026-09-12 baseline "
                  f"3.3 ±40%; old '>= 8' gate obsolete after B-series rebalance)")
            fail = True
        if not (40.0 <= r["direct_patch"] <= 170.0):
            print(f"FAIL {p}: direct_patch {r['direct_patch']:.1f} outside physical "
                  f"band [40, 170] (pure-direct capture 54.4 + GI interreflection)")
            fail = True
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
