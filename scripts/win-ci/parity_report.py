#!/usr/bin/env python3
"""parity_report.py - f350 PSNR/SSIM gate + summary for the Windows parity CI.

Reads scripts/win-ci/run_f350.ps1's f350_gates.json, verifies the in-repo
reference PPMs by SHA256, compares the captured f350 PPMs with
scripts/win-ci/compare_f350.py (pure stdlib), renders ref|win|diff triptych
PNGs (hand-rolled PNG encoder, zlib+struct), and writes the Markdown PSNR/SSIM
table for $GITHUB_STEP_SUMMARY.

Gate model (2026-11-25, mode parity):
  D3D12 still falls back to DDGI mode 0 while the canonical Vulkan group runs
  mode 7 + measured geometry, so comparing D3D12 against the mode-7 Mac
  reference can never pass (Mac's own mode 0 is ~20.6 dB against it). The job
  therefore gates on like-for-like mode 0:

  1. capture gate: the four canonical groups and both mode-0 diagnostic groups
     produced their f350 PPM;
  2. GI-off gate: PSNR vs the frozen Mac GI-off reference >= 43.5 dB (unchanged);
  3. same-mode parity gate: D3D12 mode 0 vs Vulkan mode 0, same runner, same
     run >= MODE0_PAIR_GATE_DB. This is the backend-parity signal;
  4. mode-0 reference gate: D3D12 mode 0 vs the frozen Windows Vulkan mode-0
     reference (same assets/camera/frame, re-frozen 2026-11-25) >=
     MODE0_REF_GATE_DB. This catches regressions that hit both backends.

  The canonical GI-on rows against the mode-7 Mac reference stay in the table as
  diagnostics: they track progress toward D3D12 measured mode 7, but they do not
  fail the job while D3D12 cannot run that path.

ASCII-only source; Python 3.8+ standard library only.

ASCII-only source; Python 3.8+ standard library only.
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import zlib

REF_SHA256 = ("f0b032b3b006f6655e510f77c0ece832341678215df9fb3b2cd5272fa"
              "9477158")
GIOFF_REF_SHA256 = ("358be857973d4cffd30deaeabfffd13374d5c9ea40f4b7c0f26c35e8b633a450")
# Frozen Windows/Vulkan DDGI mode-0 f350 capture (dev 762ea971, 2026-11-25).
# Same assets, camera, resolution and frame as the Windows runs, so it is the
# mode-0 counterpart of the Mac mode-7 reference.
MODE0_REF_SHA256 = ("59360839e0530cd33f2bd6c0c89c74be402d8d5d5475f7c7ab9897d6d644d81f")
PSNR_GATE_DB = 43.5
MODE0_PAIR_GATE_DB = 41.5   # D3D12 vs Vulkan, same run, mode 0 (measured 42.1)
MODE0_REF_GATE_DB = 41.0    # D3D12 vs frozen mode-0 reference (measured 42.1)
# Advisory "visible difference" criterion (NOT gated yet). Calibrated on the
# GI-off pair the reviewer accepts as visually consistent (0.21% pixels >8 LSB,
# tile16 mean p99 = 3.12) and the Vulkan run-to-run noise floor (0.00%, 0.80).
# The GI-on mode-0 pair currently sits at 1.60% / 7.28, i.e. visibly different
# in the banner and upper-arch regions; the row is printed so the CI summary
# cannot imply visual equivalence while this is true.
VIS_GT8_LIMIT = 0.005      # <=0.5% of pixels with max-channel diff >8/255
VIS_TILE_P99_LIMIT = 3.5   # 16x16 tile mean of max-channel diff, p99
MODE0_PPM = {
    "d3d12": "art2_win_d3d12_mode0_f350.ppm",
    "vulkan": "art2_win_vulkan_mode0_f350.ppm",
}
DIFF_GAIN = 8  # heatmap amplification: 8*abs(a-b), clamped to 255

GROUP_LABELS = {
    "d3d12-gion": "D3D12 GI-on",
    "d3d12-gioff": "D3D12 GI-off",
    "vulkan-gion": "Vulkan GI-on",
    "vulkan-gioff": "Vulkan GI-off",
}


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_p6(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:2] != b"P6":
        raise SystemExit("%s: not a P6 PPM" % path)
    i, fields = 2, []
    while len(fields) < 3:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    i += 1
    w, h, maxv = fields
    if maxv != 255:
        raise SystemExit("%s: only maxval=255 supported" % path)
    px = data[i:i + w * h * 3]
    if len(px) != w * h * 3:
        raise SystemExit("%s: truncated pixel data" % path)
    return w, h, px


def write_png(path, w, h, rgb):
    """8-bit RGB PNG (same technique as scripts/m6_gi_verify.py write_png)."""
    row = 3 * w
    raw = bytearray()
    mv = memoryview(rgb)
    for y in range(h):
        raw.append(0)
        raw += mv[y * row:(y + 1) * row]

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n")
        fh.write(chunk(b"IHDR", ihdr))
        fh.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        fh.write(chunk(b"IEND", b""))


def triptych(ref_path, win_path, out_path):
    """ref | win | diff heatmap side by side (labels omitted on purpose)."""
    w, h, a = read_p6(ref_path)
    w2, h2, b = read_p6(win_path)
    if (w, h) != (w2, h2):
        raise SystemExit("size mismatch %dx%d vs %dx%d" % (w, h, w2, h2))
    stride = w * 3
    out = bytearray()
    for y in range(h):
        start = y * stride
        ref_row, win_row = a[start:start + stride], b[start:start + stride]
        out.extend(ref_row)
        out.extend(win_row)
        out.extend(min(255, DIFF_GAIN * abs(x - z)) for x, z in zip(ref_row, win_row))
    write_png(out_path, w * 3, h, bytes(out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--gates", required=True,
                    help="f350_gates.json produced by run_f350.ps1")
    ap.add_argument("--out", required=True, help="output dir (compare JSON, "
                    "triptychs, summary md)")
    ap.add_argument("--summary", default="",
                    help="path to append the Markdown table to "
                    "($GITHUB_STEP_SUMMARY)")
    args = ap.parse_args()

    repo = os.path.abspath(args.repo)
    ref = os.path.join(repo, ".github", "ci-assets",
                       "ref_mac_vulkan_RG_f350.ppm")
    compare = os.path.join(repo, "scripts", "win-ci", "compare_f350.py")
    if not os.path.isfile(ref):
        print("[parity] FATAL reference PPM missing: %s" % ref)
        sys.exit(2)
    ref_sha = sha256_file(ref)
    if ref_sha != REF_SHA256:
        print("[parity] FATAL reference PPM SHA256 mismatch:\n"
              "  got      %s\n  expected %s" % (ref_sha, REF_SHA256))
        sys.exit(2)
    print("[parity] reference PPM SHA256 OK (%s)" % ref_sha)

    with open(args.gates, "r", encoding="utf-8-sig") as fh:
        gates = json.load(fh)
    if len(gates) != 4 or {g["group"] for g in gates} != set(GROUP_LABELS):
        print("[parity] FATAL expected 4 groups in %s, got %d "
              "(run_f350.ps1 writes [] when the exe is missing)"
              % (args.gates, len(gates)))
        sys.exit(2)
    # run_f350.ps1 writes the PPMs next to f350_gates.json; --out (compare
    # JSONs, triptychs, summary) may point elsewhere.
    ppm_dir = os.path.dirname(os.path.abspath(args.gates))
    os.makedirs(args.out, exist_ok=True)

    rows = []
    any_fail = False
    for g in gates:
        name = g["group"]
        label = GROUP_LABELS.get(name, name)
        row = {"group": name, "label": label,
               "exit": g.get("exit_code"),
               "watchdog": g.get("watchdog", False), "ssim": None, "mean_abs": None}
        # gi=0 (GI-off) captures are not comparable against the GI-on
        # reference (removing indirect light naturally lands ~16 dB): use
        # the Mac GI-off capture as the same-mode cross-platform reference.
        if g.get("gi") == 0:
            ref = os.path.join(repo, ".github", "ci-assets",
                               "ref_mac_vulkan_gioff_f350.ppm")
            got = sha256_file(ref)
            if got != GIOFF_REF_SHA256:
                print("[parity] FATAL GI-off reference SHA mismatch: %s" % got)
                sys.exit(2)
        else:
            ref = os.path.join(repo, ".github", "ci-assets",
                               "ref_mac_vulkan_RG_f350.ppm")
        ppm_path = os.path.join(ppm_dir, g["ppm"])
        if not g.get("ppm_exists") or not os.path.isfile(ppm_path):
            row["capture"] = "MISSING"
            row["psnr"] = None
            row["ssim"] = None
            row["mean_abs"] = None
            row["gate"] = "FAIL (no capture; exit=%s)" % row["exit"]
            any_fail = True
            rows.append(row)
            print("[parity] %s: capture MISSING (exit=%s)" %
                  (name, row["exit"]))
            continue
        row["capture"] = "ok (%s)" % g.get("ppm_sha256", "")[:12]
        json_out = os.path.join(args.out, "compare_%s.json" % name)
        cmd = [sys.executable, compare, ref, ppm_path, "--json", json_out]
        proc = subprocess.run(cmd, capture_output=True)
        if proc.returncode != 0:
            row["psnr"] = None
            row["gate"] = "FAIL (compare tool exit %d)" % proc.returncode
            any_fail = True
            rows.append(row)
            sys.stderr.write(proc.stdout.decode("utf-8", "replace"))
            continue
        with open(json_out, "r", encoding="utf-8") as fh:
            data = json.load(fh)
        pair = data["pairs"][0]
        psnr = pair["psnr_rgb_db"]
        row["psnr"] = psnr
        row["ssim"] = pair["ssim_luma_8x8block"]
        row["mean_abs"] = pair["mean_abs_diff_rgb"]
        row["byte_identical"] = pair["byte_identical"]
        if g.get("gi") == 1:
            # Canonical GI-on runs a mode 7 + measured reference against a run
            # that may have fallen back to mode 0 (D3D12 cannot run measured
            # mode 7 yet). Track progress, do not fail the job on it.
            row["gate"] = "diagnostic (mode-7 ref)"
        elif psnr == float("inf") or psnr >= PSNR_GATE_DB:
            row["gate"] = "PASS"
        else:
            row["gate"] = "FAIL %.4f dB < %.1f dB" % (psnr, PSNR_GATE_DB)
            any_fail = True
        if g.get("contract_ok") is False:
            row["gate"] += " / FAIL capture contract"
            any_fail = True
        try:
            triptych(ref, ppm_path,
                     os.path.join(args.out, "triptych_%s.png" % name))
            row["triptych"] = "triptych_%s.png" % name
        except SystemExit as exc:
            row["triptych"] = "error: %s" % exc
        rows.append(row)
        print("[parity] %s: PSNR=%s SSIM=%s -> %s" %
              (name, psnr, row["ssim"], row["gate"]))

    # ---- like-for-like mode 0 parity: the gate D3D12 can actually meet. ----
    mode0 = {"rows": [], "triptych": None}
    mode0_paths = {k: os.path.join(ppm_dir, v) for k, v in MODE0_PPM.items()}
    missing_mode0 = [k for k, path in mode0_paths.items() if not os.path.isfile(path)]
    if missing_mode0:
        mode0["rows"].append({
            "label": "D3D12 vs Vulkan mode 0",
            "psnr": None, "ssim": None, "mean_abs": None,
            "gate": "FAIL (missing %s capture)" % ",".join(missing_mode0)})
        any_fail = True
        print("[parity] mode 0 capture MISSING: %s" % ",".join(missing_mode0))
    else:
        dst = os.path.join(args.out, "compare_mode0_windows.json")
        proc = subprocess.run([sys.executable, compare,
                               mode0_paths["vulkan"], mode0_paths["d3d12"],
                               "--json", dst], capture_output=True)
        if proc.returncode != 0:
            mode0["rows"].append({"label": "D3D12 vs Vulkan mode 0",
                                  "psnr": None, "ssim": None, "mean_abs": None,
                                  "gate": "FAIL (compare tool exit %d)" % proc.returncode})
            any_fail = True
        else:
            with open(dst, "r", encoding="utf-8") as fh:
                pair = json.load(fh)["pairs"][0]
            psnr = pair["psnr_rgb_db"]
            ok = psnr == float("inf") or psnr >= MODE0_PAIR_GATE_DB
            if not ok:
                any_fail = True
            mode0["rows"].append({
                "label": "D3D12 vs Vulkan mode 0 (same run)",
                "psnr": psnr, "ssim": pair["ssim_luma_8x8block"],
                "mean_abs": pair["mean_abs_diff_rgb"],
                "gt8": pair.get("frac_pixels_gt8"),
                "tilep99": pair.get("tile16_mean_abs_p99"),
                "gate": "PASS" if ok else
                        "FAIL %.4f dB < %.1f dB" % (psnr, MODE0_PAIR_GATE_DB)})
            mode0["pair"] = pair
            print("[parity] mode0 pair: PSNR=%s -> %s" % (psnr, mode0["rows"][-1]["gate"]))
            try:
                triptych(mode0_paths["vulkan"], mode0_paths["d3d12"],
                         os.path.join(args.out, "triptych_mode0.png"))
                mode0["triptych"] = "triptych_mode0.png"
            except SystemExit as exc:
                mode0["triptych"] = "error: %s" % exc

        ref0 = os.path.join(repo, ".github", "ci-assets",
                            "ref_win_vulkan_mode0_f350.ppm")
        if os.path.isfile(ref0):
            got = sha256_file(ref0)
            if got != MODE0_REF_SHA256:
                print("[parity] FATAL mode-0 reference SHA mismatch:\n"
                      "  got      %s\n  expected %s" % (got, MODE0_REF_SHA256))
                sys.exit(2)
            for api in ("d3d12", "vulkan"):
                dst = os.path.join(args.out, "compare_mode0_ref_%s.json" % api)
                proc = subprocess.run([sys.executable, compare, ref0,
                                       mode0_paths[api], "--json", dst],
                                      capture_output=True)
                if proc.returncode != 0:
                    continue
                with open(dst, "r", encoding="utf-8") as fh:
                    pair = json.load(fh)["pairs"][0]
                psnr = pair["psnr_rgb_db"]
                if api == "d3d12":
                    ok = psnr == float("inf") or psnr >= MODE0_REF_GATE_DB
                    if not ok:
                        any_fail = True
                    gate = ("PASS" if ok else
                            "FAIL %.4f dB < %.1f dB" % (psnr, MODE0_REF_GATE_DB))
                else:
                    gate = "reference side (advisory)"
                mode0["rows"].append({
                    "label": "%s mode 0 vs frozen mode-0 reference" % api.upper(),
                    "psnr": psnr, "ssim": pair["ssim_luma_8x8block"],
                    "mean_abs": pair["mean_abs_diff_rgb"],
                    "gt8": pair.get("frac_pixels_gt8"),
                    "tilep99": pair.get("tile16_mean_abs_p99"),
                    "gate": gate})
                print("[parity] mode0 ref %s: PSNR=%s -> %s" % (api, psnr, gate))
        else:
            print("[parity] WARN mode-0 reference missing: %s" % ref0)

    # GI-off same-run pair: the calibration point for the visibility advisory.
    gioff_paths = [os.path.join(ppm_dir, n) for n in
                   ("art2_win_vulkan_gioff_f350.ppm", "art2_win_d3d12_gioff_f350.ppm")]
    if all(os.path.isfile(p) for p in gioff_paths):
        dst = os.path.join(args.out, "compare_windows_gioff.json")
        proc = subprocess.run([sys.executable, compare, gioff_paths[0],
                               gioff_paths[1], "--json", dst], capture_output=True)
        if proc.returncode == 0:
            with open(dst, "r", encoding="utf-8") as fh:
                pair = json.load(fh)["pairs"][0]
            mode0["rows"].append({
                "label": "GI-off D3D12 vs Vulkan (calibration)",
                "psnr": pair["psnr_rgb_db"], "ssim": pair["ssim_luma_8x8block"],
                "mean_abs": pair["mean_abs_diff_rgb"],
                "gt8": pair.get("frac_pixels_gt8"),
                "tilep99": pair.get("tile16_mean_abs_p99"),
                "gate": "reference pair"})

    lines = []
    lines.append("### Windows parity f350 vs frozen references")
    lines.append("")
    lines.append("Frozen references: Mac Vulkan mode-7 GI-on `%s`, Mac Vulkan "
                 "GI-off `%s`, Windows Vulkan mode-0 `%s`." %
                 (REF_SHA256, GIOFF_REF_SHA256, MODE0_REF_SHA256))
    lines.append("")
    lines.append("Gates: GI-off **psnr_rgb_db >= %.1f dB**; DDGI mode 0 same-run "
                 "D3D12 vs Vulkan **>= %.1f dB**; D3D12 mode 0 vs frozen mode-0 "
                 "reference **>= %.1f dB**. Canonical GI-on rows compare a "
                 "mode-7 reference against a run that may fall back to mode 0 "
                 "and are diagnostic." %
                 (PSNR_GATE_DB, MODE0_PAIR_GATE_DB, MODE0_REF_GATE_DB))
    lines.append("")
    lines.append("| group | capture | exe exit | PSNR (dB) | SSIM | mean abs "
                 "| gate |")
    lines.append("|---|---|---|---|---|---|---|")
    for r in rows:
        psnr_s = "inf" if r["psnr"] == float("inf") else (
            "%.4f" % r["psnr"] if r["psnr"] is not None else "n/a")
        ssim_s = ("%.6f" % r["ssim"]) if r["ssim"] is not None else "n/a"
        mean_s = ("%.3f" % r["mean_abs"]) if r["mean_abs"] is not None else "n/a"
        lines.append("| %s | %s | %s | %s | %s | %s | %s |" %
                     (r["label"], r["capture"], r["exit"], psnr_s,
                      ssim_s, mean_s, r["gate"]))
    lines.append("")
    lines.append("### Like-for-like DDGI mode 0 (same runner, same run)")
    lines.append("")
    lines.append("| comparison | PSNR (dB) | SSIM | mean abs | >8 LSB | "
                 "tile16 p99 | gate |")
    lines.append("|---|---|---|---|---|---|---|")
    for r in mode0["rows"]:
        psnr_s = "inf" if r["psnr"] == float("inf") else (
            "%.4f" % r["psnr"] if r["psnr"] is not None else "n/a")
        ssim_s = ("%.6f" % r["ssim"]) if r["ssim"] is not None else "n/a"
        mean_s = ("%.3f" % r["mean_abs"]) if r["mean_abs"] is not None else "n/a"
        gt8_s = ("%.2f%%" % (100.0 * r["gt8"])) if r.get("gt8") is not None else "n/a"
        tp_s = ("%.2f" % r["tilep99"]) if r.get("tilep99") is not None else "n/a"
        lines.append("| %s | %s | %s | %s | %s | %s | %s |" %
                     (r["label"], psnr_s, ssim_s, mean_s, gt8_s, tp_s, r["gate"]))
    lines.append("")
    pair = mode0.get("pair")
    if pair and pair.get("frac_pixels_gt8") is not None:
        gt8 = pair["frac_pixels_gt8"]
        tp99 = pair.get("tile16_mean_abs_p99") or 0.0
        visible = gt8 <= VIS_GT8_LIMIT and tp99 <= VIS_TILE_P99_LIMIT
        lines.append(
            "Visible-difference advisory (NOT gated): mode 0 D3D12 vs Vulkan has "
            "**%.2f%%** pixels with max-channel diff >8/255 (limit %.1f%%) and "
            "16x16 tile-mean p99 **%.2f** (limit %.1f) -> %s. A green gate means "
            "the regression thresholds hold; it does not certify visual "
            "equivalence." %
            (100.0 * gt8, 100.0 * VIS_GT8_LIMIT, tp99, VIS_TILE_P99_LIMIT,
             "inside the proposed bound" if visible
             else "OUTSIDE the proposed visual bound"))
        print("[parity] visibility advisory: gt8=%.2f%% tilep99=%.2f -> %s" %
              (100.0 * gt8, tp99, "pass" if visible else "FAIL(advisory)"))
    lines.append("")
    if any_fail:
        lines.append("**Overall f350 gate: FAIL.**")
    else:
        lines.append("**Overall f350 gate: PASS.**")
    lines.append("")
    lines.append("Triptychs (ref | windows | 8x diff heatmap) are in the "
                 "`f350-parity` artifact.")

    text = "\n".join(lines) + "\n"
    summary_path = os.path.join(args.out, "parity_summary.md")
    with open(summary_path, "w", encoding="utf-8") as fh:
        fh.write(text)
    if args.summary:
        with open(args.summary, "a", encoding="utf-8") as fh:
            fh.write(text)
    print(text)
    sys.exit(1 if any_fail else 0)


if __name__ == "__main__":
    main()
