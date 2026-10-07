#!/usr/bin/env python3
"""SC per-mesh 计划六 ROI 门禁评估（对照 P0 基线）。

门禁语义沿用 compare_ddgi_lower_environment.py（原 1748 阶段固化）：
  - 参考值 [162, 22.5, 8.9, 120.5, 87, 59.5]
    （direct_patch / L_arcade / R_arcade / sunlit_wall / sky / floor_near）
  - 350/480 两帧各自计算 |measured − ref| 误差
  - 目标 ROI（R_arcade, floor_near）相对基线误差各改善 ≥ 1
  - 其余四 ROI 相对基线误差各恶化 ≤ 2
  - 两帧都过才算 PASS

用法:
  python3 scripts/sc_permesh_gate.py --baseline /tmp/scpermesh-baseline/baseline_six_roi.json \
      --dir /tmp/scpermesh-p2gate --label P2-static-pages
从 --dir 里取每个 vulkan-* 子目录的 frame.ppm 算 ROI（按 capture.json 的
capture_frame 或 frame 区分 350/480）。
"""
import argparse
import glob
import json
import os
import subprocess
import sys

REFERENCE = {
    "direct_patch": 162.0,
    "L_arcade": 22.5,
    "R_arcade": 8.9,
    "sunlit_wall": 120.5,
    "sky": 87.0,
    "floor_near": 59.5,
}
TARGETS = ("R_arcade", "floor_near")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def region_lum(ppm):
    out = subprocess.run(
        [sys.executable, os.path.join(ROOT, "scripts", "sponza_region_lum.py"), "--json", ppm],
        capture_output=True, text=True)
    return json.loads(out.stdout)[ppm]


def measure_dir(d):
    frames = {}
    for sub in sorted(glob.glob(os.path.join(d, "vulkan-*"))):
        ppm = os.path.join(sub, "frame.ppm")
        if not os.path.isfile(ppm):
            continue
        vals = region_lum(ppm)
        cap = os.path.join(sub, "capture.json")
        if not os.path.isfile(cap):
            raise ValueError(f"Missing capture metadata: {cap}")
        with open(cap) as f:
            metadata = json.load(f)
        frame = metadata.get("capture_frame", metadata.get("frame"))
        if frame not in (350, 480):
            raise ValueError(f"Invalid gate frame {frame}: {cap}")
        frames[int(frame)] = vals
    return frames


def configuration_mismatches(directory, expected):
    if not isinstance(expected,dict) or not expected:
        return [{"reason":"baseline environment provenance missing"}]
    mismatches=[]
    for sub in sorted(glob.glob(os.path.join(directory,"vulkan-*"))):
        if not os.path.isfile(os.path.join(sub,"frame.ppm")):continue
        path=os.path.join(sub,"capture.json")
        if not os.path.isfile(path):
            mismatches.append({"capture":path,"reason":"missing capture metadata"});continue
        with open(path) as f:metadata=json.load(f)
        actual=metadata.get("environment",{})
        for key,value in expected.items():
            observed=actual.get(key)
            if observed!=value:mismatches.append({"capture":path,"key":key,"baseline":value,"candidate":observed})
    return mismatches


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--dir", required=True)
    ap.add_argument("--label", default="candidate")
    args = ap.parse_args()

    with open(args.baseline) as f:
        baseline = json.load(f)
    mismatches=configuration_mismatches(args.dir,baseline.get("env",{}))
    if mismatches:
        report={"label":args.label,"status":"INCOMPARABLE","gate_pass":None,"configuration_mismatches":mismatches}
        out=os.path.join(args.dir,"gate.json")
        with open(out,"w") as f:json.dump(report,f,indent=2)
        print("INCOMPARABLE GATE: capture configuration differs from baseline ->",out)
        for row in mismatches:print(json.dumps(row,ensure_ascii=False))
        return 2
    base_err = baseline["abs_error_vs_reference"]
    frames = measure_dir(args.dir)
    required = {350, 480}
    if not required.issubset(frames) or not all(str(fr) in base_err for fr in required):
        print("INCOMPLETE GATE: both candidate and baseline require frames 350 and 480")
        return 2

    all_pass = True
    report = {"label": args.label, "frames": {}}
    for fr, vals in sorted(frames.items()):
        errs = {k: abs(vals[k] - REFERENCE[k]) for k in REFERENCE}
        bl = base_err[str(fr)] if str(fr) in base_err else base_err.get(fr)
        checks = {}
        for k in REFERENCE:
            delta = errs[k] - bl[k]  # 负 = 改善
            if k in TARGETS:
                checks[k] = {"error": round(errs[k], 3), "baseline": round(bl[k], 3),
                             "delta": round(delta, 3), "pass": bool(delta <= -1.0)}
            else:
                checks[k] = {"error": round(errs[k], 3), "baseline": round(bl[k], 3),
                             "delta": round(delta, 3), "pass": bool(delta <= 2.0)}
        frame_pass = all(c["pass"] for c in checks.values())
        all_pass &= frame_pass
        report["frames"][fr] = {"values": {k: round(v, 2) for k, v in vals.items()},
                                "errors": {k: round(v, 3) for k, v in errs.items()},
                                "checks": checks, "frame_pass": frame_pass}
        print(f"frame {fr}: {'PASS' if frame_pass else 'FAIL'}")
        for k, c in checks.items():
            mark = "✓" if c["pass"] else "✗"
            print(f"  {mark} {k:12s} err {c['error']:8.3f} (base {c['baseline']:8.3f}, delta {c['delta']:+8.3f})")
    report["gate_pass"] = all_pass
    out = os.path.join(args.dir, "gate.json")
    with open(out, "w") as f:
        json.dump(report, f, indent=2)
    print("GATE:", "PASS" if all_pass else "FAIL", "->", out)
    return 0 if all_pass else 1


if __name__ == "__main__":
    sys.exit(main())
