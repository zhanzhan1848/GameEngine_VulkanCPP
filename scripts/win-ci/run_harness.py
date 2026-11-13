#!/usr/bin/env python3
"""run_harness.py - TestRHISponzaShared M2-M6 regression matrix for Windows parity CI.

Runs the 18 official harness configurations (11 D3D12 legs + 7 Vulkan legs),
byte-compares every produced PPM/PGM frame against a runner-local baseline,
and drives the M6 strict verifier (scripts/m6_gi_verify.py, in-repo) over the
GI float exports.

Gate semantics (exit codes):
  0  all runs exit 0 AND every frame byte-identical to baseline AND all M6
     frame verifications PASS;
  1  any gate broken (run failed / frames differ / M6 verification failed);
  2  baseline directory missing entirely (message includes the exact seed
     command to fix it - never a silent skip);
  3  usage / input error (bad repo, exe, trajectory file...).

This file is deliberately ASCII-only so no code page can garble it.

Usage (Windows runner):
  python scripts/win-ci/run_harness.py ^
    --repo . --exe build-winparity\bin\Release\TestRHISponzaShared.exe ^
    --out ci-out\harness --baseline "%PRIMAL_CI_HARNESS_BASELINE%"

  Seed the baseline once on a known-good commit (r11 73d6956 or the merged
  dev tip) with the same command plus --seed (requires --force if the target
  dir already has content):
    python scripts/win-ci/run_harness.py ... --seed [--force]

Environment per run = the trajectory JSON scene.config.env block (platform
key dropped) + per-run overrides + RHI_TEST_PLATFORM + RHI_CAPTURE_DIR.
Debug opt-in dumps from the trajectory env (RHI_SHADOW_DUMP /
RHI_GBUFFER_DUMP) are stripped: they are one-shot debug files, not part of
the byte-stable frame set. Exports (giexp_*.json/bin) land in the capture dir
but are excluded from the byte gate; only *.ppm and *.pgm frames are gated.

Expected per-run frame counts (advisory only, from the official r11 run on
73d6956): total 276 across the 18 runs. A mismatch is printed as a warning;
the gate itself is byte-identity against the seeded baseline, so a count
difference only matters when it also changes the file set.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

# (name, platform, trajectory, env overrides)
# "full"  = the trajectory as committed (240 frames, resize era 120-130).
# "fixed" = first 30 frames (capture set clipped to 2 frames, resize era
#           auto-disabled because 120 >= totalFrames-1).
# m6 GI-off legs use the fixed-view trajectory with GI=0 and 119 frames so
# the capture set clips to {0,29,59,89} exactly like the official runs.
MATRIX = [
    ("m6-full-gi1",   "d3d12",  "m6_sponza_gi.json",
     {"RHI_GI_EXPORT_FRAMES": "0,30,60,84,89,119,131,156,180,210,216,239"}),
    ("m6-fixed-gi1",  "d3d12",  "m6_gi_fixed_view.json",
     {"RHI_GI_EXPORT_FRAMES": "0,44,89,119"}),
    ("m6-fixed-gi0",  "d3d12",  "m6_gi_fixed_view.json",
     {"RHI_TEST_FRAMES": "119", "RHI_TEST_GI": "0",
      "RHI_GI_EXPORT_FRAMES": "0,44,89,119"}),
    ("m2-full",       "d3d12",  "m2_sponza_shadow.json",    {}),
    ("m3-full",       "d3d12",  "m3_sponza_post.json",      {}),
    ("m4-full",       "d3d12",  "m4_sponza_deferred.json",  {}),
    ("m5-full",       "d3d12",  "m5_sponza_gpudriven.json", {}),
    ("m2-fixed",      "d3d12",  "m2_sponza_shadow.json",    {"RHI_TEST_FRAMES": "30"}),
    ("m3-fixed",      "d3d12",  "m3_sponza_post.json",      {"RHI_TEST_FRAMES": "30"}),
    ("m4-fixed",      "d3d12",  "m4_sponza_deferred.json",  {"RHI_TEST_FRAMES": "30"}),
    ("m5-fixed",      "d3d12",  "m5_sponza_gpudriven.json", {"RHI_TEST_FRAMES": "30"}),
    ("vk-m6-fixed-gi1", "vulkan", "m6_gi_fixed_view.json",
     {"RHI_GI_EXPORT_FRAMES": "0,44,89,119"}),
    ("vk-m6-fixed-gi0", "vulkan", "m6_gi_fixed_view.json",
     {"RHI_TEST_FRAMES": "119", "RHI_TEST_GI": "0",
      "RHI_GI_EXPORT_FRAMES": "0,44,89,119"}),
    ("vk-m2-fixed",   "vulkan", "m2_sponza_shadow.json",    {"RHI_TEST_FRAMES": "30"}),
    ("vk-m3-fixed",   "vulkan", "m3_sponza_post.json",      {"RHI_TEST_FRAMES": "30"}),
    ("vk-m4-fixed",   "vulkan", "m4_sponza_deferred.json",  {"RHI_TEST_FRAMES": "30"}),
    ("vk-m5-fixed",   "vulkan", "m5_sponza_gpudriven.json", {"RHI_TEST_FRAMES": "30"}),
    ("vk-m6-full-gi1", "vulkan", "m6_sponza_gi.json",
     {"RHI_GI_EXPORT_FRAMES": "0,30,60,84,89,119,131,156,180,210,216,239"}),
]

# Advisory expected frame counts (official r11 evidence, commit 73d6956).
EXPECTED_COUNTS = {
    "m6-full-gi1": 73, "m6-fixed-gi1": 29, "m6-fixed-gi0": 4,
    "m2-full": 12, "m3-full": 12, "m4-full": 12, "m5-full": 12,
    "m2-fixed": 2, "m3-fixed": 2, "m4-fixed": 2, "m5-fixed": 2,
    "vk-m6-fixed-gi1": 29, "vk-m6-fixed-gi0": 4,
    "vk-m2-fixed": 2, "vk-m3-fixed": 2, "vk-m4-fixed": 2, "vk-m5-fixed": 2,
    "vk-m6-full-gi1": 73,
}

M6_VERIFY_RUNS = ["m6-full-gi1", "m6-fixed-gi1",
                  "vk-m6-fixed-gi1", "vk-m6-full-gi1"]

DEBUG_DUMP_KEYS = {"RHI_SHADOW_DUMP", "RHI_GBUFFER_DUMP", "RHI_HDR_DUMP"}

FRAME_SUFFIXES = (".ppm", ".pgm")


def fail_usage(msg):
    sys.stderr.write("[harness] usage error: %s\n" % msg)
    sys.exit(2)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def frame_files(directory):
    return sorted(f for f in os.listdir(directory)
                  if f.lower().endswith(FRAME_SUFFIXES))


def trajectory_env(repo, traj_name):
    path = os.path.join(repo, "EngineTest", "trajectories", traj_name)
    if not os.path.isfile(path):
        fail_usage("trajectory not found: %s" % path)
    with open(path, "r", encoding="utf-8") as fh:
        data = json.load(fh)
    env = data.get("scene", {}).get("config", {}).get("env", {})
    return {k: str(v) for k, v in env.items()
            if k != "RHI_TEST_PLATFORM" and k not in DEBUG_DUMP_KEYS}


def run_one(name, platform, traj, overrides, repo, exe, out_root):
    env = dict(os.environ)
    env.update(trajectory_env(repo, traj))
    env.update(overrides)
    env["RHI_TEST_PLATFORM"] = platform
    run_dir = os.path.join(out_root, name)
    if os.path.isdir(run_dir):
        shutil.rmtree(run_dir)
    os.makedirs(run_dir)
    env["RHI_CAPTURE_DIR"] = run_dir
    # The harness reads assets relative to cwd; repo root is the anchor.
    stdout_path = os.path.join(out_root, name + ".stdout.log")
    stderr_path = os.path.join(out_root, name + ".stderr.log")
    rec = {"name": name, "platform": platform, "trajectory": traj,
           "start": time.strftime("%Y-%m-%d %H:%M:%S")}
    with open(stdout_path, "wb") as so, open(stderr_path, "wb") as se:
        try:
            proc = subprocess.run([exe], cwd=repo, env=env,
                                  stdout=so, stderr=se,
                                  timeout=1200)
            rec["exit_code"] = proc.returncode
        except subprocess.TimeoutExpired:
            rec["exit_code"] = "timeout-1200s"
    rec["end"] = time.strftime("%Y-%m-%d %H:%M:%S")
    rec["frames"] = frame_files(run_dir)
    rec["frames_written"] = len(rec["frames"])
    rec["frame_shas"] = {f: sha256_file(os.path.join(run_dir, f))
                         for f in rec["frames"]}
    print("[harness] %-18s exit=%s frames=%d" %
          (name, rec["exit_code"], rec["frames_written"]), flush=True)
    return rec


def compare_to_baseline(records, baseline):
    lines = []
    totals = {"total": 0, "same": 0, "differ": 0,
              "missing_old": 0, "missing_new": 0}
    for rec in records:
        base_dir = os.path.join(baseline, rec["name"])
        if not os.path.isdir(base_dir):
            rec["baseline_missing"] = True
            for f in rec["frames"]:
                totals["missing_old"] += 1
            lines.append("%s: BASELINE DIR MISSING %s" %
                         (rec["name"], base_dir))
            continue
        base_files = frame_files(base_dir)
        new_set, old_set = set(rec["frames"]), set(base_files)
        same = differ = 0
        for f in sorted(new_set & old_set):
            if sha256_file(os.path.join(base_dir, f)) == rec["frame_shas"][f]:
                same += 1
            else:
                differ += 1
        missing_old = len(new_set - old_set)   # produced now, absent in baseline
        missing_new = len(old_set - new_set)   # in baseline, not produced now
        rec["baseline"] = {"same": same, "differ": differ,
                           "missing_old": missing_old,
                           "missing_new": missing_new}
        totals["total"] += len(new_set)
        totals["same"] += same
        totals["differ"] += differ
        totals["missing_old"] += missing_old
        totals["missing_new"] += missing_new
        lines.append("%s: total=%d same=%d differ=%d missing_old=%d "
                     "missing_new=%d" % (rec["name"], len(new_set), same,
                                         differ, missing_old, missing_new))
    return lines, totals


def seed_baseline(records, baseline, force):
    if os.path.isdir(baseline) and os.listdir(baseline) and not force:
        fail_usage("baseline dir not empty: %s (use --force to overwrite "
                   "per-run subdirs)" % baseline)
    os.makedirs(baseline, exist_ok=True)
    for rec in records:
        dst = os.path.join(baseline, rec["name"])
        if os.path.isdir(dst):
            shutil.rmtree(dst)
        os.makedirs(dst)
        src = rec["run_dir_abs"]
        for f in rec["frames"]:
            shutil.copy2(os.path.join(src, f), os.path.join(dst, f))
    with open(os.path.join(baseline, "seed.json"), "w",
              encoding="utf-8") as fh:
        json.dump({"seeded": time.strftime("%Y-%m-%d %H:%M:%S"),
                   "records": [{k: rec[k] for k in
                                ("name", "exit_code", "frames_written")}
                               for rec in records]}, fh, indent=2)


def verify_m6(records, out_root, repo, m6_script):
    results = []
    ok_all = True
    for run_name in M6_VERIFY_RUNS:
        rec = next(r for r in records if r["name"] == run_name)
        run_dir = rec["run_dir_abs"]
        manifests = sorted(f for f in os.listdir(run_dir)
                           if re.fullmatch(r"giexp_f\d+\.json", f))
        for mf in manifests:
            frame = int(re.search(r"\d+", mf).group())
            vdir = os.path.join(out_root, "verify",
                                "%s-f%d" % (run_name, frame))
            os.makedirs(vdir, exist_ok=True)
            cmd = [sys.executable, m6_script, "frame",
                   "--on", run_dir, "--frame", str(frame), "--out", vdir]
            t0 = time.time()
            try:
                proc = subprocess.run(cmd, cwd=repo,
                                      capture_output=True, timeout=900)
                rc = proc.returncode
            except subprocess.TimeoutExpired:
                rc = "timeout-900s"
            elapsed = time.time() - t0
            ok = (rc == 0)
            ok_all = ok_all and ok
            # Keep the machine-readable report; PFM/PNG images are large and
            # reproducible - drop them to bound runner disk usage.
            for f in os.listdir(vdir):
                if f.lower().endswith((".pfm", ".png")):
                    os.remove(os.path.join(vdir, f))
            results.append({"run": run_name, "frame": frame,
                            "exit": rc, "pass": ok,
                            "elapsed_s": round(elapsed, 1)})
            print("[m6-verify] %s-f%d exit=%s (%.1fs)" %
                  (run_name, frame, rc, elapsed), flush=True)
    return results, ok_all


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--baseline", default=os.environ.get(
        "PRIMAL_CI_HARNESS_BASELINE", ""))
    ap.add_argument("--seed", action="store_true")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--skip-m6-verify", action="store_true")
    ap.add_argument("--m6-script", default=None)
    args = ap.parse_args()

    repo = os.path.abspath(args.repo)
    exe = os.path.abspath(args.exe)
    out_root = os.path.abspath(args.out)
    if not os.path.isdir(repo):
        fail_usage("repo not found: %s" % repo)
    if not os.path.isfile(exe):
        fail_usage("TestRHISponzaShared.exe not found: %s" % exe)
    m6_script = args.m6_script or os.path.join(
        repo, "scripts", "m6_gi_verify.py")
    if not args.skip_m6_verify and not os.path.isfile(m6_script):
        fail_usage("m6_gi_verify.py not found: %s" % m6_script)

    os.makedirs(out_root, exist_ok=True)
    print("[harness] RUN-ALL START %s" %
          time.strftime("%Y-%m-%d %H:%M:%S"), flush=True)

    records = []
    gate = {"runs_ok": True, "baseline_ok": True, "m6_ok": True}
    for name, platform, traj, overrides in MATRIX:
        rec = run_one(name, platform, traj, overrides, repo, exe, out_root)
        rec["run_dir_abs"] = os.path.join(out_root, name)
        records.append(rec)
        if rec["exit_code"] != 0:
            gate["runs_ok"] = False

    total_frames = sum(r["frames_written"] for r in records)
    print("[harness] total frames written: %d (official r11 reference: 276)"
          % total_frames, flush=True)
    counts_note = []
    for rec in records:
        exp = EXPECTED_COUNTS.get(rec["name"])
        if exp is not None and exp != rec["frames_written"]:
            counts_note.append("%s: got %d, official r11 %d" %
                               (rec["name"], rec["frames_written"], exp))
    if counts_note:
        print("[harness] NOTE frame-count differs from official r11 "
              "evidence (advisory; the gate is byte-identity to the "
              "seeded baseline):", flush=True)
        for line in counts_note:
            print("  " + line, flush=True)

    with open(os.path.join(out_root, "runs.json"), "w",
              encoding="utf-8") as fh:
        json.dump(records, fh, indent=2)

    if args.seed:
        if not args.baseline:
            fail_usage("--seed needs --baseline or PRIMAL_CI_HARNESS_BASELINE")
        seed_baseline(records, args.baseline, args.force)
        print("[harness] baseline seeded at %s (%d frames total)" %
              (args.baseline, total_frames), flush=True)
        print("[harness] seed mode: byte gate skipped for this invocation",
              flush=True)
        with open(os.path.join(out_root, "harness_summary.md"), "w",
                  encoding="utf-8") as fh:
            fh.write("### Harness M2-M6 - SEED RUN (no byte gate)\n\n")
            fh.write("Total frames: %d written to baseline %s.\n" %
                     (total_frames, args.baseline))
        sys.exit(0)

    if not args.baseline or not os.path.isdir(args.baseline):
        sys.stderr.write(
            "[harness] GATE FAIL - PRIMAL_CI_HARNESS_BASELINE is not set or "
            "does not exist:\n  %r\n"
            "[harness] seed it on a known-good commit (r11 73d6956 or "
            "the merged dev tip):\n"
            "  set PRIMAL_CI_HARNESS_BASELINE=C:\\primal-ci\\harness-baseline\n"
            "  git checkout <known-good-sha>\n"
            "  python scripts\\win-ci\\run_harness.py --repo . --exe "
            "<build>\\bin\\Release\\TestRHISponzaShared.exe --out "
            "ci-out\\harness --seed\n"
            "[harness] after seeding, re-run this CI job; the gate then "
            "requires every frame byte-identical to the baseline.\n")
        sys.exit(2)

    if True:
        lines, totals = compare_to_baseline(records, args.baseline)
        report = ("vs baseline %s total=%d same=%d differ=%d "
                  "missing_old=%d missing_new=%d" %
                  (args.baseline, totals["total"], totals["same"],
                   totals["differ"], totals["missing_old"],
                   totals["missing_new"]))
        with open(os.path.join(out_root, "frame-sha-vs-baseline.txt"), "w",
                  encoding="utf-8") as fh:
            fh.write(report + "\n" + "\n".join(lines) + "\n")
        print("[harness] " + report, flush=True)
        for line in lines:
            print("  " + line, flush=True)
        if (totals["differ"] or totals["missing_old"]
                or totals["missing_new"]):
            gate["baseline_ok"] = False

    m6_results = []
    if not args.skip_m6_verify:
        m6_results, m6_ok = verify_m6(records, out_root, repo, m6_script)
        if len(m6_results) < 20:
            # Vacuous pass is not a pass: the official r11 gate verified 20
            # export frames (d3d12 full 12 + d3d12 fixed 4 + vk fixed 4).
            print("[m6-verify] GATE FAIL - only %d export frames found, "
                  "expected >= 20 (d3d12 full 12 + d3d12 fixed 4 + vk fixed "
                  "4 minimum)" % len(m6_results), flush=True)
            m6_ok = False
        gate["m6_ok"] = m6_ok
        passed = sum(1 for r in m6_results if r["pass"])
        print("[m6-verify] %d/%d frame verifications PASS (official r11 "
              "gate was 20/20; this driver verifies every exported frame, "
              "a superset)" % (passed, len(m6_results)), flush=True)
        with open(os.path.join(out_root, "m6-verify-summary.txt"), "w",
                  encoding="utf-8") as fh:
            for r in m6_results:
                fh.write("exit=%s name=%s-f%d time=%.1fs %s\n" %
                         (r["exit"], r["run"], r["frame"], r["elapsed_s"],
                          "PASS" if r["pass"] else "FAIL"))

    # Markdown summary for $GITHUB_STEP_SUMMARY (appended by the workflow).
    with open(os.path.join(out_root, "harness_summary.md"), "w",
              encoding="utf-8") as fh:
        fh.write("### Harness M2-M6 (TestRHISponzaShared) - 18 runs\n\n")
        fh.write("| run | platform | exit | frames | baseline (same/differ/"
                 "miss) |\n|---|---|---|---|---|\n")
        for rec in records:
            b = rec.get("baseline")
            if rec.get("baseline_missing"):
                bs = "BASELINE MISSING"
            elif b:
                bs = "%d/%d/%d+%d" % (b["same"], b["differ"],
                                      b["missing_old"], b["missing_new"])
            else:
                bs = "n/a (seed mode)"
            fh.write("| %s | %s | %s | %d | %s |\n" %
                     (rec["name"], rec["platform"], rec["exit_code"],
                      rec["frames_written"], bs))
        fh.write("\nTotal frames: %d (official r11: 276). " % total_frames)
        fh.write("M6 strict: %d/%d PASS.\n" %
                 (sum(1 for r in m6_results if r["pass"]), len(m6_results)))

    print("[harness] gate: runs_ok=%s baseline_ok=%s m6_ok=%s" %
          (gate["runs_ok"], gate["baseline_ok"], gate["m6_ok"]), flush=True)
    if all(gate.values()):
        print("[harness] RUN-ALL ALLDONE (gates green)", flush=True)
        sys.exit(0)
    sys.exit(1)


if __name__ == "__main__":
    main()
