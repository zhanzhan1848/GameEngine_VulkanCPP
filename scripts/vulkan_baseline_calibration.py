#!/usr/bin/env python3
"""vulkan_baseline_calibration.py — Vulkan 重复基线方差标定（D3D12 M0 F0-15）.

对同一配置的 N 次 Sponza 运行的逐帧 capture（PPM）做跨运行方差分析，产出
标定报告。用途：在引入 D3D12 跨后端视觉比较之前，先量化"同一后端自身"
run-to-run 的像素方差，作为视觉门禁阈值设定的下界依据。

=== 阈值冻结纪律（先于看数，必须遵守） ===
1. 本脚本输出的是【标定数据】，不是阈值。报告本身不得当作阈值来源引用。
2. 阈值冻结流程：
   a. 先写下阈值提案文件（--thresholds-proposal 输入，JSON：
      {"whole_image": {"mean_le": X, "max_le": Y}, "roi": {...}, "per_frame": {...}}），
      提案理由只能基于：算法性质（时序 pass 的理论方差下界）、已知 bug 库
      （如 SPGI mean 15/255 运行间方差）、像素统计语义——【不得】基于本报告
      的数字反推（那会把噪声冻结进阈值）。
   b. 然后才运行本脚本查看标定结果。
   c. 若提案阈值 < 标定实测方差（即阈值不可满足），判"该帧/指标不可门禁"，
      记录并换指标（如 ROI、放宽帧），而不是就地放宽阈值贴着实测值。
   d. 冻结后的阈值写入 PhaseDoc/门禁配置，附本报告路径与 SHA 作为证据。
3. 本报告头部的 "THRESHOLD FREEZE PROTOCOL" 段是上述流程的机器可读复述。

用法:
  python3 vulkan_baseline_calibration.py \
      --runs /path/run_1 /path/run_2 /path/run_3 \
      [--frames 40,80,120,160,200,240] [--roi x,y,w,h] \
      --report /path/report.json [--report-md /path/report.md] \
      [--thresholds-proposal /path/proposal.json]

输入目录约定: 每个运行目录含 reference_<frame>.ppm（SPONZA_CAPTURE_DIR 产物，
tag=reference；其他 tag 被忽略）。numpy 可用时自动启用向量化，否则退化为
纯字节统计（逐像素采样全量，速度慢但无依赖）。

统计口径:
  - 每帧每运行: SHA256、RGB 均值（0-255）。
  - 跨运行逐对 (i<j): 全图 |diff| 均值、|diff| 最大值、|diff|>2 与 >8 的像素
    占比；ROI（若提供）同口径。
  - 帧间聚合: 各帧对上述指标的最大/均值（即"该帧最坏运行对"）。
"""
import argparse
import hashlib
import itertools
import json
import os
import re
import sys
import datetime

try:
    import numpy as np
except ImportError:
    np = None

PPM_RE = re.compile(r"^reference_(\d+)\.ppm$")

PROTOCOL_HEADER = (
    "THRESHOLD FREEZE PROTOCOL: this report is CALIBRATION DATA, not thresholds. "
    "Thresholds must be proposed (from algorithmic/known-issue reasoning only) "
    "BEFORE reading this report, then frozen with this report attached as "
    "evidence. If a proposed threshold is below measured variance, the frame/"
    "metric is declared non-gateable and the metric changed -- never widen the "
    "threshold to fit the measurement."
)


def read_ppm(path):
    """读 P6 PPM，返回 (w, h, bytes_rgb)。"""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise ValueError(f"not P6: {path}")
    idx = 2
    parts = []
    while len(parts) < 3:
        while data[idx : idx + 1].isspace():
            idx += 1
        if data[idx : idx + 1] == b"#":
            while data[idx : idx + 1] != b"\n":
                idx += 1
            continue
        start = idx
        while not data[idx : idx + 1].isspace():
            idx += 1
        parts.append(int(data[start:idx]))
    idx += 1  # 单空白分隔符
    w, h, maxval = parts
    if maxval != 255:
        raise ValueError(f"maxval {maxval}: {path}")
    return w, h, data[idx : idx + w * h * 3]


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def channel_means(w, h, rgb):
    n = w * h
    if n == 0:
        return (0.0, 0.0, 0.0)
    total = sum(rgb)
    # RGB 平面交错；用切片和（C 级迭代）
    r = sum(rgb[0::3])
    g = sum(rgb[1::3])
    b = sum(rgb[2::3])
    return (r / n, g / n, b / n)


