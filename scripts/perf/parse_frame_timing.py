#!/usr/bin/env python3
"""Parse TestVulkanSponzaRenderGraph timing logs into BVH perf stats.

Sources understood (both stdlib-only, no gating — reports are informational):
  - `[FRAME_TIMING] ...` per-frame lines (PRIMAL_FRAME_TIMING=1), fields are a
    whitespace-separated `key=value` stream; empty values (timestamp not read
    that frame) are treated as missing samples.
  - `[ENV_SAH_INTERLEAVED] sample ...` per-sample lines (ENV_SAH_INTERLEAVED=1),
    grouped by `mode=` for same-process A/B/A/B comparisons. Rejection lines
    (`rejected_provenance` etc.) are counted but never treated as samples.

Usage:
  parse_frame_timing.py --log dyn-gi=run/dyn-gi.log --log dyn-gi-sah=run/sah.log \
      [--warm 60] [--baseline dyn-gi] [--interleaved] \
      [--out-json report.json] [--out-md report.md] [--git-sha SHA] [--binary-sha SHA]
  parse_frame_timing.py --selftest
"""

import argparse
import datetime
import json
import math
import os
import re
import sys

# Metrics extracted from [FRAME_TIMING] lines, in report order.
FRAME_METRICS = [
    "gpu_trace_ms",
    "gpu_resolve_ms",
    "gpu_ddgi_gather_ms",
    "gpu_spgi_ms",
    "gpu_sc_ms",
    "gpu_pass_total_ms",
    "interval_ms",
    "render_cpu_ms",
]

# Metrics extracted from [ENV_SAH_INTERLEAVED] sample lines.
INTERLEAVED_METRICS = ["trace_ms", "resolve_ms", "spgi_ms", "total_pass_ms"]

CLEAN_EXIT_RE = re.compile(r"SPONZA_MAX_FRAMES=(\d+) reached")


def _percentile(sorted_vals, q):
    """Linear-interpolated percentile on a pre-sorted list; None for empty."""
    if not sorted_vals:
        return None
    if len(sorted_vals) == 1:
        return sorted_vals[0]
    pos = (len(sorted_vals) - 1) * q
    lo = int(math.floor(pos))
    hi = min(lo + 1, len(sorted_vals) - 1)
    frac = pos - lo
    return sorted_vals[lo] * (1.0 - frac) + sorted_vals[hi] * frac


def _kv_pairs(rest):
    """Split a whitespace-separated key=value stream; values may be empty."""
    out = {}
    for token in rest.split():
        key, sep, value = token.partition("=")
        if not sep:
            continue
        out[key] = value
    return out


def _to_float(value):
    if value is None or value == "":
        return None
    try:
        return float(value)
    except ValueError:
        return None


def parse_frame_timing_log(path):
    """Return {frames_seen, frames_used, begin_failed, metrics: {name: [values]}}."""
    frames_seen = set()
    begin_failed = 0
    metric_values = {name: [] for name in FRAME_METRICS}
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            line = raw.strip()
            if "[FRAME_TIMING]" not in line:
                continue
            rest = line.split("[FRAME_TIMING]", 1)[1].strip()
            if rest.startswith("begin_failed"):
                begin_failed += 1
                continue
            fields = _kv_pairs(rest)
            frame = fields.get("frame")
            if frame is None or frame == "":
                continue
            try:
                frames_seen.add(int(frame))
            except ValueError:
                continue
            for name in FRAME_METRICS:
                value = _to_float(fields.get(name))
                if value is not None:
                    metric_values[name].append((int(frame), value))
    return {
        "frames_seen": len(frames_seen),
        "begin_failed": begin_failed,
        "metric_values": metric_values,
    }


def _apply_warm(metric_values, warm):
    """Drop samples from frames < warm; returns {name: sorted-values}."""
    trimmed = {}
    for name, samples in metric_values.items():
        kept = sorted(value for frame, value in samples if frame >= warm)
        trimmed[name] = kept
    return trimmed


def _stats(values):
    if not values:
        return {"n": 0, "mean": None, "p50": None, "p95": None, "max": None}
    return {
        "n": len(values),
        "mean": sum(values) / len(values),
        "p50": _percentile(values, 0.50),
        "p95": _percentile(values, 0.95),
        "max": max(values),
    }


