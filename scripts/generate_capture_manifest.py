#!/usr/bin/env python3
"""generate_capture_manifest.py — D3D12 M0 F0-14 capture manifest 生成器.

输入:
  --capture-dir DIR     PPM 序列目录（<tag>_<frame>.ppm，由 Sponza 测试
                        SPONZA_CAPTURE_FRAMES + SPONZA_CAPTURE_DIR 产出）
  --run-config FILE     运行配置 JSON（字段见下）
  --log FILE            可选：测试 stderr 日志；提供时核对 [FIXED_DT] 行
                        与配置 dt 一致（轨迹模式证据链）
  --output FILE         manifest 输出路径（默认 <capture-dir>/manifest.json）

运行配置 JSON（run config）字段:
  {
    "timing": {"expected_frames": [40,80,...], "primal_fixed_dt": 0.0166667},
    "render": {"resolution": [1280, 720]},        # 可选；提供则必须与 PPM 一致
    "scene":  {"env": {...}}, "build": {...}      # 原样快照进 manifest
  }

输出 manifest.json:
  status: VALID / INVALID。
  INVALID 条件（任一）: 期望帧缺失 / 多余未知帧 / PPM 头损坏 / 帧间尺寸不一致 /
  分辨率与配置冲突 / 配置缺 dt / 提供了 --log 但无 [FIXED_DT] 行 / 日志 dt
  与配置 dt 不一致。INVALID 时退出码 1（manifest 仍写出，供诊断）。

仅用 Python 标准库；无 numpy 依赖。
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import sys

PPM_NAME_RE = re.compile(r"^(?P<tag>[A-Za-z_][A-Za-z0-9_]*)_(?P<frame>\d+)\.ppm$")
FIXED_DT_RE = re.compile(r"\[FIXED_DT\]\s+frame\s+(\d+)\s+dt=([0-9.eE+-]+)")


def parse_ppm_header(path):
    """P6 PPM 头解析：返回 (width, height, maxval, header_bytes) 或 None。"""
    try:
        with open(path, "rb") as f:
            head = f.read(64)
        if not head.startswith(b"P6"):
            return None
        # 三个整数（允许 # 注释行不在头三 token 内的常规 P6）
        parts = []
        idx = 2
        while len(parts) < 3 and idx < len(head):
            while idx < len(head) and head[idx : idx + 1].isspace():
                idx += 1
            if idx < len(head) and head[idx : idx + 1] == b"#":
                while idx < len(head) and head[idx : idx + 1] != b"\n":
                    idx += 1
                continue
            start = idx
            while idx < len(head) and not head[idx : idx + 1].isspace():
                idx += 1
            if idx == start:
                return None
            parts.append(head[start:idx])
        if len(parts) < 3:
            return None
        try:
            w, h, maxval = (int(p) for p in parts[:3])
        except ValueError:
            return None
        if w <= 0 or h <= 0 or maxval != 255:
            return None
        return w, h
    except OSError:
        return None


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--capture-dir", required=True)
    ap.add_argument("--run-config", required=True)
    ap.add_argument("--log")
    ap.add_argument("--tag", help="only collect <tag>_<frame>.ppm; other tags "
                    "are recorded under ignored_tags (default: collect all tags; "
                    "multiple tags for one frame is INVALID)")
    ap.add_argument("--output")
    args = ap.parse_args()

    reasons = []
    try:
        with open(args.run_config, "r", encoding="utf-8") as f:
            run_config = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        print(f"run-config unreadable: {e}", file=sys.stderr)
        return 2

    # ---- 收集帧文件 ----
    frames = {}
    ignored_tags = set()
    for name in os.listdir(args.capture_dir):
        m = PPM_NAME_RE.match(name)
        if not m:
            continue
        if args.tag and m.group("tag") != args.tag:
            ignored_tags.add(m.group("tag"))
            continue
        frames.setdefault(int(m.group("frame")), []).append(name)
    for fl in frames.values():
        fl.sort()

    # ---- 逐帧摘要 ----
    frame_records = []
    dims = set()
    headers_ok = True
    for frame in sorted(frames):
        for name in frames[frame]:
            path = os.path.join(args.capture_dir, name)
            dims_hdr = parse_ppm_header(path)
            if dims_hdr is None:
                headers_ok = False
                reasons.append(f"frame {frame}: PPM header invalid ({name})")
                continue
            dims.add(dims_hdr)
            frame_records.append(
                {
                    "frame": frame,
                    "file": name,
                    "sha256": sha256_file(path),
                    "bytes": os.path.getsize(path),
                    "width": dims_hdr[0],
                    "height": dims_hdr[1],
                }
            )
    if len(dims) > 1:
        reasons.append(f"dimensions inconsistent across frames: {sorted(dims)}")

    # ---- 期望帧集核对 ----
    timing = run_config.get("timing", {})
    expected = timing.get("expected_frames")
    frames_complete = None
    if expected is not None:
        expected_set = set(int(x) for x in expected)
        found_set = set(frames.keys())
        missing = sorted(expected_set - found_set)
        extra = sorted(found_set - expected_set)
        frames_complete = not missing and not extra
        if missing:
            reasons.append(f"missing frames: {missing}")
        if extra:
            reasons.append(f"unexpected extra frames: {extra}")
        if any(len(v) > 1 for v in frames.values()):
            reasons.append("duplicate tag variants for same frame number")

    # ---- dt 记录与一致性 ----
    dt = timing.get("primal_fixed_dt")
    dt_recorded = isinstance(dt, (int, float)) and dt > 0
    if not dt_recorded:
        reasons.append("run-config timing.primal_fixed_dt missing or not > 0")
    dt_log_verified = False
    if args.log:
        try:
            with open(args.log, "r", errors="replace") as f:
                log_text = f.read()
        except OSError as e:
            reasons.append(f"--log unreadable: {e}")
            log_text = ""
        hits = FIXED_DT_RE.findall(log_text)
        if not hits:
            reasons.append("no [FIXED_DT] lines in log (dt injection unverified)")
        elif dt_recorded:
            logged = {float(v) for _, v in hits}
            if any(abs(v - dt) > 1e-6 for v in logged):
                reasons.append(
                    f"log dt values {sorted(logged)} != config dt {dt}"
                )
            else:
                dt_log_verified = True

    # ---- 分辨率一致性 ----
    resolution = run_config.get("render", {}).get("resolution")
    if resolution and len(dims) == 1:
        w, h = next(iter(dims))
        if [w, h] != [int(resolution[0]), int(resolution[1])]:
            reasons.append(
                f"PPM dims {w}x{h} != config resolution {resolution[0]}x{resolution[1]}"
            )

    status = "VALID" if not reasons else "INVALID"

    manifest = {
        "manifest_version": 1,
        "generated_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "generator": "scripts/generate_capture_manifest.py",
        "status": status,
        "invalid_reasons": reasons,
        "capture_dir": os.path.abspath(args.capture_dir),
        "frame_count": len(frame_records),
        "frames": frame_records,
        "ignored_tags": sorted(ignored_tags),
        "timing": {
            "dt": dt,
            "dt_source": "PRIMAL_FIXED_DT" if dt_recorded else None,
            "dt_log_verified": dt_log_verified,
        },
        "config": run_config,
        "checks": {
            "frames_complete": frames_complete,
            "ppm_headers_valid": headers_ok,
            "dimensions_consistent": len(dims) <= 1,
        },
        "build": run_config.get("build", {"build_git_sha": None, "provided": False}),
    }

    out = args.output or os.path.join(args.capture_dir, "manifest.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, sort_keys=False)
        f.write("\n")
    print(f"manifest: {out} status={status} frames={len(frame_records)}")
    for r in reasons:
        print(f"  INVALID: {r}", file=sys.stderr)
    return 0 if status == "VALID" else 1


if __name__ == "__main__":
    sys.exit(main())