def diff_stats(rgb_a, rgb_b, roi=None):
    """纯字节版: (mean_abs_diff, max_abs_diff, frac>2, frac>8) over 0-255。"""
    if np is not None:
        a = np.frombuffer(rgb_a, dtype=np.uint8).astype(np.int16)
        b = np.frombuffer(rgb_b, dtype=np.uint8).astype(np.int16)
        d = np.abs(a - b)
        if roi is not None:
            x, y, w, h, iw = roi
            sel = np.zeros(d.shape[0] // 3, dtype=bool)
            sel = sel.reshape(iw, (d.shape[0] // 3) // iw)
            sel[y : y + h, x : x + w] = True
            sel = sel.reshape(-1)
            d = d.reshape(-1, 3)[sel].reshape(-1)
        if d.size == 0:
            return (0.0, 0, 0.0, 0.0)
        return (
            float(d.mean()),
            int(d.max()),
            float((d > 2).mean()),
            float((d > 8).mean()),
        )
    # 无 numpy：逐字节（采样全量）
    if roi is not None:
        raise SystemExit("--roi requires numpy (not installed)")
    diffs = [abs(x - y) for x, y in zip(rgb_a, rgb_b)]
    n = len(diffs)
    return (
        sum(diffs) / n,
        max(diffs),
        sum(1 for d in diffs if d > 2) / n,
        sum(1 for d in diffs if d > 8) / n,
    )


def main():
    ap = argparse.ArgumentParser(description="Vulkan repeated-baseline variance calibration")
    ap.add_argument("--runs", nargs="+", required=True, help="N 个运行 capture 目录")
    ap.add_argument("--frames", help="逗号分隔帧号；缺省=所有运行交集")
    ap.add_argument("--roi", help="x,y,w,h（全图像素坐标；需 numpy）")
    ap.add_argument("--report", required=True, help="报告 JSON 输出")
    ap.add_argument("--report-md")
    ap.add_argument("--thresholds-proposal", help="冻结前写好的阈值提案 JSON（只随报告附上）")
    args = ap.parse_args()

    runs = args.runs
    # 收集各运行帧集
    per_run = {}
    for r in runs:
        found = {}
        for name in os.listdir(r):
            m = PPM_RE.match(name)
            if m:
                found[int(m.group(1))] = os.path.join(r, name)
        per_run[r] = found
    if args.frames:
        want = [int(x) for x in args.frames.split(",")]
    else:
        sets = [set(v.keys()) for v in per_run.values()]
        want = sorted(set.intersection(*sets))
    missing = {r: [f for f in want if f not in per_run[r]] for r in runs}
    missing = {r: ms for r, ms in missing.items() if ms}
    if missing:
        print(f"missing frames: {missing}", file=sys.stderr)
        return 2

    roi = None
    if args.roi:
        x, y, w, h = (int(v) for v in args.roi.split(","))
        roi = (x, y, w, h, None)

    frames_out = []
    for frame in want:
        imgs = {}
        entry = {"frame": frame, "runs": {}, "pairs": []}
        first_dims = None
        for r in runs:
            path = per_run[r][frame]
            w, h, rgb = read_ppm(path)
            if first_dims is None:
                first_dims = (w, h)
            elif (w, h) != first_dims:
                print(f"frame {frame}: dims mismatch {w}x{h} vs {first_dims}", file=sys.stderr)
                return 2
            imgs[r] = rgb
            entry["runs"][r] = {
                "file": os.path.basename(path),
                "sha256": sha256_file(path),
                "rgb_mean": [round(v, 4) for v in channel_means(w, h, rgb)],
            }
        if roi is not None:
            roi = (roi[0], roi[1], roi[2], roi[3], first_dims[0])
        for r1, r2 in itertools.combinations(runs, 2):
            s = diff_stats(imgs[r1], imgs[r2], roi)
            entry["pairs"].append(
                {
                    "a": os.path.basename(r1),
                    "b": os.path.basename(r2),
                    "mean_abs_diff": round(s[0], 4),
                    "max_abs_diff": s[1],
                    "frac_gt2": round(s[2], 6),
                    "frac_gt8": round(s[3], 6),
                }
            )
        worst = max(p["mean_abs_diff"] for p in entry["pairs"])
        worst_max = max(p["max_abs_diff"] for p in entry["pairs"])
        entry["worst_pair_mean_abs_diff"] = round(worst, 4)
        entry["worst_pair_max_abs_diff"] = worst_max
        entry["sha256_all_equal"] = len({v["sha256"] for v in entry["runs"].values()}) == 1
        frames_out.append(entry)

    report = {
        "report_version": 1,
        "generated_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "protocol": PROTOCOL_HEADER,
        "numpy_used": np is not None,
        "runs": runs,
        "frames_requested": want,
        "roi": args.roi,
        "dims": list(first_dims) if frames_out else None,
        "frames": frames_out,
        "aggregate": {
            "max_over_frames_worst_pair_mean_abs_diff": round(
                max((f["worst_pair_mean_abs_diff"] for f in frames_out), default=0.0), 4
            ),
            "max_over_frames_worst_pair_max_abs_diff": max(
                (f["worst_pair_max_abs_diff"] for f in frames_out), default=0
            ),
            "frames_sha256_all_equal": sum(1 for f in frames_out if f["sha256_all_equal"]),
            "frames_total": len(frames_out),
        },
        "thresholds_proposal": None,
    }
    if args.thresholds_proposal:
        with open(args.thresholds_proposal, "r", encoding="utf-8") as f:
            report["thresholds_proposal"] = json.load(f)

    os.makedirs(os.path.dirname(os.path.abspath(args.report)), exist_ok=True)
    with open(args.report, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
        f.write("\n")

    if args.report_md:
        lines = [
            "# Vulkan 重复基线方差标定报告",
            "",
            f"生成: {report['generated_utc']} | numpy: {report['numpy_used']} | "
            f"runs: {len(runs)} | frames: {want}",
            "",
            PROTOCOL_HEADER,
            "",
            "| frame | worst-pair mean \\|diff\\| | worst-pair max \\|diff\\| | SHA 全等 |",
            "|---|---|---|---|",
        ]
        for f_ in frames_out:
            lines.append(
                f"| {f_['frame']} | {f_['worst_pair_mean_abs_diff']} | "
                f"{f_['worst_pair_max_abs_diff']} | {f_['sha256_all_equal']} |"
            )
        lines += [
            "",
            f"聚合: max-over-frames worst-pair mean={report['aggregate']['max_over_frames_worst_pair_mean_abs_diff']}, "
            f"max={report['aggregate']['max_over_frames_worst_pair_max_abs_diff']}, "
            f"SHA 全等帧 {report['aggregate']['frames_sha256_all_equal']}/{report['aggregate']['frames_total']}",
        ]
        with open(args.report_md, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")

    print(json.dumps(report["aggregate"], indent=2))
    print(f"report: {args.report}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