def parse_interleaved_log(path):
    """Return {rejections: N, modes: {mode: {metric: [(block, value)]}}}."""
    rejections = 0
    modes = {}
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            line = raw.strip()
            if "[ENV_SAH_INTERLEAVED]" not in line:
                continue
            rest = line.split("[ENV_SAH_INTERLEAVED]", 1)[1].strip()
            if rest.startswith("sample"):
                pass
            elif rest.startswith("rejected"):
                rejections += 1
                continue
            else:
                # block header / warmup announcement — neither sample nor rejection
                continue
            fields = _kv_pairs(rest)
            mode = fields.get("mode")
            block = fields.get("block")
            if not mode:
                rejections += 1
                continue
            try:
                block_num = int(block) if block not in (None, "") else -1
            except ValueError:
                block_num = -1
            bucket = modes.setdefault(mode, {name: [] for name in INTERLEAVED_METRICS})
            for name in INTERLEAVED_METRICS:
                value = _to_float(fields.get(name))
                if value is not None:
                    bucket[name].append((block_num, value))
    return {"rejections": rejections, "modes": modes}


def summarize_log(path, warm, interleaved=False):
    """Parse one run log into the report fragment for a group."""
    summary = {}
    parsed = parse_frame_timing_log(path)
    trimmed = _apply_warm(parsed["metric_values"], warm)
    summary["frames_seen"] = parsed["frames_seen"]
    summary["begin_failed"] = parsed["begin_failed"]
    summary["metrics"] = {name: _stats(values) for name, values in trimmed.items()}
    clean_exit = None
    max_frames = None
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            match = CLEAN_EXIT_RE.search(raw)
            if match:
                clean_exit = True
                max_frames = int(match.group(1))
                break
        else:
            clean_exit = False
    summary["clean_exit"] = clean_exit
    summary["max_frames"] = max_frames
    if interleaved:
        inter = parse_interleaved_log(path)
        summary["interleaved_rejections"] = inter["rejections"]
        summary["interleaved_modes"] = {
            mode: {name: _stats([v for _, v in vals]) for name, vals in per_metric.items()}
            for mode, per_metric in inter["modes"].items()
        }
    return summary


def _fmt(value, digits=3):
    if value is None:
        return "-"
    return f"{value:.{digits}f}"


def _pct_delta(candidate, baseline):
    if candidate is None or baseline is None or baseline == 0:
        return None
    return (candidate - baseline) / baseline * 100.0


DELTA_METRICS = ["gpu_trace_ms", "gpu_resolve_ms", "gpu_ddgi_gather_ms", "gpu_pass_total_ms", "interval_ms"]


def build_deltas(groups, baseline):
    """p50/mean percentage deltas of every non-baseline group vs baseline."""
    base = groups.get(baseline)
    if not base:
        return {}
    deltas = {}
    for name, summary in groups.items():
        if name == baseline:
            continue
        per_metric = {}
        for metric in DELTA_METRICS:
            cand = summary["metrics"].get(metric, {})
            base_stats = base["metrics"].get(metric, {})
            per_metric[metric] = {
                "p50_pct": _pct_delta(cand.get("p50"), base_stats.get("p50")),
                "mean_pct": _pct_delta(cand.get("mean"), base_stats.get("mean")),
            }
        deltas[name] = {"baseline": baseline, "metrics": per_metric}
    return deltas


def build_interleaved_deltas(interleaved_modes):
    """Deltas of every mode vs the mode with the fewest... no: vs literal 'on'.

    The interleaved protocol labels blocks; the un-candidate mode is whatever
    the harness logged as the non-SAH side (typically `on` or `baseline`).
    Prefer those names, else the first-seen mode.
    """
    if not interleaved_modes:
        return {}
    base_name = None
    for candidate in ("on", "baseline", "off"):
        if candidate in interleaved_modes:
            base_name = candidate
            break
    if base_name is None:
        base_name = sorted(interleaved_modes.keys())[0]
    base = interleaved_modes[base_name]
    deltas = {}
    for mode, per_metric in interleaved_modes.items():
        if mode == base_name:
            continue
        deltas[mode] = {
            "baseline": base_name,
            "metrics": {
                name: {
                    "p50_pct": _pct_delta(stats.get("p50"), base.get(name, {}).get("p50")),
                    "mean_pct": _pct_delta(stats.get("mean"), base.get(name, {}).get("mean")),
                }
                for name, stats in per_metric.items()
            },
        }
    return deltas


