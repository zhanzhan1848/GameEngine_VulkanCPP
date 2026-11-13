#!/usr/bin/env python3
"""parity_report.py - f350 PSNR/SSIM gate + summary for the Windows parity CI.

Reads scripts/win-ci/run_f350.ps1's f350_gates.json, verifies the in-repo Mac
Vulkan reference PPM by SHA256, compares every captured f350 PPM against the
reference with scripts/win-ci/compare_f350.py (pure stdlib), renders
ref|win|diff triptych PNGs (hand-rolled PNG encoder, zlib+struct), and writes
the Markdown PSNR/SSIM table for $GITHUB_STEP_SUMMARY.

Gates (both must hold for exit 0):
  1. capture gate: every one of the four groups produced its f350 PPM;
  2. psnr gate: psnr_rgb_db >= 43.5 dB for every group that produced a PPM
     (byte-identical / Infinity counts as pass).

The 43.5 dB threshold is the frozen noise-floor gate from the parity rounds.
DO NOT lower it: as of r11 the Windows Vulkan GI-on f350 measures 34.43 dB
(gather-resolution family residual) and D3D12 produces no frames at all -
this workflow must report those as FAIL until the engine actually reaches
parity. Advisory bands (for humans reading the summary, not gates):
  >= 43.5  pass (parity noise floor)
  31.8-43.5 SDF root cause cleared; gather-resolution residual (r11 state)
  < 31.8   different fork (missing-SDF / global brightness family)

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
PSNR_GATE_DB = 43.5
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
    n = len(a)
    out = bytearray(n * 3)
    for k in range(n):
        out[k] = a[k]
        out[n + k] = b[k]
        d = a[k] - b[k]
        if d < 0:
            d = -d
        v = d * DIFF_GAIN
        out[2 * n + k] = 255 if v > 255 else v
    write_png(out_path, w * 3, h, bytes(out))


def psnr_band(psnr):
    if psnr == float("inf") or psnr >= 43.5:
        return "pass"
    if psnr >= 31.8:
        return "gather-resolution residual (r11 state)"
    return "below SDF-cleared band"


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
    if len(gates) != 4:
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
               "watchdog": g.get("watchdog", False)}
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
        if psnr == float("inf") or psnr >= PSNR_GATE_DB:
            row["gate"] = "PASS"
        else:
            row["gate"] = ("FAIL %.4f dB < %.1f dB - %s" %
                           (psnr, PSNR_GATE_DB, psnr_band(psnr)))
            any_fail = True
        try:
            triptych(ref, ppm_path,
                     os.path.join(args.out, "triptych_%s.png" % name))
            row["triptych"] = "triptych_%s.png" % name
        except SystemExit as exc:
            row["triptych"] = "error: %s" % exc
        print("[parity] %s: PSNR=%s SSIM=%s -> %s" %
              (name, psnr, row["ssim"], row["gate"]))

    lines = []
    lines.append("### Windows parity f350 vs Mac Vulkan reference")
    lines.append("")
    lines.append("Reference: `.github/ci-assets/ref_mac_vulkan_RG_f350.ppm` "
                 "(SHA256 `%s`, verified)." % REF_SHA256)
    lines.append("")
    lines.append("PSNR gate: **psnr_rgb_db >= %.1f dB** (frozen noise-floor "
                 "gate; NOT lowered for the current 34.43 dB state)." %
                 PSNR_GATE_DB)
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
    if any_fail:
        lines.append("**Overall f350 gate: FAIL.** Known r11 state: D3D12 "
                     "produces no frames; Windows Vulkan GI-on is ~34.43 dB "
                     "(gather-resolution residual, below the 43.5 dB gate). "
                     "These failures are expected and must stay visible.")
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