def render_markdown(meta, groups, deltas, interleaved=None, interleaved_deltas=None):
    lines = []
    lines.append("# BVH perf — frame timing report")
    lines.append("")
    lines.append(f"- generated (UTC): {meta.get('generated_utc', '-')}")
    lines.append(f"- warm trim: first {meta.get('warm_frames', '-')} frames")
    if meta.get("git_sha"):
        lines.append(f"- git SHA: `{meta['git_sha']}`")
    if meta.get("binary_sha"):
        lines.append(f"- binary SHA256: `{meta['binary_sha']}`")
    lines.append("")
    lines.append("## Group stats (ms)")
    lines.append("")
    header = "| group | n | trace p50 | trace p95 | resolve p50 | resolve p95 | gather p50 | pass_total p50 | pass_total p95 | interval p50 | clean exit |"
    lines.append(header)
    lines.append("|---|---|---|---|---|---|---|---|---|---|---|")
    for name in groups:
        g = groups[name]
        m = g["metrics"]
        trace = m.get("gpu_trace_ms", {})
        resolve = m.get("gpu_resolve_ms", {})
        gather = m.get("gpu_ddgi_gather_ms", {})
        total = m.get("gpu_pass_total_ms", {})
        interval = m.get("interval_ms", {})
        n = max((stats.get("n") or 0) for stats in (trace, resolve, gather, total, interval))
        lines.append(
            f"| {name} | {n} | {_fmt(trace.get('p50'))} | {_fmt(trace.get('p95'))} "
            f"| {_fmt(resolve.get('p50'))} | {_fmt(resolve.get('p95'))} "
            f"| {_fmt(gather.get('p50'))} | {_fmt(total.get('p50'))} | {_fmt(total.get('p95'))} "
            f"| {_fmt(interval.get('p50'))} | {'✅' if g.get('clean_exit') else '❌'} |"
        )
    if deltas:
        lines.append("")
        base_name = next(iter(deltas.values()))["baseline"] if deltas else None
        lines.append(f"## Deltas vs `{base_name}` (negative = faster)")
        lines.append("")
        lines.append("| group | trace Δp50 | resolve Δp50 | gather Δp50 | pass_total Δp50 | interval Δp50 |")
        lines.append("|---|---|---|---|---|---|")
        for name, d in deltas.items():
            cells = []
            for metric in DELTA_METRICS:
                value = d["metrics"][metric]["p50_pct"]
                cells.append(f"{_fmt(value)}%" if value is not None else "-")
            lines.append(f"| {name} | " + " | ".join(cells) + " |")
    if interleaved is not None:
        lines.append("")
        lines.append("## Interleaved modes (same-process A/B)")
        lines.append("")
        lines.append("| mode | n(trace) | trace p50 | trace p95 | resolve p50 | total_pass p50 |")
        lines.append("|---|---|---|---|---|---|")
        for mode, per_metric in interleaved.items():
            trace = per_metric.get("trace_ms", {})
            resolve = per_metric.get("resolve_ms", {})
            total = per_metric.get("total_pass_ms", {})
            lines.append(
                f"| {mode} | {trace.get('n', 0)} | {_fmt(trace.get('p50'))} | {_fmt(trace.get('p95'))} "
                f"| {_fmt(resolve.get('p50'))} | {_fmt(total.get('p50'))} |"
            )
        if interleaved_deltas:
            lines.append("")
            lines.append("### Interleaved deltas (p50)")
            lines.append("")
            for mode, d in interleaved_deltas.items():
                trace = d["metrics"].get("trace_ms", {}).get("p50_pct")
                resolve = d["metrics"].get("resolve_ms", {}).get("p50_pct")
                lines.append(f"- `{mode}` vs `{d['baseline']}`: trace {_fmt(trace)}%, resolve {_fmt(resolve)}%")
    return "\n".join(lines) + "\n"


def run_selftest():
    """Feed a synthetic log through the parsers and assert the numbers."""
    frame_lines = [
        "[FRAME_TIMING] frame=0 interval_ms=16.7 gpu_trace_ms=1.0 gpu_resolve_ms=2.0 gpu_pass_total_ms=5.0",
        "[FRAME_TIMING] frame=1 interval_ms=16.7 gpu_trace_ms=2.0 gpu_resolve_ms=2.0 gpu_pass_total_ms=6.0",
        "[FRAME_TIMING] begin_failed frame=2",
        "[FRAME_TIMING] frame=2 interval_ms=16.7 gpu_trace_ms=10 gpu_resolve_ms= gpu_pass_total_ms=7.0",
        "[FRAME_TIMING] frame=3 interval_ms=16.7 gpu_trace_ms=20 gpu_resolve_ms=8 gpu_pass_total_ms=7.0",
        "[FRAME_TIMING] frame=4 interval_ms=16.7 gpu_trace_ms=30 gpu_resolve_ms=8 gpu_pass_total_ms=7.0",
        "[FRAME_TIMING] frame=5 interval_ms=16.7 gpu_trace_ms=40 gpu_resolve_ms=8 gpu_pass_total_ms=7.0",
        "[SponzaRun] SPONZA_MAX_FRAMES=6 reached — clean exit at frame 6",
    ]
    inter_lines = [
        "[ENV_SAH_INTERLEAVED] block=0 mode=on warmup=1",
        "[ENV_SAH_INTERLEAVED] sample block=0 offset=0 mode=on trace_ms=10 resolve_ms=4 total_pass_ms=15",
        "[ENV_SAH_INTERLEAVED] sample block=0 offset=1 mode=on trace_ms=12 resolve_ms=4 total_pass_ms=17",
        "[ENV_SAH_INTERLEAVED] rejected_provenance frame=9",
        "[ENV_SAH_INTERLEAVED] sample block=1 offset=0 mode=sah trace_ms=5 resolve_ms=4 total_pass_ms=10",
        "[ENV_SAH_INTERLEAVED] sample block=1 offset=1 mode=sah trace_ms=7 resolve_ms=4 total_pass_ms=12",
    ]

    class _FakeArg:
        pass

    # Feed the frame path through the real parser by writing a temp file.
    import tempfile

    checks = 0

    def check(label, got, want, tol=1e-6):
        nonlocal checks
        assert got is not None, f"{label}: got None, want {want}"
        assert math.isclose(got, want, rel_tol=0, abs_tol=tol), f"{label}: got {got}, want {want}"
        checks += 1

    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "frame.log")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write("\n".join(frame_lines) + "\n")
        summary = summarize_log(path, warm=2)
        assert summary["frames_seen"] == 6, summary["frames_seen"]
        assert summary["begin_failed"] == 1, summary["begin_failed"]
        assert summary["clean_exit"] is True and summary["max_frames"] == 6
        check("trace n", summary["metrics"]["gpu_trace_ms"]["n"], 4)
        check("trace mean", summary["metrics"]["gpu_trace_ms"]["mean"], 25.0)
        check("trace p50", summary["metrics"]["gpu_trace_ms"]["p50"], 25.0)
        # linear interpolation: 10 + 0.95*3*(40-10)/3 = 38.5
        check("trace p95", summary["metrics"]["gpu_trace_ms"]["p95"], 38.5)
        check("trace max", summary["metrics"]["gpu_trace_ms"]["max"], 40.0)
        # resolve was empty on frame 2 → only 3 samples survive warm trim.
        check("resolve n", summary["metrics"]["gpu_resolve_ms"]["n"], 3)
        check("resolve mean", summary["metrics"]["gpu_resolve_ms"]["mean"], 8.0)
        check("interval p50", summary["metrics"]["interval_ms"]["p50"], 16.7)

        inter_path = os.path.join(tmp, "inter.log")
        with open(inter_path, "w", encoding="utf-8") as handle:
            handle.write("\n".join(inter_lines) + "\n")
        inter = parse_interleaved_log(inter_path)
        assert inter["rejections"] == 1, inter["rejections"]
        assert set(inter["modes"].keys()) == {"on", "sah"}, inter["modes"].keys()
        check("inter on trace n", len(inter["modes"]["on"]["trace_ms"]), 2)
        check("inter sah trace p50", _stats([v for _, v in inter["modes"]["sah"]["trace_ms"]])["p50"], 6.0)

        modes_stats = {
            mode: {name: _stats([v for _, v in vals]) for name, vals in per_metric.items()}
            for mode, per_metric in inter["modes"].items()
        }
        deltas = build_interleaved_deltas(modes_stats)
        # on p50 = 11 ([10,12] interp), sah p50 = 6 → (6-11)/11
        check("inter delta trace", deltas["sah"]["metrics"]["trace_ms"]["p50_pct"], -45.45454545454545)

        # End-to-end summarize + markdown on the interleaved log.
        full = summarize_log(inter_path, warm=0, interleaved=True)
        md = render_markdown({"generated_utc": "test"}, {"g": full}, {}, full["interleaved_modes"], None)
        assert "| g |" in md
        checks += 1

    print(f"selftest: PASS ({checks} checks)")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--log", action="append", default=[],
                        metavar="NAME=PATH", help="group log; bare PATH uses the file stem")
    parser.add_argument("--warm", type=int, default=60, help="drop frames before this index (default 60)")
    parser.add_argument("--baseline", default=None, help="delta baseline group (default: dyn-gi if present, else first)")
    parser.add_argument("--interleaved", action="store_true",
                        help="also parse [ENV_SAH_INTERLEAVED] samples from each log")
    parser.add_argument("--out-json", default=None)
    parser.add_argument("--out-md", default=None)
    parser.add_argument("--git-sha", default=None)
    parser.add_argument("--binary-sha", default=None)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)

    if args.selftest:
        return run_selftest()

    if not args.log:
        parser.error("no --log given (or use --selftest)")
    sources = {}
    for spec in args.log:
        if "=" in spec:
            name, path = spec.split("=", 1)
        else:
            name = os.path.splitext(os.path.basename(spec))[0]
            path = spec
        if not os.path.isfile(path):
            parser.error(f"log not found: {path}")
        sources[name] = path

    groups = {}
    for name, path in sources.items():
        groups[name] = summarize_log(path, args.warm, interleaved=args.interleaved)

    baseline = args.baseline
    if baseline is None:
        baseline = "dyn-gi" if "dyn-gi" in groups else sorted(groups.keys())[0]
    deltas = build_deltas(groups, baseline)

    interleaved_modes = None
    interleaved_deltas = None
    if args.interleaved:
        # Merge every log's modes (nightly runs one log per group; interleaved
        # usually lives in a single dedicated log, but merging is harmless).
        interleaved_modes = {}
        for summary in groups.values():
            for mode, stats in summary.get("interleaved_modes", {}).items():
                interleaved_modes.setdefault(mode, {})
                for metric, s in stats.items():
                    merged = interleaved_modes[mode].get(metric)
                    if merged is None or (s.get("n") or 0) > (merged.get("n") or 0):
                        interleaved_modes[mode][metric] = s
        interleaved_deltas = build_interleaved_deltas(interleaved_modes)

    meta = {
        "generated_utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "warm_frames": args.warm,
        "baseline": baseline,
        "git_sha": args.git_sha,
        "binary_sha": args.binary_sha,
        "sources": sources,
    }

    report = {
        "meta": meta,
        "groups": groups,
        "deltas": deltas,
    }
    if args.interleaved:
        report["interleaved_modes"] = interleaved_modes
        report["interleaved_deltas"] = interleaved_deltas

    markdown = render_markdown(meta, groups, deltas, interleaved_modes, interleaved_deltas)

    if args.out_json:
        with open(args.out_json, "w", encoding="utf-8") as handle:
            json.dump(report, handle, indent=2, ensure_ascii=False)
            handle.write("\n")
    if args.out_md:
        with open(args.out_md, "w", encoding="utf-8") as handle:
            handle.write(markdown)
    if not args.out_md:
        sys.stdout.write(markdown)

    # Report-only: missing clean-exit anchors are flagged in the report but do
    # not change the exit code here; the caller decides on harness exit codes.
    missing = [name for name, g in groups.items() if not g.get("clean_exit")]
    if missing:
        sys.stderr.write(f"warning: no clean-exit anchor in: {', '.join(missing)}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
