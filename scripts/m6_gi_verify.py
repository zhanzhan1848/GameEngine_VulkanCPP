#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M6 GI direct-only-input verifier for TestRHISponzaShared float exports (stdlib only).

Purpose
-------
Proves (or disproves) the M6 GI slice properties of the shared Vulkan/D3D12 harness
(design spec: M6_direct_input_spec.md §3/§4/§6.5/§7/§8) from the raw float exports that
`RHI_GI_EXPORT_FRAMES` writes, and from the harness stderr log:

  frame      masks + every §8 per-frame check (occluded exact zero, lit/penumbra identity and
             no-false-black, direct >= direct_diffuse, composite bit-exact identity, background
             zeros, indirect stats per ROI, GI on/off cross-checks) + PFM/PNG/contact sheet.
  ladder     [GI_MEAN]/RESET parse, per static segment least-squares fit of
             M(f) = T*(1 - 0.9^(f - f0 + 1)), residuals, T ratio across intensity steps
             (consecutive pairs or explicit --pair-ratios i/j=r), model-free ratios at
             equal frames-since-reset ("k-aligned"), monotonicity, degenerate segments.
  linearity  intensity A/B (e.g. 2.2 vs 1.1): per-pixel ratios of direct_diffuse, direct,
             indirect, final_prepost and probe_sh c00 must equal --ratio (= expected B/A;
             no constant / ambient / sky term anywhere).
  selftest   builds synthetic exports with planted defects (fp16 subnormals, a nonzero in an
             occluded pixel, a 1-ulp and a 3-ulp composite error, padded AND tight row_pitch
             rows, ...) and asserts that exactly the planted defects are detected.
  summary    aggregates frame/ladder/linearity/selftest report JSONs into one SUMMARY.json
             with the key numbers, every invocation's argv and this tool's sha256.

Every check yields PASS/FAIL (or SKIP when its inputs are absent) with numbers; every
epsilon/threshold and its justification is written into the report.  Exit code 0 only if
all checks PASS (SKIP does not fail); 1 = some check FAILED; 2 = usage/input error.

Runs with `python3 -I` (isolated mode); Python 3.8+ standard library only (no numpy/PIL).

Export file format (spec §6.5, as written by the harness)
---------------------------------------------------------
  <dir>/giexp_f<frame>.json             manifest, schema "rhi-m6-gi-export/1":
       frame, platform, gi(bool), post(bool), exposure (effective post exposure,
       = render.post.exposure * 2^RHI_POST_EV), width, height,
       camera{position, lookAt, fov_deg, near, far}, light{dir_to_light, intensity, color},
       shadow{...}, gi_grid{dims, box_min, box_max, cell, normal_bias, sh_layout, ema,
       reset_this_frame, frames_since_reset} (GI on only),
       buffers{name: {file, format, bytes_per_pixel, row_pitch, width, height, [depth],
                      semantics, units}}
  <dir>/giexp_f<frame>_<name>.bin       raw readback bytes, `height` rows of `row_pitch`
       bytes each (harness v1: row_pitch = align(width*bpp, 256); harness v2: tight
       row_pitch = width*bpp with an informational readback_row_pitch); only the first
       width*bpp bytes of a row are pixels.  3D (probe_sh): `depth` slices of `height` rows
       of `row_pitch` bytes (v1: 256 B = rowLength 32 texels; v2: 64 B tight).  The reader
       honours row_pitch/bytes_per_pixel/width/height/depth, never assumes tight rows, and
       requires the file size to equal row_pitch*height*max(depth,1) exactly.
  Row 0 of every 2D buffer is the TOP image row (F2 Y-down clip: NDC y=-1 -> RT row 0 on
  both backends), so PNG rows are written in buffer order (no flip).
  Buffer names: direct, direct_diffuse, indirect, final_prepost, probe_sh (RGBA16F),
  depth (D32 -> R32F, 4 B), albedo (RGBA8 UNorm), normal (RG16F), material (RGBA8 UNorm).
  Accepted format strings (case/punctuation-insensitive): RGBA16_Float|RGBA16F,
  RG16_Float|RG16F, RGBA8_UNorm|RGBA8, BGRA8_UNorm|BGRA8, D32_Float|R32_Float|R32F.
  GI off exports contain only final_prepost, depth, albedo, normal, material.

Units and conventions
---------------------
  Radiance = harness linear HDR, pre-exposure, pre-ACES.  Lambert WITHOUT 1/pi: a white
  surface lit head-on by colour*intensity = 1 has value 1.0.  Harness irradiance
  E_h = E_phys/pi; displayed diffuse = albedo * E_h.  albedo = u8/255 (UNORM, no sRGB).
  normal: RG16F xy, z = (material.a*2-1) * sqrt(max(0, 1-x^2-y^2)).
  probe_sh texel (i, j, s*8+k) = (c00, c1-1, c10, c11) of probe (i,j,k), slab s = R,G,B;
  isotropic mean = 0.282095 * c00 (what [GI_MEAN] averages over the 512 probes).
  fp16 decode = exact float32 value of the half (65536-entry table; subnormals, +-0,
  inf/nan exact; cross-checked against struct '<e' in selftest).  fp16 rounding of expected
  values is evaluated under BOTH RT-write rounding modes: round-half-even (struct.pack
  '<e'; |x| >= 65520 -> inf) and round-toward-zero (mantissa truncation; overflow -> max
  finite 65504) -- Mac MoltenVK converts f32->f16 on RT writes with RTZ, D3D12/NVIDIA with
  RNE, so bit-exact checks report both match fractions, infer the mode, and PASS iff
  either mode matches 100% (<=1 ulp still counted separately).  float32 rounding of
  intermediate GPU arithmetic = struct.pack('<f') (round-half-to-even).

Masks (from the GI-on export, spec §8)
--------------------------------------
  valid = depth < 1;  vis = direct_diffuse.a (shadow);  ndl = direct.a
  occluded   = valid and (vis == 0 or ndl == 0)       (the only light is the sun)
  penumbra   = valid and 0 < vis < 1 and ndl > 0
  lit        = valid and vis == 1 and ndl > 0
  background = not valid
  (A valid pixel with vis > 1 or NaN would be 'unclassified'; it is counted and reported.)

Image outputs (frame)
---------------------
  PFM: float32 RGB (alpha dropped), little-endian, header "PF\\n<w> <h>\\n-1.000000\\n",
       rows stored BOTTOM-TO-TOP as the PFM spec requires (first stored row = image bottom).
  PNG: 8-bit RGB, M3Post-identical LDR mapping: v = clamp(ACES(x*exposure), 0, 1),
       ACES(x) = x(2.51x+0.03) / (x(2.43x+0.59)+0.14), u8 = floor(v*255 + 0.5); NO gamma
       (the harness post encodes linear tonemapped values straight into BGRA8 UNorm).
  Files: giexp_f<N>_<name>.{pfm,png} for GI-on buffers, giexp_f<N>_off_final_prepost.* for
  the GI-off run, mask_overlay.png, diff_on_off_x3.png (= tonemap(3*|final_on-final_off|)),
  same-view-gi-comparison-v2.png (2x3 labelled contact sheet), indirect_x<G>.png
  (= tonemap(G*indirect), G = --indirect-gain, default 16, printed in the image),
  indirect-structure.png (2x2: indirect x1 | x G / E_h = indirect/albedo x G/4 |
  direct_diffuse), report.json.

Usage (macOS/Linux; on Windows use `py -3 -I` and backslash paths, e.g. C:\\cap\\gi_on)
------------------------------------------------------------------------------------
  python3 -I scripts/m6_gi_verify.py selftest
  python3 -I scripts/m6_gi_verify.py frame --on <gi_on_dir> --frame 89 \\
          [--off <gi_off_dir> [--off-frame 89]] --out <out_dir>
  python3 -I scripts/m6_gi_verify.py ladder --log <stderr.log> \\
          [--segments 90-115,132-155] [--intensity-ratio 2.588235] --out <report.json>
  python3 -I scripts/m6_gi_verify.py ladder --log <steps_stderr.log> \\
          --segments 0-44,45-89,90-119 --pair-ratios 2/1=0.386364,3/2=2.588235,3/1=1 \\
          --ratio-tol 0.02 --out <report.json>
  python3 -I scripts/m6_gi_verify.py linearity --a <dir_int_2.2> --b <dir_int_1.1> \\
          --frame 119 --ratio 0.5 --out <report.json>      (--ratio = expected B/A)
  python3 -I scripts/m6_gi_verify.py summary --reports <a.json> <b.json> ... --out SUMMARY.json
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import struct
import sys
import tempfile
import time
import zlib
from array import array

TOOL_VERSION = "m6_gi_verify/1"
EXPORT_SCHEMA = "rhi-m6-gi-export/1"

# ----------------------------------------------------------------------------------------
# fp16 / fp32 helpers
# ----------------------------------------------------------------------------------------

_F32 = struct.Struct("<f")
_U32 = struct.Struct("<I")
_E16 = struct.Struct("<e")

HALF_MIN_SUBNORMAL = 2.0 ** -24          # smallest nonzero |fp16|
HALF_MIN_NORMAL = 2.0 ** -14             # smallest normal fp16
HALF_REL_ROUND = 2.0 ** -11              # max relative rounding error of fp16 (normal range)
HALF_ONE = 0x3C00


def _build_half_table():
    """65536-entry fp16 bit pattern -> exact float32 value (as array('f'))."""
    tbl = array("f", bytes(4 * 65536))
    pack_f = _F32.pack
    unpack_u = _U32.unpack
    pack_u = _U32.pack
    unpack_f = _F32.unpack
    for h in range(65536):
        sign = (h >> 15) & 1
        exp = (h >> 10) & 0x1F
        mant = h & 0x3FF
        if exp == 0:
            # +-0 or subnormal: mant * 2^-24 is exactly representable as a normal float32.
            v = math.ldexp(mant, -24) if mant else 0.0
            if sign:
                v = -v
            bits = unpack_u(pack_f(v))[0]
        elif exp == 31:
            bits = (sign << 31) | 0x7F800000 | (mant << 13)   # inf / nan (payload kept)
        else:
            bits = (sign << 31) | ((exp + 112) << 23) | (mant << 13)
        tbl[h] = unpack_f(pack_u(bits))[0]
    return tbl


HALF_TABLE = _build_half_table()


def f32(x):
    """Round a Python float to the nearest float32 (round-half-even), returned as float."""
    return _F32.unpack(_F32.pack(x))[0]


_FP16_CACHE = {}


def fp16_bits(x):
    """IEEE fp16 round-half-to-even bit pattern of x (|x| >= 65520 -> +-inf)."""
    v = _FP16_CACHE.get(x)
    if v is not None:
        return v
    try:
        b = _E16.pack(x)
    except OverflowError:
        b = _E16.pack(math.inf if x > 0 else -math.inf)
    v = b[0] | (b[1] << 8)
    if len(_FP16_CACHE) < (1 << 20):
        _FP16_CACHE[x] = v
    return v


def half_is_nan(bits):
    return (bits & 0x7C00) == 0x7C00 and (bits & 0x3FF) != 0


def rtz_fp16_bits(x):
    """IEEE round-toward-zero (truncation) float32 -> fp16 bits.

    The Mac Vulkan path (MoltenVK/ Metal) converts f32->f16 on RGBA16F render-target
    writes with round-toward-zero; D3D12/NVIDIA use round-half-even.  Bit-exact checks
    therefore evaluate the expectation under BOTH modes (see selftest: RTZ is idempotent
    on every fp16-exact value, i.e. rtz_fp16_bits(HALF_TABLE[h]) == h for all 65536 h).
    Overflow truncates to +-65504 under RTZ (IEEE alternate rounding), not to inf.
    """
    try:
        u = _U32.unpack(_F32.pack(x))[0]
    except (OverflowError, ValueError):
        return 0x7E00
    sign = (u >> 31) << 15
    rest = u & 0x7FFFFFFF
    if rest >= 0x7F800000:
        if rest == 0x7F800000:
            return sign | 0x7BFF        # +-inf overflows to max finite under RTZ
        return sign | 0x7E00            # NaN -> default qNaN
    e32 = (rest >> 23) - 127
    frac = (rest & 0x7FFFFF) | 0x800000  # 24-bit significand incl. implicit bit
    if e32 >= 16:
        return sign | 0x7BFF
    if e32 >= -14:
        return sign | ((e32 + 15) << 10) | ((frac >> 13) & 0x3FF)
    if e32 >= -24:
        return sign | (frac >> (-e32 - 1))
    return sign


def ulp_key(bits):
    """Monotone integer key for fp16 bits; +0 and -0 map to the same key."""
    mag = bits & 0x7FFF
    return -mag if bits & 0x8000 else mag


def ulp_diff(a_bits, b_bits):
    return abs(ulp_key(a_bits) - ulp_key(b_bits))


# ----------------------------------------------------------------------------------------
# small numeric helpers
# ----------------------------------------------------------------------------------------

def pct_sorted(sorted_vals, q):
    """Linear-interpolated percentile (q in [0,100]) of an already sorted list."""
    n = len(sorted_vals)
    if n == 0:
        return None
    if n == 1:
        return sorted_vals[0]
    pos = (n - 1) * q / 100.0
    lo = int(math.floor(pos))
    hi = min(lo + 1, n - 1)
    t = pos - lo
    return sorted_vals[lo] * (1.0 - t) + sorted_vals[hi] * t


class LogHist:
    """Deterministic log10 histogram for non-negative error magnitudes (percentiles approx.).

    Bins cover [10^lo, 10^hi) with `bins` equal log-width bins (resolution
    (hi-lo)/bins decades); zeros and values below 10^lo go to an underflow bin.  The reported
    percentile is the geometric centre of the bin holding that rank (exact max is tracked
    separately).
    """

    def __init__(self, lo=-9.0, hi=2.0, bins=440):
        self.lo, self.hi, self.bins = lo, hi, bins
        self.under = 0
        self.over = 0
        self.counts = [0] * bins
        self.n = 0
        self.maxv = 0.0
        self.scale = bins / (hi - lo)

    def add(self, v):
        self.n += 1
        if v > self.maxv:
            self.maxv = v
        if v <= 0.0:
            self.under += 1
            return
        t = (math.log10(v) - self.lo) * self.scale
        if t < 0:
            self.under += 1
        elif t >= self.bins:
            self.over += 1
        else:
            self.counts[int(t)] += 1

    def percentile(self, q):
        if self.n == 0:
            return None
        rank = q / 100.0 * self.n
        acc = self.under
        if acc >= rank and self.under > 0:
            return 0.0
        for i, c in enumerate(self.counts):
            acc += c
            if acc >= rank and c > 0:
                return 10.0 ** (self.lo + (i + 0.5) / self.scale)
        return self.maxv


class LinHist:
    """Deterministic linear histogram on [lo, hi) with outside counters; exact min/max."""

    def __init__(self, lo, hi, bins=4000):
        self.lo, self.hi, self.bins = lo, hi, bins
        self.counts = [0] * bins
        self.below = 0
        self.above = 0
        self.n = 0
        self.minv = None
        self.maxv = None
        self.scale = bins / (hi - lo)

    def add(self, v):
        self.n += 1
        if self.minv is None or v < self.minv:
            self.minv = v
        if self.maxv is None or v > self.maxv:
            self.maxv = v
        t = (v - self.lo) * self.scale
        if t < 0:
            self.below += 1
        elif t >= self.bins:
            self.above += 1
        else:
            self.counts[int(t)] += 1

    def percentile(self, q):
        if self.n == 0:
            return None
        rank = q / 100.0 * self.n
        acc = self.below
        if acc >= rank and self.below > 0:
            return self.minv
        for i, c in enumerate(self.counts):
            acc += c
            if acc >= rank and c > 0:
                return self.lo + (i + 0.5) / self.scale
        return self.maxv


def summarize_sorted(vals):
    """mean/p50/p95/max of a list (sorted in place)."""
    if not vals:
        return {"n": 0, "mean": None, "p50": None, "p95": None, "max": None}
    vals.sort()
    return {"n": len(vals), "mean": sum(vals) / len(vals), "p50": pct_sorted(vals, 50),
            "p95": pct_sorted(vals, 95), "max": vals[-1]}


# ----------------------------------------------------------------------------------------
# export reader
# ----------------------------------------------------------------------------------------

class ExportError(Exception):
    pass


_FORMAT_ALIASES = {
    "rgba16float": "rgba16f", "rgba16f": "rgba16f", "rgba16sfloat": "rgba16f",
    "rg16float": "rg16f", "rg16f": "rg16f", "rg16sfloat": "rg16f",
    "rgba8unorm": "rgba8", "rgba8": "rgba8",
    "bgra8unorm": "bgra8", "bgra8": "bgra8",
    "d32float": "r32f", "r32float": "r32f", "r32f": "r32f", "d32": "r32f", "d32sfloat": "r32f",
    "r32sfloat": "r32f",
}
_FORMAT_BPP = {"rgba16f": 8, "rg16f": 4, "rgba8": 4, "bgra8": 4, "r32f": 4}


def normalize_format(fmt):
    key = re.sub(r"[^a-z0-9]", "", str(fmt).lower())
    if key not in _FORMAT_ALIASES:
        raise ExportError("unknown buffer format %r" % (fmt,))
    return _FORMAT_ALIASES[key]


def _load_json_lenient(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        # C++ iostreams print non-finite floats as nan/inf: map them to JSON-ish constants.
        fixed = re.sub(r"(?<=[\s\[,:])-?nan(?=[\s,\]}])", "NaN", text)
        fixed = re.sub(r"(?<=[\s\[,:])(-?)inf(?=[\s,\]}])", r"\1Infinity", fixed)
        return json.loads(fixed)


class Buffer:
    """One decoded export buffer (tight rows, row 0 = top)."""

    def __init__(self, name, entry, fmt, raw):
        self.name = name
        self.entry = entry
        self.fmt = fmt
        self.w = int(entry["width"])
        self.h = int(entry["height"])
        self.d = int(entry.get("depth") or 0)
        self.raw = raw                       # array('B'), tight
        self.halves = None                   # array('H') for f16 formats
        self.floats = None                   # array('f') for f16 / f32 formats
        if fmt in ("rgba16f", "rg16f"):
            hv = array("H")
            hv.frombytes(raw.tobytes())
            if sys.byteorder == "big":
                hv.byteswap()
            self.halves = hv
            tbl = HALF_TABLE
            out = array("f", bytes(4 * len(hv)))
            for i, b in enumerate(hv):
                out[i] = tbl[b]
            self.floats = out
        elif fmt == "r32f":
            fv = array("f")
            fv.frombytes(raw.tobytes())
            if sys.byteorder == "big":
                fv.byteswap()
            self.floats = fv

    @property
    def is_u8(self):
        return self.fmt in ("rgba8", "bgra8")

    def rgb_float(self, i):
        """(r, g, b) of pixel i as floats (u8 formats -> /255)."""
        if self.fmt == "rgba16f":
            f = self.floats
            j = 4 * i
            return f[j], f[j + 1], f[j + 2]
        r = self.raw
        j = 4 * i
        if self.fmt == "bgra8":
            return r[j + 2] / 255.0, r[j + 1] / 255.0, r[j] / 255.0
        return r[j] / 255.0, r[j + 1] / 255.0, r[j + 2] / 255.0


def read_export_buffer(mdir, name, entry):
    fmt = normalize_format(entry.get("format", ""))
    bpp = int(entry["bytes_per_pixel"])
    if bpp != _FORMAT_BPP[fmt]:
        raise ExportError("%s: bytes_per_pixel %d does not match format %s" % (name, bpp, fmt))
    w, h = int(entry["width"]), int(entry["height"])
    d = int(entry.get("depth") or 0)
    pitch = int(entry["row_pitch"])
    row = w * bpp
    if w <= 0 or h <= 0 or pitch < row:
        raise ExportError("%s: bad geometry w=%d h=%d row_pitch=%d bpp=%d" % (name, w, h, pitch, bpp))
    slices = d if d > 0 else 1
    path = os.path.join(mdir, entry["file"])
    expect = pitch * h * slices
    size = os.path.getsize(path)
    if size != expect:
        hint = ""
        rb = entry.get("readback_row_pitch")
        if rb and int(rb) * h * slices == size:
            hint = (" (file size matches readback_row_pitch=%s layout: manifest row_pitch and "
                    "file layout disagree)" % rb)
        raise ExportError("%s: file %s has %d bytes, manifest row_pitch*height*depth = %d%s"
                          % (name, path, size, expect, hint))
    src = array("B")
    with open(path, "rb") as fh:
        src.fromfile(fh, expect)
    if pitch == row:
        tight = src
    else:
        tight = array("B", bytes(row * h * slices))
        o = 0
        for r in range(h * slices):
            s = r * pitch
            tight[o:o + row] = src[s:s + row]
            o += row
    return Buffer(name, entry, fmt, tight), size


class ExportFrame:
    """Manifest + all buffers of one exported frame."""

    def __init__(self, mdir, frame):
        self.dir = mdir
        self.frame = int(frame)
        self.stem = "giexp_f%d" % self.frame
        mpath = os.path.join(mdir, self.stem + ".json")
        if not os.path.isfile(mpath):
            raise ExportError("manifest not found: %s" % mpath)
        self.manifest_path = mpath
        m = _load_json_lenient(mpath)
        self.m = m
        if m.get("schema") != EXPORT_SCHEMA:
            raise ExportError("%s: schema %r != %r" % (mpath, m.get("schema"), EXPORT_SCHEMA))
        if int(m.get("frame", -1)) != self.frame:
            raise ExportError("%s: frame field %r != %d" % (mpath, m.get("frame"), self.frame))
        self.w = int(m["width"])
        self.h = int(m["height"])
        self.gi = bool(m.get("gi"))
        self.post = bool(m.get("post"))
        self.exposure = float(m.get("exposure", 1.0))
        light = m.get("light") or {}
        self.intensity = float(light.get("intensity", 0.0))
        self.color = [float(c) for c in (light.get("color") or [1.0, 1.0, 1.0])]
        # lightColor = color.rgb * intensity, both float32 in the shader.
        self.light_color = [f32(f32(c) * f32(self.intensity)) for c in self.color]
        self.buffers = {}
        self.file_sizes = {}
        for name, entry in (m.get("buffers") or {}).items():
            buf, size = read_export_buffer(mdir, name, entry)
            if buf.d == 0 and (buf.w != self.w or buf.h != self.h):
                raise ExportError("%s: %dx%d differs from frame %dx%d" % (name, buf.w, buf.h, self.w, self.h))
            self.buffers[name] = buf
            self.file_sizes[name] = size

    def get(self, name):
        return self.buffers.get(name)

    def summary(self):
        return {
            "dir": os.path.abspath(self.dir), "manifest": os.path.basename(self.manifest_path),
            "frame": self.frame, "platform": self.m.get("platform"), "gi": self.gi,
            "post": self.post, "exposure": self.exposure, "width": self.w, "height": self.h,
            "light": self.m.get("light"), "camera": self.m.get("camera"),
            "gi_grid": self.m.get("gi_grid"),
            "buffers": {n: {"format": b.entry.get("format"), "fmt": b.fmt,
                            "bytes_per_pixel": b.entry.get("bytes_per_pixel"),
                            "row_pitch": b.entry.get("row_pitch"), "width": b.w,
                            "height": b.h, "depth": b.d or None,
                            "file_bytes": self.file_sizes[n]}
                        for n, b in sorted(self.buffers.items())},
        }


# ----------------------------------------------------------------------------------------
# image writers (PNG via zlib, PFM via struct) and a 5x7 bitmap font
# ----------------------------------------------------------------------------------------

def write_png(path, w, h, rgb):
    """8-bit RGB PNG; rgb = bytes-like of length w*h*3, row 0 = top."""
    row = 3 * w
    if len(rgb) != row * h:
        raise ValueError("png payload size mismatch")
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


def write_pfm_rgb(path, w, h, floats4):
    """float32 RGB PFM from an RGBA float array (row 0 = top); stored bottom-to-top."""
    out = array("f", bytes(4 * 3 * w * h))
    o = 0
    for y in range(h - 1, -1, -1):
        base = 4 * w * y
        for x in range(w):
            j = base + 4 * x
            out[o] = floats4[j]
            out[o + 1] = floats4[j + 1]
            out[o + 2] = floats4[j + 2]
            o += 3
    if sys.byteorder == "big":
        out.byteswap()
    with open(path, "wb") as fh:
        fh.write(("PF\n%d %d\n-1.000000\n" % (w, h)).encode("ascii"))
        fh.write(out.tobytes())


FONT_5X7 = {
    "A": ".###.|#...#|#...#|#####|#...#|#...#|#...#",
    "B": "####.|#...#|#...#|####.|#...#|#...#|####.",
    "C": ".###.|#...#|#....|#....|#....|#...#|.###.",
    "D": "###..|#..#.|#...#|#...#|#...#|#..#.|###..",
    "E": "#####|#....|#....|####.|#....|#....|#####",
    "F": "#####|#....|#....|####.|#....|#....|#....",
    "G": ".###.|#...#|#....|#.###|#...#|#...#|.####",
    "H": "#...#|#...#|#...#|#####|#...#|#...#|#...#",
    "I": ".###.|..#..|..#..|..#..|..#..|..#..|.###.",
    "J": "..###|...#.|...#.|...#.|...#.|#..#.|.##..",
    "K": "#...#|#..#.|#.#..|##...|#.#..|#..#.|#...#",
    "L": "#....|#....|#....|#....|#....|#....|#####",
    "M": "#...#|##.##|#.#.#|#.#.#|#...#|#...#|#...#",
    "N": "#...#|#...#|##..#|#.#.#|#..##|#...#|#...#",
    "O": ".###.|#...#|#...#|#...#|#...#|#...#|.###.",
    "P": "####.|#...#|#...#|####.|#....|#....|#....",
    "Q": ".###.|#...#|#...#|#...#|#.#.#|#..#.|.##.#",
    "R": "####.|#...#|#...#|####.|#.#..|#..#.|#...#",
    "S": ".####|#....|#....|.###.|....#|....#|####.",
    "T": "#####|..#..|..#..|..#..|..#..|..#..|..#..",
    "U": "#...#|#...#|#...#|#...#|#...#|#...#|.###.",
    "V": "#...#|#...#|#...#|#...#|#...#|.#.#.|..#..",
    "W": "#...#|#...#|#...#|#.#.#|#.#.#|##.##|#...#",
    "X": "#...#|#...#|.#.#.|..#..|.#.#.|#...#|#...#",
    "Y": "#...#|#...#|.#.#.|..#..|..#..|..#..|..#..",
    "Z": "#####|....#|...#.|..#..|.#...|#....|#####",
    "0": ".###.|#...#|#..##|#.#.#|##..#|#...#|.###.",
    "1": "..#..|.##..|..#..|..#..|..#..|..#..|.###.",
    "2": ".###.|#...#|....#|...#.|..#..|.#...|#####",
    "3": "#####|...#.|..#..|...#.|....#|#...#|.###.",
    "4": "...#.|..##.|.#.#.|#..#.|#####|...#.|...#.",
    "5": "#####|#....|####.|....#|....#|#...#|.###.",
    "6": "..##.|.#...|#....|####.|#...#|#...#|.###.",
    "7": "#####|....#|...#.|..#..|.#...|.#...|.#...",
    "8": ".###.|#...#|#...#|.###.|#...#|#...#|.###.",
    "9": ".###.|#...#|#...#|.####|....#|...#.|.##..",
    " ": ".....|.....|.....|.....|.....|.....|.....",
    "-": ".....|.....|.....|.###.|.....|.....|.....",
    "_": ".....|.....|.....|.....|.....|.....|#####",
    ".": ".....|.....|.....|.....|.....|.##..|.##..",
    ",": ".....|.....|.....|.....|.##..|..#..|.#...",
    ":": ".....|.##..|.##..|.....|.##..|.##..|.....",
    "/": "....#|...#.|...#.|..#..|.#...|.#...|#....",
    "(": "...#.|..#..|.#...|.#...|.#...|..#..|...#.",
    ")": ".#...|..#..|...#.|...#.|...#.|..#..|.#...",
    "[": ".###.|.#...|.#...|.#...|.#...|.#...|.###.",
    "]": ".###.|...#.|...#.|...#.|...#.|...#.|.###.",
    "%": "##..#|##.#.|...#.|..#..|.#...|.#.##|#..##",
    "=": ".....|.....|#####|.....|#####|.....|.....",
    "+": ".....|..#..|..#..|#####|..#..|..#..|.....",
    "|": "..#..|..#..|..#..|..#..|..#..|..#..|..#..",
    "<": "...#.|..#..|.#...|#....|.#...|..#..|...#.",
    ">": ".#...|..#..|...#.|....#|...#.|..#..|.#...",
    "!": "..#..|..#..|..#..|..#..|..#..|.....|..#..",
    "*": ".....|#.#.#|.###.|#####|.###.|#.#.#|.....",
    "#": ".#.#.|#####|.#.#.|.#.#.|#####|.#.#.|.....",
    "'": "..#..|..#..|.#...|.....|.....|.....|.....",
}
_GLYPHS = {}
for _ch, _rows in FONT_5X7.items():
    _GLYPHS[_ch] = [[x for x, c in enumerate(r) if c == "#"] for r in _rows.split("|")]


def draw_text(img, w, h, x0, y0, text, color=(255, 255, 255), scale=1):
    """Draw text with the 5x7 font into an RGB bytearray (clipped). Returns end x."""
    x = x0
    for ch in str(text).upper():
        glyph = _GLYPHS.get(ch, _GLYPHS["_"])
        for gy, cols in enumerate(glyph):
            for gx in cols:
                for sy in range(scale):
                    py = y0 + gy * scale + sy
                    if py < 0 or py >= h:
                        continue
                    for sx in range(scale):
                        px = x + gx * scale + sx
                        if 0 <= px < w:
                            j = 3 * (py * w + px)
                            img[j] = color[0]
                            img[j + 1] = color[1]
                            img[j + 2] = color[2]
        x += 6 * scale
    return x


def fill_rect(img, w, h, x0, y0, x1, y1, color):
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(w, x1), min(h, y1)
    if x1 <= x0 or y1 <= y0:
        return
    rowbytes = bytes(color) * (x1 - x0)
    for y in range(y0, y1):
        j = 3 * (y * w + x0)
        img[j:j + 3 * (x1 - x0)] = rowbytes


# ----------------------------------------------------------------------------------------
# M3Post-identical LDR mapping
# ----------------------------------------------------------------------------------------

ACES_A, ACES_B, ACES_C, ACES_D, ACES_E = 2.51, 0.03, 2.43, 0.59, 0.14


def tonemap_u8(x, exposure):
    """clamp(ACES(x*exposure),0,1) -> round(v*255) (no gamma), NaN -> 0."""
    v = x * exposure
    if not v > 0.0:          # also catches NaN
        return 0
    t = (v * (ACES_A * v + ACES_B)) / (v * (ACES_C * v + ACES_D) + ACES_E)
    if t >= 1.0:
        return 255
    return int(t * 255.0 + 0.5)


def ldr_from_rgba_floats(floats4, n, exposure, scale=1.0):
    """RGB bytes of n pixels from an RGBA float array, via the M3Post mapping."""
    out = bytearray(3 * n)
    cache = {}
    ex = exposure * scale
    for i in range(n):
        j = 4 * i
        for c in range(3):
            v = floats4[j + c]
            u = cache.get(v)
            if u is None:
                u = tonemap_u8(v, ex)
                cache[v] = u
            out[3 * i + c] = u
    return out


def ldr_from_buffer(buf, n, exposure):
    if buf.fmt == "rgba16f":
        return ldr_from_rgba_floats(buf.floats, n, exposure)
    out = bytearray(3 * n)            # 8-bit final (POST=0): already display values
    r = buf.raw
    for i in range(n):
        j = 4 * i
        if buf.fmt == "bgra8":
            out[3 * i], out[3 * i + 1], out[3 * i + 2] = r[j + 2], r[j + 1], r[j]
        else:
            out[3 * i], out[3 * i + 1], out[3 * i + 2] = r[j], r[j + 1], r[j + 2]
    return out


# ----------------------------------------------------------------------------------------
# masks
# ----------------------------------------------------------------------------------------

MASK_BG, MASK_OCC, MASK_PEN, MASK_LIT, MASK_OTHER = 0, 1, 2, 3, 4
MASK_NAMES = {0: "background", 1: "occluded", 2: "penumbra", 3: "lit", 4: "unclassified"}
MASK_COLORS = {MASK_BG: (24, 34, 66), MASK_OCC: (255, 64, 64), MASK_PEN: (255, 184, 48),
               MASK_LIT: (72, 200, 88), MASK_OTHER: (160, 60, 200)}


def build_masks(on):
    """bytearray mask codes from depth/direct.a/direct_diffuse.a (spec §8)."""
    depth = on.get("depth")
    direct = on.get("direct")
    dd = on.get("direct_diffuse")
    n = on.w * on.h
    if depth is None or direct is None or dd is None:
        raise ExportError("GI-on export is missing depth/direct/direct_diffuse")
    df = depth.floats
    da = direct.floats
    dda = dd.floats
    masks = bytearray(n)
    counts = {0: 0, 1: 0, 2: 0, 3: 0, 4: 0}
    for i in range(n):
        valid = df[i] < 1.0
        if not valid:
            masks[i] = MASK_BG
            counts[0] += 1
            continue
        ndl = da[4 * i + 3]
        vis = dda[4 * i + 3]
        if vis == 0.0 or ndl == 0.0:
            masks[i] = MASK_OCC
            counts[1] += 1
        elif 0.0 < vis < 1.0 and ndl > 0.0:
            masks[i] = MASK_PEN
            counts[2] += 1
        elif vis == 1.0 and ndl > 0.0:
            masks[i] = MASK_LIT
            counts[3] += 1
        else:
            masks[i] = MASK_OTHER
            counts[4] += 1
    return masks, counts


# ----------------------------------------------------------------------------------------
# frame checks (spec §8)
# ----------------------------------------------------------------------------------------

def check_occluded_zero_exact(masks, on):
    """Occluded pixels: direct.rgb and direct_diffuse.rgb must be fp16 +-0 exactly."""
    direct, dd = on.get("direct"), on.get("direct_diffuse")
    anom = 0
    anom_gt = 0
    nan_cnt = 0
    maxv = 0.0
    total = 0.0
    cnt = 0
    occ_px = 0
    dh, df = direct.halves, direct.floats
    qh, qf = dd.halves, dd.floats
    for i, m in enumerate(masks):
        if m != MASK_OCC:
            continue
        occ_px += 1
        j = 4 * i
        for k in range(3):
            cnt += 1
            bh = dh[j + k]
            v = abs(df[j + k])
            total += v
            if bh & 0x7FFF:
                anom += 1
                if half_is_nan(bh):
                    nan_cnt += 1
                elif v > HALF_MIN_SUBNORMAL:
                    anom_gt += 1
                if v > maxv:
                    maxv = v
            bv = abs(qf[j + k])
            if qh[j + k] & 0x7FFF:
                anom += 1
                if half_is_nan(qh[j + k]):
                    nan_cnt += 1
                elif bv > HALF_MIN_SUBNORMAL:
                    anom_gt += 1
                if bv > maxv:
                    maxv = bv
    ok = anom == 0
    return {
        "id": "occluded_zero_exact", "desc": "occluded: direct/direct_diffuse rgb == fp16 +-0 "
        "(bits 0x0000/0x8000); epsilon = 0 exactly",
        "status": "PASS" if ok else "FAIL",
        "occluded_pixels": occ_px,
        "channel_samples": cnt, "anomaly_count": anom,
        "anomaly_count_gt_2^-24": anom_gt, "nan_count": nan_cnt,
        "anomaly_count_equal": anom == anom_gt,
        "max_abs_value": maxv, "mean_abs_value": (total / cnt) if cnt else None,
        "justification": "spec §8: eps_occ = 0 exactly; any nonzero fp16 bits (incl. "
        "subnormals >= 2^-24) are anomalies, so anomaly_count == anomaly_count_gt_2^-24 "
        "(+ nan_count) must hold for both reported counts",
    }


def check_dd_identity(masks, on):
    """Lit+penumbra: |dd - albedo*lightColor*ndl*vis| <= 4e-3*expected + 1e-5."""
    dd = on.get("direct_diffuse")
    albedo = on.get("albedo")
    direct = on.get("direct")
    if dd is None or albedo is None or direct is None:
        return {"id": "dd_identity", "desc": "lit/penumbra dd identity",
                "status": "SKIP", "reason": "direct/direct_diffuse/albedo missing"}
    df = dd.floats
    da = direct.floats
    ar = albedo.raw
    lc = on.light_color
    rels = []
    viol = 0
    viol_px = 0
    max_abs = 0.0          # over ALL checked channels
    max_rel = 0.0          # over all channels with expected > 1e-6
    worst = None           # worst violating channel (if any)
    worst_excess = 0.0
    used = 0
    for i, m in enumerate(masks):
        if m not in (MASK_LIT, MASK_PEN):
            continue
        used += 1
        j = 4 * i
        a8 = 4 * i
        ndl = da[j + 3]
        vis = df[j + 3]
        pv = False
        for c in range(3):
            exp = (ar[a8 + c] / 255.0) * lc[c] * ndl * vis
            got = df[j + c]
            diff = abs(got - exp)
            if diff > max_abs:
                max_abs = diff
            if exp > 1e-6:
                rel = diff / exp
                rels.append(rel)
                if rel > max_rel:
                    max_rel = rel
            excess = diff - (4e-3 * exp + 1e-5)
            if excess > 0.0:
                viol += 1
                pv = True
                if excess > worst_excess:
                    worst_excess = excess
                    worst = (i % on.w, i // on.w, c, got, exp)
        if pv:
            viol_px += 1
    rels.sort()
    status = "PASS" if viol == 0 else "FAIL"
    return {
        "id": "dd_identity", "desc": "lit+penumbra: |dd - albedo*lightColor*ndl16*vis16| "
        "<= 4e-3*expected + 1e-5 (spec §8 identity; absorbs fp16 quantization of "
        "ndl/vis/albedo and f32 shader arithmetic)",
        "status": status, "pixels_checked": used, "violation_channels": viol,
        "violation_pixels": viol_px, "rel_err_samples": len(rels),
        "rel_err_p50": pct_sorted(rels, 50), "rel_err_p99": pct_sorted(rels, 99),
        "max_abs_err": max_abs, "max_rel_err": max_rel, "worst_violation": worst,
        "thresholds": {"rel": 4e-3, "abs": 1e-5},
        "justification": "spec §8 tolerance; albedo = u8/255 exact, ndl/vis = exact f32 "
        "decode of the fp16 alpha channels; rel-err percentiles are exact (sorted, linear "
        "interpolation) over channels with expected > 1e-6; max_abs/max_rel are over ALL "
        "checked channels (not only violators)",
    }


def check_no_false_black(masks, on):
    dd = on.get("direct_diffuse")
    albedo = on.get("albedo")
    direct = on.get("direct")
    if dd is None or albedo is None or direct is None:
        return {"id": "no_false_black", "desc": "no false black in lit/penumbra",
                "status": "SKIP", "reason": "direct/direct_diffuse/albedo missing"}
    df, ar, da = dd.floats, albedo.raw, direct.floats
    lc = on.light_color
    lcmax = max(lc)
    cnt = 0
    underflow = 0
    examples = []
    for i, m in enumerate(masks):
        if m not in (MASK_LIT, MASK_PEN):
            continue
        a8 = 4 * i
        amax = max(ar[a8], ar[a8 + 1], ar[a8 + 2])
        if amax > 1 and df[4 * i] == 0.0 and df[4 * i + 1] == 0.0 and df[4 * i + 2] == 0.0:
            cnt += 1
            # exact product of the largest channel: below the smallest fp16 subnormal
            # (2^-24) an exact zero is the correctly-rounded store under RTZ (and under
            # RNE below 2^-25) -- quantization underflow, not a missing lighting term.
            exp_max = (amax / 255.0) * lcmax * da[4 * i + 3] * df[4 * i + 3]
            if exp_max <= HALF_MIN_SUBNORMAL:
                underflow += 1
            elif len(examples) < 4:
                examples.append((i % on.w, i // on.w, exp_max))
    unexplained = cnt - underflow
    return {
        "id": "no_false_black", "desc": "lit/penumbra: count(albedo.max > 1/255 and "
        "dd.rgb == 0) with the exact product max channel > 2^-24 must be 0 (no false "
        "black: dd = lightColor*albedo*ndl*vis > 0 whenever albedo, ndl, vis are all > 0 "
        "and the product is representable)",
        "status": "PASS" if unexplained == 0 else "FAIL",
        "false_black_pixels_raw": cnt,
        "fp16_subnormal_underflow_explained": underflow,
        "false_black_unexplained": unexplained,
        "examples_unexplained": examples,
        "justification": "spec §8 asks for count(albedo.max>1/255 AND dd.max==0); at "
        "double-grazing pixels (ndl and vis both ~1e-4) the exact product can be below "
        "2^-24, where exact zero is the correctly-rounded fp16 store under RTZ (< 2^-24) "
        "and under RNE (< 2^-25) -- those are reported as explained, everything above the "
        "subnormal threshold is a real false black",
    }


def check_direct_ge_dd(masks, on):
    direct, dd = on.get("direct"), on.get("direct_diffuse")
    qf, df = direct.floats, dd.floats
    viol = 0
    worst = None
    for i, m in enumerate(masks):
        if m == MASK_BG:
            continue
        j = 4 * i
        for c in range(3):
            d = df[j + c]
            excess = d - qf[j + c]
            if excess > 4e-3 * d + 1e-5:
                viol += 1
                if worst is None or excess > worst[0]:
                    worst = (excess, i % on.w, i // on.w, c, qf[j + c], d)
    return {
        "id": "direct_ge_direct_diffuse", "desc": "direct >= direct_diffuse - tol "
        "(direct adds a nonnegative specular term onto the diffuse term)",
        "status": "PASS" if viol == 0 else "FAIL", "violation_channels": viol,
        "worst_violation": worst, "tolerance": "4e-3*dd + 1e-5 per channel",
    }


def check_spec_structure(masks, on):
    """direct - dd must equal scalar*lightColor with 0 <= scalar <= 0.22*ndl*vis."""
    direct, dd = on.get("direct"), on.get("direct_diffuse")
    qf, df, da = direct.floats, dd.floats, direct.floats
    lc = on.light_color
    viol = 0
    checked = 0
    worst = None
    for i, m in enumerate(masks):
        if m not in (MASK_LIT, MASK_PEN):
            continue
        j = 4 * i
        dmax = max(qf[j], qf[j + 1], qf[j + 2])
        if dmax <= 1e-6:
            continue
        checked += 1
        q = [(qf[j + c] - df[j + c]) / lc[c] if lc[c] > 0 else 0.0 for c in range(3)]
        sref = max(q)
        tol = 4e-3 * dmax + 1e-5
        bad = False
        for c in range(3):
            if abs(q[c] - sref) > tol:
                bad = True
        ndl = da[j + 3]
        vis = df[j + 3]
        if sref > 0.22 * ndl * vis + tol:
            bad = True
        if bad:
            viol += 1
            if worst is None:
                worst = (i % on.w, i // on.w, q, sref, ndl, vis)
    return {
        "id": "direct_dd_spec_structure", "desc": "(direct - dd)/lightColor must be a "
        "single scalar s per pixel with 0 <= s <= 0.22*ndl*vis (proves direct = full "
        "direct radiance and dd = its diffuse-only part of the SAME pass)",
        "status": "PASS" if viol == 0 else "FAIL", "pixels_checked": checked,
        "violation_pixels": viol, "example": worst,
        "tolerance": "cross-channel spread <= 4e-3*direct_max + 1e-5 (fp16 cancellation)",
    }


def check_composite_bitexact(masks, on):
    """final_prepost bits == fp16_rne(f32(direct16)+f32(indirect16)) per channel."""
    final = on.get("final_prepost")
    direct = on.get("direct")
    indirect = on.get("indirect")
    if final is None or direct is None or indirect is None:
        return {"id": "composite_bitexact", "desc": "composite identity",
                "status": "SKIP", "reason": "missing direct/indirect/final_prepost"}
    if final.fmt != "rgba16f":
        # POST=0 path: final is BGRA8 UNorm. Compare against u8-quantized expectation.
        qf, inf_ = direct.floats, indirect.floats
        fr = final.raw
        bgra = final.fmt == "bgra8"
        viol = 0
        maxdev = 0.0
        for i, m in enumerate(masks):
            j = 4 * i
            for c in range(3):
                v = qf[j + c] + inf_[j + c]
                v = 0.0 if v < 0.0 else (1.0 if v > 1.0 else v)
                e8 = int(v * 255.0 + 0.5)
                a8 = fr[j + (2 - c if bgra else c)]
                dev = abs(a8 - e8)
                if dev > 1:
                    viol += 1
                if dev > maxdev:
                    maxdev = dev
        return {
            "id": "composite_bitexact", "desc": "composite identity (POST=0, 8-bit final: "
            "|u8(final) - round255(clamp(direct+indirect))| <= 1 LSB)",
            "status": "PASS" if viol == 0 else "FAIL", "violation_channels": viol,
            "max_lsb_dev": maxdev,
            "justification": "POST=0 writes BGRA8 UNorm; only +/-1 LSB of quantization is "
            "allowed (lower-confidence path: POST=1 RGBA16F is the bit-exact contract)",
        }
    fh_, ff = final.halves, final.floats
    qf, inf_ = direct.floats, indirect.floats
    n = on.w * on.h
    exact_rne = exact_rtz = 0
    ulp1_rne = ulp1_rtz = 0
    best_exact = best_ulp1 = best_gt1 = 0
    max_ulp = 0
    nan_cnt = 0
    alpha_bad = 0
    worst = None
    for i in range(n):
        j = 4 * i
        if fh_[j + 3] != HALF_ONE:
            alpha_bad += 1
        for c in range(3):
            s = f32(qf[j + c] + inf_[j + c])
            e_rne = fp16_bits(s)
            e_rtz = rtz_fp16_bits(s)
            got = fh_[j + c]
            if half_is_nan(e_rne) or half_is_nan(e_rtz) or half_is_nan(got):
                nan_cnt += 1
                continue
            d_rne = ulp_diff(e_rne, got)
            d_rtz = ulp_diff(e_rtz, got)
            if d_rne == 0:
                exact_rne += 1
            if d_rtz == 0:
                exact_rtz += 1
            if d_rne == 1:
                ulp1_rne += 1
            if d_rtz == 1:
                ulp1_rtz += 1
            d = d_rne if d_rne < d_rtz else d_rtz
            if d == 0:
                best_exact += 1
            elif d == 1:
                best_ulp1 += 1
            else:
                best_gt1 += 1
                if worst is None or d > max_ulp:
                    worst = (i % on.w, i // on.w, c, hex(e_rne), hex(e_rtz), hex(got), d)
            if d > max_ulp:
                max_ulp = d
    total = 3 * n - nan_cnt
    inferred = "rne" if exact_rne > exact_rtz else ("rtz" if exact_rtz > exact_rne else "tie")
    ok = total > 0 and (exact_rne == total or exact_rtz == total) and nan_cnt == 0
    return {
        "id": "composite_bitexact", "desc": "final_prepost bits == "
        "fp16(f32(direct16)+f32(indirect16)) per channel under round-half-even OR "
        "round-toward-zero; alpha == fp16(1.0)",
        "status": "PASS" if ok else "FAIL",
        "exact_fraction_rne": exact_rne / total if total else None,
        "exact_fraction_rtz": exact_rtz / total if total else None,
        "inferred_rt_write_rounding": inferred,
        "best_exact_fraction": best_exact / total if total else None,
        "ulp1_count_best": best_ulp1, "ulp_gt1_count_best": best_gt1,
        "max_ulp_best": max_ulp, "ulp1_rne": ulp1_rne, "ulp1_rtz": ulp1_rtz,
        "nan_count": nan_cnt, "alpha_not_one_count": alpha_bad,
        "example": worst,
        "justification": "spec §8: composite is expected bit-exact. The f32->f16 RT write "
        "rounding differs per platform (D3D12/NVIDIA = round-half-even, Mac MoltenVK = "
        "round-toward-zero), so the expectation is evaluated under BOTH modes and PASSes "
        "iff either mode matches 100% of channels; <=1 ulp counts are reported per mode "
        "and separately for the per-pixel better mode; NaN or alpha!=1 fails",
    }


def check_background_zeros(masks, on):
    names = ("direct", "direct_diffuse", "indirect", "final_prepost")
    per = {}
    total_bad = 0
    for name in names:
        buf = on.get(name)
        if buf is None:
            per[name] = "missing"
            continue
        is_final = name == "final_prepost"
        if buf.fmt != "rgba16f":
            r = buf.raw
            bgra = buf.fmt == "bgra8"
            bad = 0
            alpha_bad = 0
            for i, m in enumerate(masks):
                if m == MASK_BG:
                    j = 4 * i
                    for c in range(3):
                        if r[j + (2 - c if bgra else c)]:
                            bad += 1
                    if is_final and r[j + 3] != 255:
                        alpha_bad += 1
            per[name] = {"rgb_nonzero": bad, "alpha_not_255": alpha_bad}
            total_bad += bad + alpha_bad
            continue
        hv = buf.halves
        bad = 0
        for i, m in enumerate(masks):
            if m == MASK_BG:
                j = 4 * i
                for c in range(3):
                    if hv[j + c] & 0x7FFF:
                        bad += 1
                if is_final:
                    if hv[j + 3] != HALF_ONE:
                        bad += 1          # composite alpha contract: 1.0 everywhere
                else:
                    if hv[j + 3] & 0x7FFF:
                        bad += 1          # vec4(0) early-out covers alpha too
        per[name] = {"nonzero_component_count": bad}
        total_bad += bad
    return {
        "id": "background_zeros", "desc": "background (depth >= 1): "
        "direct/direct_diffuse/indirect rgb (and direct/dd alpha) == fp16 +-0; "
        "final_prepost rgb == 0 (alpha == 1)",
        "status": "PASS" if total_bad == 0 else "FAIL", "per_buffer": per,
        "total_nonzero": total_bad,
    }


def check_normal_disc(masks, on):
    normal = on.get("normal")
    if normal is None:
        return {"id": "normal_unit_disc", "desc": "GBuffer normal xy disc check",
                "status": "SKIP", "reason": "normal buffer missing"}
    nf = normal.floats
    tol = 6.1e-3
    viol = 0
    worst = None
    checked = 0
    for i, m in enumerate(masks):
        if m == MASK_BG:
            continue
        checked += 1
        x = nf[2 * i]
        y = nf[2 * i + 1]
        s = x * x + y * y
        if s > 1.0 + tol:
            viol += 1
            if worst is None or s > worst[0]:
                worst = (s, i % on.w, i // on.w, x, y)
    return {
        "id": "normal_unit_disc", "desc": "valid pixels: n.x^2 + n.y^2 <= 1 + tol "
        "(unit normal with z = sign*sqrt(1 - x^2 - y^2))",
        "status": "PASS" if viol == 0 else "FAIL", "pixels_checked": checked,
        "violations": viol, "worst": worst, "tolerance": tol,
        "justification": "fp16 xy of a unit vector: |x|,|y| <= 1 carry rel err <= 2^-11, "
        "so x^2+y^2 <= 1 + ~2e-3; 6.1e-3 = 1 fp16 ulp at 1.0 (2^-8... 2^-11*5) plus "
        "headroom; values above it cannot come from a unit normal",
    }


def probe_bounds(on):
    """(max c00, max |c1-1|+|c10|+|c11|) over all probe texels, or None."""
    probe = on.get("probe_sh")
    if probe is None:
        return None
    hv = probe.halves
    tbl = HALF_TABLE
    max_c00 = 0.0
    max_l1 = 0.0
    for t in range(len(hv) // 4):
        j = 4 * t
        c00 = tbl[hv[j]]
        if c00 > max_c00:
            max_c00 = c00
        l1 = abs(tbl[hv[j + 1]]) + abs(tbl[hv[j + 2]]) + abs(tbl[hv[j + 3]])
        if l1 > max_l1:
            max_l1 = l1
    return max_c00, max_l1


def check_indirect_sh_bound(masks, on, bounds):
    if bounds is None:
        return {"id": "indirect_sh_bound", "desc": "indirect <= albedo * SH bound",
                "status": "SKIP", "reason": "probe_sh buffer missing"}
    max_c00, max_l1 = bounds
    bound = 0.282095 * max_c00 + 0.325735 * max_l1
    indirect = on.get("indirect")
    albedo = on.get("albedo")
    if indirect is None or albedo is None:
        return {"id": "indirect_sh_bound", "desc": "indirect <= albedo * SH bound",
                "status": "SKIP", "reason": "indirect/albedo buffer missing"}
    inf_, ar = indirect.floats, albedo.raw
    viol = 0
    neg = 0
    worst = None
    for i, m in enumerate(masks):
        if m == MASK_BG:
            continue
        j = 4 * i
        a8 = 4 * i
        for c in range(3):
            v = inf_[j + c]
            if v < 0.0:
                neg += 1
            lim = (ar[a8 + c] / 255.0) * bound
            if v > lim + 4e-3 * lim + 1e-5:
                viol += 1
                if worst is None or v > worst[0]:
                    worst = (v, lim, i % on.w, i // on.w, c)
    return {
        "id": "indirect_sh_bound", "desc": "valid pixels: 0 <= indirect_c <= albedo_c * "
        "(0.282095*max_c00 + 0.325735*max|c1-1|+|c10|+|c11|) + fp16 tol (resolve output "
        "cannot exceed the global probe-energy envelope)",
        "status": "PASS" if viol == 0 and neg == 0 else "FAIL",
        "sh_bound": bound, "max_c00": max_c00, "max_l1_sum": max_l1,
        "bound_violations": viol, "negative_count": neg, "example": worst,
        "justification": "E_h(n) = 0.282095*c00 + 0.325735*(n.y*c1-1 + n.z*c10 + n.x*c11) "
        "<= 0.282095*max c00 + 0.325735*sum|max c_lm| over all probes (spec §3); "
        "trilinear mixes probes so the global max still bounds it; tol 4e-3 rel + 1e-5 "
        "abs covers fp16 storage of indirect and probes",
    }


def check_on_off_same_view(on, off):
    """Manifest level: GI-on and GI-off exports must share resolution, camera and light,
    otherwise the on/off cross-checks are meaningless."""
    mismatches = []
    unverifiable = []

    def cmp(name, a, b, rel=1e-6):
        if a is None or b is None:
            unverifiable.append(name)
            return
        va = a if isinstance(a, list) else [a]
        vb = b if isinstance(b, list) else [b]
        if len(va) != len(vb):
            mismatches.append({"field": name, "on": a, "off": b})
            return
        for x, y in zip(va, vb):
            x, y = float(x), float(y)
            if abs(x - y) > rel * max(1.0, abs(x), abs(y)):
                mismatches.append({"field": name, "on": a, "off": b})
                return

    ca, cb = on.m.get("camera") or {}, off.m.get("camera") or {}
    la, lb = on.m.get("light") or {}, off.m.get("light") or {}
    cmp("width", on.w, off.w)
    cmp("height", on.h, off.h)
    for k in ("position", "lookAt", "fov_deg", "near", "far", "inv_view_projection"):
        cmp("camera." + k, ca.get(k), cb.get(k))
    for k in ("dir_to_light", "intensity", "color"):
        cmp("light." + k, la.get(k), lb.get(k))
    return {
        "id": "on_off_same_view_and_light", "desc": "GI-on/GI-off manifests: identical "
        "resolution, camera (position/lookAt/fov/near/far/inv_view_projection) and light "
        "(dir/intensity/color) within 1e-6 relative -- precondition of every on/off "
        "cross-check",
        "status": "PASS" if not mismatches else "FAIL",
        "mismatches": mismatches, "unverifiable_fields": unverifiable,
        "exposure_on_off": [on.exposure, off.exposure],
        "note": "fields absent from either manifest are listed as unverifiable (not "
        "failing); gbuffer_bitmatch_on_off independently proves the same view",
    }


def check_gbuffer_bitmatch(on, off):
    per = {}
    bad_total = 0
    for name in ("depth", "albedo", "normal", "material"):
        a, b = on.get(name), off.get(name)
        if a is None or b is None:
            per[name] = "missing"
            continue
        if a.raw.tobytes() == b.raw.tobytes():
            per[name] = {"differing_bytes": 0, "differing_pixels": 0}
        else:
            ra, rb = a.raw, b.raw
            n4 = len(ra) // 4
            db = 0
            dp = 0
            for i in range(n4):
                j = 4 * i
                px = False
                for k in range(4):
                    if ra[j + k] != rb[j + k]:
                        db += 1
                        px = True
                if px:
                    dp += 1
            per[name] = {"differing_bytes": db, "differing_pixels": dp}
            bad_total += db
    return {
        "id": "gbuffer_bitmatch_on_off", "desc": "GI on/off same view+frame: depth/albedo/"
        "normal/material exports must be bit-identical (GBuffer path is outside the GI "
        "gate; deterministic harness)",
        "status": "PASS" if bad_total == 0 else "FAIL", "per_buffer": per,
    }


def check_off_occluded_ambient_exact(masks, on, off):
    final = off.get("final_prepost")
    albedo = on.get("albedo")
    if final is None or albedo is None:
        return {"id": "off_occluded_ambient_exact", "desc": "GI-off occluded == "
                "fp16(albedo*0.15) exactly", "status": "SKIP",
                "reason": "off final_prepost or albedo missing"}
    ar = albedo.raw
    kA = f32(0.15)
    if final.fmt != "rgba16f":
        r = final.raw
        bgra = final.fmt == "bgra8"
        bad = 0
        for i, m in enumerate(masks):
            if m != MASK_OCC:
                continue
            j = 4 * i
            for c in range(3):
                exp = f32((ar[j + c] / 255.0) * kA)
                e8 = int(exp * 255.0 + 0.5)
                a8 = r[j + (2 - c if bgra else c)]
                if abs(a8 - e8) > 1:
                    bad += 1
        return {
            "id": "off_occluded_ambient_exact", "desc": "GI-off occluded pixels == "
            "u8(albedo*0.15) within 1 LSB (POST=0 8-bit final)",
            "status": "PASS" if bad == 0 else "FAIL", "violations": bad,
        }
    fh_ = final.halves
    bad_rne = bad_rtz = 0
    exact_rne = exact_rtz = 0
    total = 0
    max_ulp = 0
    worst = None
    for i, m in enumerate(masks):
        if m != MASK_OCC:
            continue
        j = 4 * i
        for c in range(3):
            p = f32((ar[j + c] / 255.0) * kA)
            e_rne = fp16_bits(p)
            e_rtz = rtz_fp16_bits(p)
            got = fh_[j + c]
            total += 1
            d_rne = ulp_diff(e_rne, got)
            d_rtz = ulp_diff(e_rtz, got)
            if d_rne == 0:
                exact_rne += 1
            else:
                bad_rne += 1
            if d_rtz == 0:
                exact_rtz += 1
            else:
                bad_rtz += 1
            d = d_rne if d_rne < d_rtz else d_rtz
            if d > max_ulp:
                max_ulp = d
            if d > 0 and worst is None:
                worst = (i % on.w, i // on.w, c, hex(e_rne), hex(e_rtz), hex(got))
    inferred = "rne" if exact_rne > exact_rtz else ("rtz" if exact_rtz > exact_rne else "tie")
    ok = total > 0 and (exact_rne == total or exact_rtz == total)
    return {
        "id": "off_occluded_ambient_exact", "desc": "GI-off run, occluded pixels (mask from "
        "the ON run): final_off bits == fp16(f32(albedo)*f32(0.15)) exactly under RNE or "
        "RTZ -- independent cross-check of the occlusion mask + proves the M4 direct term "
        "vanishes there, leaving only kAmbient",
        "status": "PASS" if ok else "FAIL", "channels": total,
        "exact_rne": exact_rne, "exact_rtz": exact_rtz,
        "exact_fraction_rne": exact_rne / total if total else None,
        "exact_fraction_rtz": exact_rtz / total if total else None,
        "violations_rne": bad_rne, "violations_rtz": bad_rtz,
        "inferred_rt_write_rounding": inferred, "max_ulp_best": max_ulp,
        "example": worst,
        "justification": "spec §8: exact fp16 equality (dual rounding mode: RNE via "
        "struct '<e', RTZ via mantissa truncation; Mac MoltenVK writes RTZ); kAmbient = "
        "0.15f32, albedo = u8/255 (exact in f32), product rounded once to f32 then to fp16",
    }


def check_ambient_no_feedback(masks, on, off):
    final = off.get("final_prepost")
    direct = on.get("direct")
    albedo = on.get("albedo")
    if final is None or direct is None or albedo is None:
        return {"id": "ambient_no_feedback", "desc": "|final_off - (direct_on + "
                "0.15*albedo)| within 2 fp16 ulp", "status": "SKIP",
                "reason": "off final / on direct / albedo missing"}
    kA = f32(0.15)
    ar = albedo.raw
    qf = direct.floats
    if final.fmt != "rgba16f":
        r = final.raw
        bgra = final.fmt == "bgra8"
        viol = 0
        max_lsb = 0
        for i, m in enumerate(masks):
            if m == MASK_BG:
                continue
            j = 4 * i
            for c in range(3):
                amb = f32((ar[j + c] / 255.0) * kA)
                v = f32(qf[j + c] + amb)
                v = 0.0 if v < 0.0 else (1.0 if v > 1.0 else v)
                e8 = int(v * 255.0 + 0.5)
                a8 = r[j + (2 - c if bgra else c)]
                dev = abs(a8 - e8)
                if dev > max_lsb:
                    max_lsb = dev
                if dev > 1:
                    viol += 1
        return {
            "id": "ambient_no_feedback", "desc": "|final_off - u8(direct_on + 0.15*albedo)| "
            "<= 1 LSB (POST=0 8-bit final)",
            "status": "PASS" if viol == 0 else "FAIL", "violations": viol,
            "max_lsb_dev": max_lsb,
        }
    fh_ = final.halves
    viol_rne = viol_rtz = 0
    max_ulp = 0
    worst = None
    checked = 0
    for i, m in enumerate(masks):
        if m == MASK_BG:
            continue
        checked += 1
        j = 4 * i
        for c in range(3):
            amb = f32((ar[j + c] / 255.0) * kA)
            s = f32(qf[j + c] + amb)
            e_rne = fp16_bits(s)
            e_rtz = rtz_fp16_bits(s)
            got = fh_[j + c]
            d_rne = ulp_diff(e_rne, got)
            d_rtz = ulp_diff(e_rtz, got)
            if d_rne > 2:
                viol_rne += 1
            if d_rtz > 2:
                viol_rtz += 1
            d = d_rne if d_rne < d_rtz else d_rtz
            if d > max_ulp:
                max_ulp = d
            if d > 2 and (worst is None or d > worst[-1]):
                worst = (i % on.w, i // on.w, c, hex(e_rne), hex(e_rtz), hex(got), d)
    ok = checked > 0 and (viol_rne == 0 or viol_rtz == 0)
    return {
        "id": "ambient_no_feedback", "desc": "all non-background pixels: "
        "|final_off - fp16(direct_on + 0.15*albedo)| <= 2 fp16 ulp under RNE or RTZ "
        "(M4 = direct term + kAmbient only; the direct path is independent of the GI "
        "state -> no feedback)",
        "status": "PASS" if ok else "FAIL", "pixels_checked": checked,
        "violations_rne": viol_rne, "violations_rtz": viol_rtz,
        "max_ulp_best": max_ulp, "example": worst,
        "justification": "spec §8: within 1-2 fp16 ulp; the 2-ulp envelope absorbs one "
        "f32 rounding difference between the M4DeferredLight and M6DeferredDirect shader "
        "compilations (FMA contraction) after fp16 storage, evaluated under both RT-write "
        "rounding modes",
    }


# ----------------------------------------------------------------------------------------
# ROI / stats / contrast (informational)
# ----------------------------------------------------------------------------------------

ROIS = [
    ("upper_walls", 0.30, 0.70, 0.05, 0.35),
    ("floor", 0.30, 0.70, 0.55, 0.95),
    ("center", 0.40, 0.60, 0.35, 0.65),
    ("left_wall", 0.02, 0.28, 0.15, 0.85),
    ("right_wall", 0.72, 0.98, 0.15, 0.85),
]
ROI_SAMPLE_MAX = 16384   # deterministic stride sampling cap per ROI


def indirect_roi_stats(masks, on):
    indirect = on.get("indirect")
    out = {}
    if indirect is None:
        return out
    inf_ = indirect.floats
    w, h = on.w, on.h
    for name, x0, x1, y0, y1 in ROIS:
        xa, xb = int(x0 * w), max(int(x1 * w), int(x0 * w) + 1)
        ya, yb = int(y0 * h), max(int(y1 * h), int(y0 * h) + 1)
        px = []
        for y in range(ya, yb):
            for x in range(xa, xb):
                px.append(y * w + x)
        step = max(1, (len(px) + ROI_SAMPLE_MAX - 1) // ROI_SAMPLE_MAX)
        px = px[::step]
        chans = [[], [], []]
        mfrac = {MASK_NAMES[k]: 0 for k in MASK_NAMES}
        for i in px:
            mfrac[MASK_NAMES[masks[i]]] += 1
            j = 4 * i
            for c in range(3):
                chans[c].append(inf_[j + c])
        out[name] = {
            "sampled_pixels": len(px), "stride": step,
            "mask_fractions": {k: v / len(px) for k, v in mfrac.items()},
            "channels": {("rgb"[c]): summarize_sorted(chans[c]) for c in range(3)},
        }
    return out


def contrast_stats(masks, on, off):
    """Mean final over lit / over occluded, GI on vs off (+old analytic flood reference)."""
    res = {"reference_old_flood_gi_mean_windows": [1.07, 1.12, 1.21],
           "reference_source": "Windows evidence, analytic sky+flood bake (spec §8)"}
    fin = on.get("final_prepost")
    if fin is not None and fin.fmt == "rgba16f":
        s_occ = [0.0, 0.0, 0.0]
        s_lit = [0.0, 0.0, 0.0]
        n_occ = n_lit = 0
        ff = fin.floats
        for i, m in enumerate(masks):
            j = 4 * i
            if m == MASK_OCC:
                n_occ += 1
                for c in range(3):
                    s_occ[c] += ff[j + c]
            elif m == MASK_LIT:
                n_lit += 1
                for c in range(3):
                    s_lit[c] += ff[j + c]
        if n_occ and n_lit:
            mo = [v / n_occ for v in s_occ]
            ml = [v / n_lit for v in s_lit]
            res["final_on"] = {"mean_occluded": mo, "mean_lit": ml,
                               "lit_over_occluded": [ml[c] / mo[c] if mo[c] > 0 else None
                                                     for c in range(3)],
                               "occluded_n": n_occ, "lit_n": n_lit}
    if off is not None:
        foff = off.get("final_prepost")
        if foff is not None:
            s_occ = [0.0, 0.0, 0.0]
            s_lit = [0.0, 0.0, 0.0]
            n_occ = n_lit = 0
            fo = foff.floats if foff.fmt == "rgba16f" else None
            for i, m in enumerate(masks):
                if fo is not None:
                    vals = (fo[4 * i], fo[4 * i + 1], fo[4 * i + 2])
                else:
                    vals = foff.rgb_float(i)
                if m == MASK_OCC:
                    n_occ += 1
                    for c in range(3):
                        s_occ[c] += vals[c]
                elif m == MASK_LIT:
                    n_lit += 1
                    for c in range(3):
                        s_lit[c] += vals[c]
            if n_occ and n_lit:
                mo = [v / n_occ for v in s_occ]
                ml = [v / n_lit for v in s_lit]
                res["final_off"] = {"mean_occluded": mo, "mean_lit": ml,
                                    "lit_over_occluded": [ml[c] / mo[c] if mo[c] > 0 else None
                                                          for c in range(3)]}
    return res


def indirect_misc(masks, on):
    ind = on.get("indirect")
    if ind is None:
        return {}
    inf_ = ind.floats
    occ_pos = 0
    occ_n = 0
    maxv = [0.0, 0.0, 0.0]
    for i, m in enumerate(masks):
        j = 4 * i
        if m == MASK_OCC:
            occ_n += 1
            if inf_[j] > 0 or inf_[j + 1] > 0 or inf_[j + 2] > 0:
                occ_pos += 1
        for c in range(3):
            if inf_[j + c] > maxv[c]:
                maxv[c] = inf_[j + c]
    return {"occluded_pixels": occ_n, "occluded_with_indirect_gt0": occ_pos,
            "fraction": occ_pos / occ_n if occ_n else None,
            "global_max_per_channel": maxv,
            "note": "occluded pixels are GI receivers (single bounce); indirect > 0 there "
            "is expected once the grid has energy"}


def on_off_diff(masks, on, off):
    """Global |final_on - final_off| stats + x3 PNG payload."""
    fin_on = on.get("final_prepost")
    fin_off = off.get("final_prepost") if off else None
    if fin_on is None or fin_off is None:
        return None, None
    n = on.w * on.h
    fa = fin_on.floats if fin_on.fmt == "rgba16f" else None
    diffs = [0.0, 0.0, 0.0]
    total = 0.0
    cnt = 0
    viol = 0
    img = array("f", bytes(16 * n))      # RGBA floats per pixel
    for i in range(n):
        j = 4 * i
        va = (fa[j], fa[j + 1], fa[j + 2]) if fa is not None else fin_on.rgb_float(i)
        vb = fin_off.rgb_float(i)
        for c in range(3):
            d = abs(va[c] - vb[c])
            img[j + c] = d
            img[j + 3] = 1.0
            total += d
            cnt += 1
            if d > diffs[c]:
                diffs[c] = d
            mag = max(abs(va[c]), abs(vb[c]), HALF_MIN_NORMAL)
            if d > 4 * HALF_REL_ROUND * mag + 1e-7:
                viol += 1
    return ({"mean_abs_diff": total / cnt if cnt else None,
             "max_abs_diff_per_channel": diffs,
             "count_beyond_4ulp_equiv": viol,
             "tolerance_note": "descriptive; the pass/fail on/off check is "
             "ambient_no_feedback (2 ulp vs reconstructed M4 value)"}, img)


def write_frame_images(out_dir, on, off, masks, diff_img):
    files = []
    n = on.w * on.h
    for name in ("direct", "direct_diffuse", "indirect", "final_prepost"):
        buf = on.get(name)
        if buf is None:
            continue
        stem = "giexp_f%d_%s" % (on.frame, name)
        if buf.fmt == "rgba16f":
            write_pfm_rgb(os.path.join(out_dir, stem + ".pfm"), on.w, on.h, buf.floats)
            files.append((stem + ".pfm", "pfm", name + " (float32 RGB, bottom-to-top rows)"))
        png = ldr_from_buffer(buf, n, on.exposure)
        write_png(os.path.join(out_dir, stem + ".png"), on.w, on.h, png)
        files.append((stem + ".png", "png", name + " LDR (exposure x ACES fit, no gamma)"))
    if off is not None and off.get("final_prepost") is not None:
        stem = "giexp_f%d_off_final_prepost" % on.frame
        fbuf = off.get("final_prepost")
        if fbuf.fmt == "rgba16f":
            write_pfm_rgb(os.path.join(out_dir, stem + ".pfm"), off.w, off.h, fbuf.floats)
            files.append((stem + ".pfm", "pfm", "GI-off final (M4 output)"))
        png = ldr_from_buffer(fbuf, off.w * off.h, off.exposure)
        write_png(os.path.join(out_dir, stem + ".png"), off.w, off.h, png)
        files.append((stem + ".png", "png", "GI-off final (M4 output) LDR"))
    # mask overlay
    ov = bytearray(3 * n)
    for i, m in enumerate(masks):
        c = MASK_COLORS[m]
        j = 3 * i
        ov[j], ov[j + 1], ov[j + 2] = c
    write_png(os.path.join(out_dir, "mask_overlay.png"), on.w, on.h, ov)
    files.append(("mask_overlay.png", "png", "mask overlay"))
    # |on-off| x3
    if diff_img is not None:
        d = ldr_from_rgba_floats(diff_img, n, on.exposure, scale=3.0)
        write_png(os.path.join(out_dir, "diff_on_off_x3.png"), on.w, on.h, d)
        files.append(("diff_on_off_x3.png", "png", "3*|final_on - final_off| LDR"))
    return files


def downscale_nearest(rgb, w, h, tw, th):
    xs = [(i * w) // tw for i in range(tw)]
    ys = [(i * h) // th for i in range(th)]
    out = bytearray(3 * tw * th)
    for ty, sy in enumerate(ys):
        so = 3 * sy * w
        do = 3 * ty * tw
        for tx, sx in enumerate(xs):
            s = so + 3 * sx
            d = do + 3 * tx
            out[d] = rgb[s]
            out[d + 1] = rgb[s + 1]
            out[d + 2] = rgb[s + 2]
    return out


def write_contact_sheet(out_dir, on, off, masks):
    """2x3 labelled contact sheet same-view-gi-comparison-v2.png."""
    w, h = on.w, on.h
    n = w * h
    tw = min(480, w)
    th = max(1, (tw * h) // w)
    lab = 12
    mar = 4
    head = 14
    sw = 3 * tw + 4 * mar
    sh = head + 2 * (lab + th) + 3 * mar
    sheet = bytearray(3 * sw * sh)
    fill_rect(sheet, sw, sh, 0, 0, sw, sh, (12, 12, 12))
    tiles = []
    foff = off.get("final_prepost") if off else None
    tiles.append(("GI OFF FINAL (M4 OUTPUT)",
                  downscale_nearest(ldr_from_buffer(foff, off.w * off.h, off.exposure),
                                    off.w, off.h, tw, th) if foff else None))
    db = on.get("direct")
    tiles.append(("DIRECT ONLY (GI ON, FRAME %d)" % on.frame,
                  downscale_nearest(ldr_from_buffer(db, n, on.exposure), w, h, tw, th)
                  if db else None))
    ib = on.get("indirect")
    tiles.append(("INDIRECT ONLY (GI ON)",
                  downscale_nearest(ldr_from_buffer(ib, n, on.exposure), w, h, tw, th)
                  if ib else None))
    fb = on.get("final_prepost")
    tiles.append(("GI ON FINAL (DIRECT+INDIRECT)",
                  downscale_nearest(ldr_from_buffer(fb, n, on.exposure), w, h, tw, th)
                  if fb else None))
    ddb = on.get("direct_diffuse")
    tiles.append(("DIRECT_DIFFUSE = GI INPUT",
                  downscale_nearest(ldr_from_buffer(ddb, n, on.exposure), w, h, tw, th)
                  if ddb else None))
    ov = bytearray(3 * n)
    for i, m in enumerate(masks):
        c = MASK_COLORS[m]
        j = 3 * i
        ov[j], ov[j + 1], ov[j + 2] = c
    tiles.append(("MASK: GREEN=LIT AMBER=PEN RED=OCC BLUE=BG",
                  downscale_nearest(ov, w, h, tw, th)))
    off_name = ("OFF F%d" % off.frame) if off else "OFF NONE"
    draw_text(sheet, sw, sh, mar, 3,
              "SAME-VIEW GI COMPARISON V2 - ON F%d VS %s - %s" % (on.frame, off_name,
                                                                  TOOL_VERSION))
    for k, (label, img) in enumerate(tiles):
        row, col = divmod(k, 3)
        x = mar + col * (tw + mar)
        y = head + row * (lab + th + mar)
        fill_rect(sheet, sw, sh, x, y, x + tw, y + lab, (0, 0, 0))
        draw_text(sheet, sw, sh, x + 2, y + 3, label)
        yy = y + lab
        if img is None:
            fill_rect(sheet, sw, sh, x, yy, x + tw, yy + th, (40, 40, 40))
            draw_text(sheet, sw, sh, x + 4, yy + th // 2 - 3, "MISSING")
            continue
        for t in range(th):
            so = 3 * t * tw
            do = 3 * ((yy + t) * sw + x)
            sheet[do:do + 3 * tw] = img[so:so + 3 * tw]
    path = os.path.join(out_dir, "same-view-gi-comparison-v2.png")
    write_png(path, sw, sh, sheet)
    return path


def _gain_tag(g):
    g = float(g)
    return str(int(g)) if g.is_integer() else ("%g" % g)


def write_indirect_views(out_dir, on, masks, gain):
    """indirect_x<gain>.png (gain applied to LINEAR indirect before exposure x ACES, gain
    printed in the image) + 2x2 sheet indirect-structure.png:
      INDIRECT X1 | INDIRECT X<gain>
      E_H = INDIRECT/ALBEDO X<gain/4> | DIRECT_DIFFUSE = GI INPUT
    Returns [(file, kind, desc)]."""
    ind = on.get("indirect")
    albedo = on.get("albedo")
    if ind is None or ind.fmt != "rgba16f":
        return []
    w, h = on.w, on.h
    n = w * h
    files = []
    tag = _gain_tag(gain)
    amp = ldr_from_rgba_floats(ind.floats, n, on.exposure, scale=gain)
    labelled = bytearray(amp)
    label = "INDIRECT X%s (LINEAR GAIN, THEN EXPOSURE %s X ACES, NO GAMMA)" % (
        tag, _gain_tag(on.exposure))
    fill_rect(labelled, w, h, 0, 0, min(w, 6 * len(label) + 8), 13, (0, 0, 0))
    draw_text(labelled, w, h, 4, 3, label, (255, 230, 0))
    name = "indirect_x%s.png" % tag
    write_png(os.path.join(out_dir, name), w, h, labelled)
    files.append((name, "png", "indirect x%s: u8 = round(255*clamp(ACES(exposure*%s*x))), "
                  "gain label drawn in the top-left banner" % (tag, tag)))
    # irradiance view E_h = indirect / albedo (albedo-texture-free probe field structure)
    e_gain = gain / 4.0
    ef = array("f", bytes(16 * n))
    fi = ind.floats
    for i in range(n):
        j = 4 * i
        if masks[i] == MASK_BG:
            continue
        for c in range(3):
            a = albedo.raw[j + c] if albedo is not None else 0
            ef[j + c] = fi[j + c] * 255.0 / a if a else 0.0
    tw = min(640, w)
    th = max(1, (tw * h) // w)
    lab, mar, head = 12, 4, 14
    sw = 2 * tw + 3 * mar
    sh = head + 2 * (lab + th) + 3 * mar
    sheet = bytearray(3 * sw * sh)
    fill_rect(sheet, sw, sh, 0, 0, sw, sh, (12, 12, 12))
    dd = on.get("direct_diffuse")
    tiles = [
        ("INDIRECT X1 (SAME MAPPING AS FINAL)",
         ldr_from_rgba_floats(ind.floats, n, on.exposure)),
        ("INDIRECT X%s" % tag, amp),
        ("E_H = INDIRECT/ALBEDO X%s (PROBE IRRADIANCE)" % _gain_tag(e_gain),
         ldr_from_rgba_floats(ef, n, on.exposure, scale=e_gain)),
        ("DIRECT_DIFFUSE = GI INPUT (X1)",
         ldr_from_buffer(dd, n, on.exposure) if dd is not None else None),
    ]
    draw_text(sheet, sw, sh, mar, 3, "INDIRECT STRUCTURE - F%d - GAIN %s - EXPOSURE %s - %s"
              % (on.frame, tag, _gain_tag(on.exposure), TOOL_VERSION))
    for k, (title, img) in enumerate(tiles):
        row, col = divmod(k, 2)
        x = mar + col * (tw + mar)
        y = head + row * (lab + th + mar)
        fill_rect(sheet, sw, sh, x, y, x + tw, y + lab, (0, 0, 0))
        draw_text(sheet, sw, sh, x + 2, y + 3, title)
        yy = y + lab
        if img is None:
            fill_rect(sheet, sw, sh, x, yy, x + tw, yy + th, (40, 40, 40))
            draw_text(sheet, sw, sh, x + 4, yy + th // 2 - 3, "MISSING")
            continue
        small = downscale_nearest(img, w, h, tw, th)
        for t in range(th):
            so = 3 * t * tw
            do = 3 * ((yy + t) * sw + x)
            sheet[do:do + 3 * tw] = small[so:so + 3 * tw]
    write_png(os.path.join(out_dir, "indirect-structure.png"), sw, sh, sheet)
    files.append(("indirect-structure.png", "png", "2x2 sheet: indirect x1 | x%s / E_h = "
                  "indirect/albedo x%s | direct_diffuse (GI input)" % (tag, _gain_tag(e_gain))))
    return files


def cmd_frame(args):
    t_start = time.time()
    on = ExportFrame(args.on, args.frame)
    off = None
    if args.off:
        off_frame = args.off_frame if args.off_frame is not None else args.frame
        off = ExportFrame(args.off, off_frame)
        if (off.w, off.h) != (on.w, on.h):
            raise ExportError("off export is %dx%d, on is %dx%d (same view required)"
                              % (off.w, off.h, on.w, on.h))
    if not on.gi:
        raise ExportError("--on must point at a GI-on export (gi=false in manifest); "
                          "use --off for the GI-off run")
    os.makedirs(args.out, exist_ok=True)
    masks, counts = build_masks(on)

    checks = []
    checks.append(check_occluded_zero_exact(masks, on))
    checks.append(check_dd_identity(masks, on))
    checks.append(check_no_false_black(masks, on))
    checks.append(check_direct_ge_dd(masks, on))
    checks.append(check_spec_structure(masks, on))
    checks.append(check_composite_bitexact(masks, on))
    checks.append(check_background_zeros(masks, on))
    checks.append(check_normal_disc(masks, on))
    bounds = probe_bounds(on)
    checks.append(check_indirect_sh_bound(masks, on, bounds))
    if off is not None:
        checks.append(check_on_off_same_view(on, off))
        checks.append(check_gbuffer_bitmatch(on, off))
        checks.append(check_off_occluded_ambient_exact(masks, on, off))
        checks.append(check_ambient_no_feedback(masks, on, off))
    else:
        for cid in ("on_off_same_view_and_light", "gbuffer_bitmatch_on_off",
                    "off_occluded_ambient_exact", "ambient_no_feedback"):
            checks.append({"id": cid, "desc": "GI on/off cross-check", "status": "SKIP",
                           "reason": "--off not given"})

    diff_stats, diff_img = on_off_diff(masks, on, off)

    image_files = write_frame_images(args.out, on, off, masks, diff_img)
    sheet = write_contact_sheet(args.out, on, off, masks)
    image_files.append((os.path.basename(sheet), "png", "2x3 labelled contact sheet"))
    image_files.extend(write_indirect_views(args.out, on, masks, args.indirect_gain))

    failed = [c["id"] for c in checks if c["status"] == "FAIL"]
    overall = "PASS" if not failed else "FAIL"
    report = {
        "tool": TOOL_VERSION, "command": "frame", "argv": _ARGV[0],
        "generated_utc": time.strftime(
            "%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "elapsed_sec": round(time.time() - t_start, 3),
        "inputs": {"on": on.summary(), "off": off.summary() if off else None},
        "masks": {"definitions": {
            "valid": "depth < 1", "vis": "direct_diffuse.a (shadow)", "ndl": "direct.a",
            "occluded": "valid and (vis == 0 or ndl == 0)",
            "penumbra": "valid and 0 < vis < 1 and ndl > 0",
            "lit": "valid and vis == 1 and ndl > 0", "background": "not valid"},
            "counts": {MASK_NAMES[k]: v for k, v in counts.items()},
            "fractions": {MASK_NAMES[k]: v / (on.w * on.h) for k, v in counts.items()}},
        "checks": checks,
        "info": {
            "indirect_roi_stats": indirect_roi_stats(masks, on),
            "indirect_misc": indirect_misc(masks, on),
            "contrast": contrast_stats(masks, on, off),
            "on_off_abs_diff": diff_stats,
        },
        "images": [{"file": f, "kind": k, "desc": d} for f, k, d in image_files],
        "legend": {"mask_overlay_colors": {MASK_NAMES[k]: "#%02X%02X%02X" % v
                                           for k, v in MASK_COLORS.items() if k != MASK_OTHER},
                   "pfm": "float32 RGB, little-endian, header scale -1.000000, rows stored "
                          "bottom-to-top (first stored row = image bottom), alpha dropped",
                   "png": "u8 = round(255*clamp(ACES(exposure*x))); ACES fit "
                          "2.51/0.03/2.43/0.59/0.14; NO gamma (M3Post-identical)",
                   "indirect_gain": args.indirect_gain,
                   "irradiance_view_gain": args.indirect_gain / 4.0,
                   "indirect_views": "indirect_x<gain>.png = tonemap(exposure * gain * "
                                     "indirect) with the gain printed in the image; "
                                     "indirect-structure.png tile 3 = tonemap(exposure * "
                                     "gain/4 * indirect/albedo) (albedo-free E_h field)"},
        "thresholds": {
            "occluded_epsilon": {"value": "0 exactly (bits 0x0000/0x8000)",
                                 "source": "spec §8"},
            "identity": {"rel": 4e-3, "abs": 1e-5, "source": "spec §8"},
            "direct_ge_dd": {"rel": 4e-3, "abs": 1e-5, "source": "spec §8 'direct >= "
                             "direct_diffuse - tol' with the §8 tol family"},
            "composite": {"rule": "bit-exact expected; <=1 ulp tolerated and counted "
                          "separately; >1 ulp or NaN fails", "source": "spec §8 + task "
                          "requirement 'composite bit-exact expected, <=1 ulp counted "
                          "separately'"},
            "background": "exact fp16 +-0 rgb", "off_occluded_ambient": "exact fp16 bits",
            "on_off_global": "<= 2 fp16 ulp vs fp16(direct_on + 0.15*albedo)",
        },
        "overall": overall, "failed_checks": failed,
    }
    rpath = os.path.join(args.out, "report.json")
    with open(rpath, "w", encoding="utf-8") as fh:
        json.dump(report, fh, indent=1, sort_keys=True)
    print("[frame] %s  failed=%s  report=%s  (%.1fs)" %
          (overall, failed or "none", rpath, time.time() - t_start))
    return 0 if overall == "PASS" else 1


# ----------------------------------------------------------------------------------------
# ladder: [GI_MEAN] / [GI] RESET temporal fit
# ----------------------------------------------------------------------------------------

_GI_MEAN_RE = re.compile(
    r"\[GI_MEAN\]\s+frame\s+(\d+)\s+r=\s*(\S+)\s+g=\s*(\S+)\s+b=\s*(\S+)")
_GI_RESET_RE = re.compile(r"\[GI\]\s+frame\s+(\d+)\s+RESET")


def parse_gi_log(path):
    means = {}
    resets = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = _GI_MEAN_RE.search(line)
            if m:
                f = int(m.group(1))
                means[f] = tuple(float(m.group(k)) for k in (2, 3, 4))
                continue
            r = _GI_RESET_RE.search(line)
            if r:
                resets.append(int(r.group(1)))
    return means, sorted(set(resets))


def parse_segments(spec):
    segs = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        a, _, b = part.partition("-")
        segs.append((int(a), int(b)))
    if not segs:
        raise ExportError("empty --segments")
    return segs


def fit_segment(frames_vals, f0):
    """Least-squares T in M(f) = T*(1 - 0.9^(f-f0+1)) per channel.

    frames_vals: sorted [(f, (r,g,b))].  Returns per-channel dict with T, residuals.
    """
    chans = []
    ws = [1.0 - 0.9 ** (f - f0 + 1) for f, _ in frames_vals]
    denom = sum(w * w for w in ws)
    for c in range(3):
        ms = [v[c] for _, v in frames_vals]
        if denom > 0:
            T = sum(m * w for m, w in zip(ms, ws)) / denom
        else:
            T = 0.0
        preds = [T * w for w in ws]
        res = [m - p for m, p in zip(ms, preds)]
        rms = math.sqrt(sum(r * r for r in res) / len(res)) if res else 0.0
        maxres = max((abs(r) for r in res), default=0.0)
        ss_tot = sum((m - sum(ms) / len(ms)) ** 2 for m in ms) if ms else 0.0
        ss_res = sum(r * r for r in res)
        r2 = 1.0 - ss_res / ss_tot if ss_tot > 0 else (1.0 if ss_res == 0 else None)
        chans.append({"T": T, "rms_residual": rms, "max_abs_residual": maxres,
                      "r_squared": r2,
                      "residuals": {f: round(r, 9) for (f, _), r in zip(frames_vals, res)}})
    return chans


def _parse_pair_ratios(spec, nseg):
    """'2/1=0.386,3/2=2.588,3/1=1' -> [(i, j, r)]: expected T_i/T_j = r, 1-based segments."""
    pairs = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        lhs, _, rhs = part.partition("=")
        i, _, j = lhs.partition("/")
        try:
            i, j, r = int(i), int(j), float(rhs)
        except ValueError:
            raise ExportError("bad --pair-ratios entry %r (want i/j=ratio)" % part)
        if not (1 <= i <= nseg and 1 <= j <= nseg) or i == j or r <= 0:
            raise ExportError("bad --pair-ratios entry %r for %d segments" % (part, nseg))
        pairs.append((i, j, r))
    if not pairs:
        raise ExportError("empty --pair-ratios")
    return pairs


def cmd_ladder(args):
    means, resets = parse_gi_log(args.log)
    if not means:
        raise ExportError("no [GI_MEAN] lines parsed from %s" % args.log)
    segs = parse_segments(args.segments)
    tol = args.ratio_tol
    seg_reports = []
    seg_data = []          # per segment: None (no data) or {fv, f0, T, degenerate}
    for a, b in segs:
        fv = sorted((f, v) for f, v in means.items() if a <= f <= b)
        prior = [r for r in resets if r <= (fv[0][0] if fv else a)]
        f0 = prior[-1] if prior else 0
        assumed_f0 = not prior
        if not fv:
            seg_reports.append({"segment": [a, b], "status": "SKIP",
                                "reason": "no [GI_MEAN] frames in range"})
            seg_data.append(None)
            continue
        mags = [max(abs(v[c]) for c in range(3)) for _, v in fv]
        degenerate = max(mags) < 1e-12
        if degenerate:
            seg_reports.append({
                "segment": [a, b], "f0": f0, "f0_assumed": assumed_f0,
                "status": "degenerate: target 0",
                "note": "all [GI_MEAN] magnitudes < 1e-12 (e.g. fully shadowed view "
                "injects exact zeros); EMA fit is vacuous",
                "frames": [f for f, _ in fv], "T": [0.0, 0.0, 0.0]})
            seg_data.append({"fv": fv, "f0": f0, "T": (0.0, 0.0, 0.0), "degenerate": True})
            continue
        fits = fit_segment(fv, f0)
        T = tuple(f["T"] for f in fits)
        for fch in fits:
            aT = abs(fch["T"])
            fch["rms_residual_over_T"] = fch["rms_residual"] / aT if aT > 0 else None
            fch["max_abs_residual_over_T"] = fch["max_abs_residual"] / aT if aT > 0 else None
        seg_data.append({"fv": fv, "f0": f0, "T": T, "degenerate": False})
        # monotonicity per channel (dips below 0.5% of T are tolerated as measurement
        # noise; equality plateaus are allowed)
        mono_viol = 0
        worst_dip = 0.0
        for c in range(3):
            for k in range(1, len(fv)):
                dip = fv[k - 1][1][c] - fv[k][1][c]
                if dip > 0.005 * abs(T[c]) + 1e-12:
                    mono_viol += 1
                    worst_dip = max(worst_dip, dip)
        rms_bad = any(f["rms_residual"] > 0.05 * abs(f["T"]) + 1e-12 for f in fits)
        seg_reports.append({
            "segment": [a, b], "f0": f0, "f0_assumed": assumed_f0,
            "frames": [f for f, _ in fv],
            "values": {f: list(v) for f, v in fv},
            "T": list(T), "channels": {("rgb"[c]): fits[c] for c in range(3)},
            "model": "M(f) = T*(1 - 0.9^(f - f0 + 1)), f0 = segment's RESET frame "
                     "(0 if none precedes the segment)",
            "monotonicity_violations": mono_viol, "max_dip": worst_dip,
            "status": "PASS" if (mono_viol == 0 and not rms_bad) else "FAIL"})
    if args.pair_ratios:
        pairs = _parse_pair_ratios(args.pair_ratios, len(segs))
        pair_src = "--pair-ratios " + args.pair_ratios
    else:
        pairs = [(k + 1, k, args.intensity_ratio) for k in range(1, len(segs))]
        pair_src = ("--intensity-ratio %r for consecutive pairs (default 2.2/0.85 from the "
                    "m6 trajectory light keyframes)" % args.intensity_ratio)
    rows = []
    evaluated = 0
    ok = True
    for i, j, r in pairs:
        si, sj = seg_data[i - 1], seg_data[j - 1]
        row = {"pair": "%d/%d" % (i, j), "segments": [list(segs[i - 1]), list(segs[j - 1])],
               "expected_ratio": r}
        if si is None or sj is None or si["degenerate"] or sj["degenerate"] or \
                any(abs(si["T"][c]) < 1e-12 or abs(sj["T"][c]) < 1e-12 for c in range(3)):
            row["status"] = "SKIP"
            row["reason"] = "segment without [GI_MEAN] data or degenerate: target 0"
            rows.append(row)
            continue
        t_ratio = [si["T"][c] / sj["T"][c] for c in range(3)]
        t_dev = [abs(q / r - 1.0) for q in t_ratio]
        # model-free check: both segments restart the EMA at their RESET frame, so at equal
        # frames-since-reset k the ratio of [GI_MEAN] equals the injection ratio directly.
        ki = {f - si["f0"]: v for f, v in si["fv"]}
        kj = {f - sj["f0"]: v for f, v in sj["fv"]}
        common = sorted(set(ki) & set(kj))
        aligned = {"common_k": common, "per_channel": {}}
        al_dev = None
        for c in range(3):
            qs = [ki[k][c] / kj[k][c] for k in common if abs(kj[k][c]) > 1e-12]
            if qs:
                devs = [abs(q / r - 1.0) for q in qs]
                aligned["per_channel"]["rgb"[c]] = {
                    "n": len(qs), "mean": sum(qs) / len(qs), "min": min(qs), "max": max(qs),
                    "max_rel_dev": max(devs)}
                al_dev = max(devs) if al_dev is None else max(al_dev, max(devs))
        passed = max(t_dev) <= tol and (al_dev is None or al_dev <= tol)
        row.update({"T_ratio": dict(zip("rgb", t_ratio)), "T_ratio_max_rel_dev": max(t_dev),
                    "k_aligned": aligned, "k_aligned_max_rel_dev": al_dev,
                    "status": "PASS" if passed else "FAIL"})
        evaluated += 1
        ok = ok and passed
        rows.append(row)
    ratio_report = {"pairs": rows, "expected_source": pair_src, "tolerance_rel": tol,
                    "status": ("PASS" if ok else "FAIL") if evaluated else "SKIP",
                    "reason": None if evaluated else
                    "no evaluable pair (missing or degenerate segments)"}
    failed = []
    for s in seg_reports:
        if s.get("status") == "FAIL":
            failed.append("segment %s fit/monotonicity" % (s["segment"],))
    if ratio_report.get("status") == "FAIL":
        failed.append("segment T ratios vs expected")
    overall = "PASS" if not failed else "FAIL"
    report = {
        "tool": TOOL_VERSION, "command": "ladder", "argv": _ARGV[0],
        "log": os.path.abspath(args.log),
        "segments_spec": args.segments,
        "reset_frames": resets, "gi_mean_frames": sorted(means),
        "gi_mean_count": len(means),
        "segments": seg_reports, "ratio": ratio_report,
        "thresholds": {
            "fit_rms_over_T": {"value": 0.05, "justification": "EMA is exact arithmetic "
             "on a static injection; residuals come only from slow view/light drift and "
             "fp16 storage ripple (~2^-11 per store, EMA steady state ~10x -> ~5e-3); "
             "5% headroom"},
            "ratio_rel_dev": {"value": tol, "justification": "--ratio-tol (default 0.15): "
             "T scales linearly with light intensity; 0.15 is for the m6 trajectory, whose "
             "two segments sit at different resolutions (1280x720 vs resized 1600x900) and "
             "inject different stride-sample sets. For same-view same-resolution step runs "
             "use 0.02: only fp16 stores (<= 2^-10 relative per RTZ store) and the EMA "
             "truncation stall (<= ~10 ulp ~ 1%) separate the segments. Applied to both "
             "the fitted-T ratio and every k-aligned (equal frames-since-reset) ratio"},
            "monotonic_dip_over_T": {"value": 0.005, "justification": "M(f) increases "
             "strictly for T > 0 constant; dips beyond 0.5% of T indicate a missed RESET "
             "or non-static injection"},
            "degenerate": "segments with all |GI_MEAN| < 1e-12 (e.g. the all-shadow "
            "official m6 view) are reported 'degenerate: target 0' and neither fitted "
            "nor used for ratios",
        },
        "overall": overall, "failed_checks": failed,
    }
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(report, fh, indent=1, sort_keys=True)
    print("[ladder] %s  resets=%s  segments=%s  report=%s" %
          (overall, resets, args.segments, args.out))
    return 0 if overall == "PASS" else 1


# ----------------------------------------------------------------------------------------
# linearity A/B
# ----------------------------------------------------------------------------------------

def cmd_linearity(args):
    A = ExportFrame(args.a, args.frame)
    B = ExportFrame(args.b, args.frame)
    if (A.w, A.h) != (B.w, B.h):
        raise ExportError("A/B resolutions differ (%dx%d vs %dx%d)" %
                          (A.w, A.h, B.w, B.h))
    ratio = args.ratio
    checks = []
    n = A.w * A.h
    for name in ("direct_diffuse", "direct", "indirect", "final_prepost"):
        ba, bb = A.get(name), B.get(name)
        if ba is None or bb is None:
            checks.append({"id": "linearity_" + name, "desc": "per-pixel B/A ratio == "
                           "%g" % ratio, "status": "SKIP",
                           "reason": "buffer missing in A (%s) or B (%s)" %
                           (ba is None, bb is None)})
            continue
        if ba.fmt != bb.fmt:
            checks.append({"id": "linearity_" + name, "status": "SKIP",
                           "reason": "format mismatch %s vs %s" % (ba.fmt, bb.fmt)})
            continue
        tol_med, tol_p99 = 2e-3, 1e-2
        hists = [LinHist(ratio * 0.5, ratio * 1.5) for _ in range(3)]
        devh = [LogHist() for _ in range(3)]
        exact = [0, 0, 0]
        zero_bad = 0
        counted = 0
        if ba.fmt == "rgba16f":
            fa, fb = ba.floats, bb.floats
            for c in range(3):
                hc, dc = hists[c], devh[c]
                ex = 0
                for i in range(n):
                    j = 4 * i + c
                    va = fa[j]
                    vb = fb[j]
                    if va > 1e-3:
                        q = vb / va
                        hc.add(q)
                        dc.add(abs(q - ratio))
                        if vb == ratio * va:
                            ex += 1
                        counted += 1
                    elif va == 0.0 and vb != 0.0:
                        zero_bad += 1
                exact[c] = ex
        if counted == 0:
            checks.append({"id": "linearity_" + name, "status": "SKIP",
                           "reason": "no comparable float samples (8-bit final or all "
                           "below threshold)"})
            continue
        med = [h.percentile(50) for h in hists]
        p99 = [h.percentile(99) for h in hists]
        dev50 = [d.percentile(50) for d in devh]
        dev99 = [d.percentile(99) for d in devh]
        med_dev = max(v for v in dev50 if v is not None)
        p99_dev = max(v for v in dev99 if v is not None)
        swap_hint = None
        if abs(ratio - 1.0) > 0.05 and all(m is not None and abs(m * ratio - 1.0) < 0.01
                                           for m in med):
            swap_hint = ("median B/A ~= 1/ratio: --a/--b look swapped (tool convention: "
                         "--ratio = expected B/A)")
        ok = med_dev <= tol_med and p99_dev <= tol_p99 and zero_bad == 0
        checks.append({
            "id": "linearity_" + name, "desc": "pixels with A > 1e-3: |B/A - %g| median "
            "<= %g and p99 <= %g; pixels with A == 0 must have B == 0" %
            (ratio, tol_med, tol_p99),
            "status": "PASS" if ok else "FAIL",
            "per_channel": {"rgb"[c]: {"median_ratio": med[c], "p99_ratio": p99[c],
                                        "min_ratio": hists[c].minv,
                                        "max_ratio": hists[c].maxv,
                                        "samples": hists[c].n,
                                        "exact_ratio_fraction": (exact[c] / hists[c].n
                                                                 if hists[c].n else None),
                                        "dev_p50": dev50[c], "dev_p99": dev99[c],
                                        "dev_max": devh[c].maxv}
                            for c in range(3)},
            "zero_consistency_violations": zero_bad,
            "max_dev_p50": med_dev, "max_dev_p99": p99_dev, "swap_hint": swap_hint,
            "thresholds": {"dev_p50": tol_med, "dev_p99": tol_p99, "a_min": 1e-3},
            "justification": "halving intensity halves lightColor exactly in f32 (power-of-"
            "two scaling), so every downstream product/sum and the fp16 store (RTZ or RNE) "
            "commute with x0.5 in the normal range: B/A = 0.5 exactly (exact_ratio_fraction "
            "~1). Deviations can only come from fp16-subnormal stores or non-linear terms. "
            "Tolerances on |B/A - ratio| (ratio units): p50 2e-3 (single-store path), p99 "
            "1e-2 (accumulated inject/EMA/resolve chain); exact zero consistency (a "
            "constant/ambient/sky term in B would light A==0 pixels). Dev quantiles come "
            "from a log histogram whose underflow bin holds exact zeros (and devs < 1e-9), "
            "so they are exactly 0 when most samples match exactly; dev_max is exact",
        })
    pa, pb = A.get("probe_sh"), B.get("probe_sh")
    if pa is None or pb is None:
        checks.append({"id": "linearity_probe_c00", "desc": "probe c00 B/A ratio",
                       "status": "SKIP", "reason": "probe_sh missing"})
    else:
        ha, hb = pa.halves, pb.halves
        tbl = HALF_TABLE
        hist = LinHist(ratio * 0.5, ratio * 1.5)
        devh = LogHist()
        exact = 0
        zero_bad = 0
        for t in range(len(ha) // 4):
            va = tbl[ha[4 * t]]
            vb = tbl[hb[4 * t]]
            if abs(va) > 1e-6:
                q = vb / va
                hist.add(q)
                devh.add(abs(q - ratio))
                if vb == ratio * va:
                    exact += 1
            elif va == 0.0 and vb != 0.0:
                zero_bad += 1
        med, p99 = hist.percentile(50), hist.percentile(99)
        med_dev, p99_dev = devh.percentile(50), devh.percentile(99)
        tol_med, tol_p99 = 1e-2, 5e-2
        ok = (med_dev is not None and med_dev <= tol_med and
              (p99_dev is not None and p99_dev <= tol_p99) and zero_bad == 0)
        checks.append({
            "id": "linearity_probe_c00", "desc": "probe_sh c00 texels with |A| > 1e-6: "
            "|B/A - %g| median <= %g, p99 <= %g" % (ratio, tol_med, tol_p99),
            "status": "PASS" if ok else "FAIL",
            "median_ratio": med, "p99_ratio": p99, "min_ratio": hist.minv,
            "max_ratio": hist.maxv, "samples": hist.n,
            "exact_ratio_fraction": exact / hist.n if hist.n else None,
            "dev_p50": med_dev, "dev_p99": p99_dev, "dev_max": devh.maxv,
            "zero_consistency_violations": zero_bad,
            "justification": "c00 passes through inject (f32 sums of fp16 inputs) + EMA "
            "store ripple (steady state ~10x 2^-11) -> wider tolerances than single-store "
            "buffers; exact zero consistency required (injection of exact-zero pixels "
            "contributes exact zero at any intensity)",
        })
    gi_mean = {"status": "SKIP", "reason": "--log-a/--log-b not given"}
    if args.log_a and args.log_b:
        ma, _ra = parse_gi_log(args.log_a)
        mb, _rb = parse_gi_log(args.log_b)
        common = sorted(set(ma) & set(mb))
        if common:
            tail = common[-5:]
            rows = {}
            ok = True
            for c, chn in enumerate("rgb"):
                rs = [mb[f][c] / ma[f][c] for f in tail if abs(ma[f][c]) > 1e-9]
                allr = [mb[f][c] / ma[f][c] for f in common if abs(ma[f][c]) > 1e-9]
                if rs:
                    r = sum(rs) / len(rs)
                    all_dev = max(abs(q / ratio - 1.0) for q in allr)
                    rows[chn] = {"mean_ratio_last5": r, "rel_dev": abs(r / ratio - 1.0),
                                 "all_common_frames": len(allr),
                                 "min_ratio_all": min(allr), "max_ratio_all": max(allr),
                                 "max_rel_dev_all": all_dev}
                    ok = ok and abs(r / ratio - 1.0) <= 0.05 and all_dev <= 0.05
            gi_mean = {"status": "PASS" if ok and rows else "FAIL",
                       "per_channel": rows, "frames_used": tail,
                       "threshold_rel_dev": 0.05,
                       "note": "printed with 6 significant digits by the harness; last-5 "
                               "mean and every common frame must be within 5%"}
        else:
            gi_mean = {"status": "SKIP", "reason": "no common [GI_MEAN] frames"}
    checks.append({"id": "linearity_gi_mean", "desc": "log [GI_MEAN] ratio (last 5 common "
                   "frames, and every common frame) == %g within 5%%" % ratio, **gi_mean})
    failed = [c["id"] for c in checks if c["status"] == "FAIL"]
    overall = "PASS" if not failed else "FAIL"
    report = {
        "tool": TOOL_VERSION, "command": "linearity", "argv": _ARGV[0], "frame": args.frame,
        "a": A.summary(), "b": B.summary(), "ratio": ratio,
        "ratio_convention": "--ratio is the expected B/A (B = --b, A = --a)",
        "checks": checks, "overall": overall, "failed_checks": failed,
        "threshold_note": "all thresholds duplicated per-check with justifications",
    }
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(report, fh, indent=1, sort_keys=True)
    print("[linearity] %s  failed=%s  report=%s" % (overall, failed or "none", args.out))
    return 0 if overall == "PASS" else 1


# ----------------------------------------------------------------------------------------
# synthetic export generator (selftest): RTZ-emulating fake GPU with planted defects
# ----------------------------------------------------------------------------------------

_ALIGN = struct.Struct("<I").pack
_PACK4H = struct.Struct("<4H")
_PACK2H = struct.Struct("<2H")
_PACKF = struct.Struct("<f")


def _align256(x):
    return (x + 255) // 256 * 256


def _store_h(x):
    """RTZ store to fp16 bits (the synthetic GPU writes like Mac MoltenVK)."""
    return rtz_fp16_bits(x)


def _pad_rows(tight, row, pitch, nrows):
    if pitch == row:
        return bytes(tight)
    out = bytearray(pitch * nrows)
    for r in range(nrows):
        out[r * pitch:r * pitch + row] = tight[r * row:(r + 1) * row]
    return bytes(out)


def _write_export(d, frame, gi, post, W, H, intensity, bufs, exposure=2.0):
    """Write a spec §6.5-shaped export dir. bufs: name -> (fmt, bpp, w, h, depth, payload,
    semantics, units). Returns manifest dict."""
    os.makedirs(d, exist_ok=True)
    stem = "giexp_f%d" % frame
    manifest = {
        "schema": EXPORT_SCHEMA, "frame": frame, "platform": "synthetic-rtz/1",
        "gi": gi, "post": post, "exposure": exposure, "width": W, "height": H,
        "camera": {"position": [0.0, 5.0, 0.0], "lookAt": [-10.0, 1.5, 0.0],
                   "fov_deg": 60.0, "near": 0.1, "far": 200,
                   "projection_y_row_negated": True},
        "light": {"dir_to_light": [0.85, 1.4, 0.35], "intensity": intensity,
                  "color": [1.0, 0.98, 0.92]},
        "shadow": {"map_size": 2048, "pcf": "3x3", "compare": "LessEqual",
                   "ortho_half_extent": 40.0, "near": 20.0, "far": 150.0},
    }
    if gi:
        manifest["gi_grid"] = {
            "dims": [8, 8, 8], "box_min": [-40.0, -10.0, -30.0],
            "box_max": [40.0, 30.0, 50.0], "cell": [10.0, 5.0, 10.0],
            "normal_bias": 1.25, "sh_layout": "synthetic", "ema": "hist*0.9+new*0.1",
            "reset_this_frame": False, "frames_since_reset": frame - 30}
    mb = {}
    tight = _LAYOUT[0] == "tight"
    for name, (fmt, bpp, w, h, depth, payload, semantics, units) in bufs.items():
        row = w * bpp
        readback_pitch = 256 if depth else _align256(row)
        pitch = row if tight else readback_pitch
        nrows = h * (depth if depth else 1)
        data = _pad_rows(payload, row, pitch, nrows)
        with open(os.path.join(d, stem + "_" + name + ".bin"), "wb") as fh:
            fh.write(data)
        entry = {"file": stem + "_" + name + ".bin", "format": fmt,
                 "bytes_per_pixel": bpp, "row_pitch": pitch, "width": w, "height": h}
        if tight:
            entry["readback_row_pitch"] = readback_pitch     # informational (harness v2)
        if depth:
            entry["depth"] = depth
        entry["semantics"] = semantics
        entry["units"] = units
        mb[name] = entry
    manifest["buffers"] = mb
    with open(os.path.join(d, stem + ".json"), "w", encoding="utf-8") as fh:
        json.dump(manifest, fh, indent=1)
    return manifest


def _gen_base(W, H):
    """Deterministic per-pixel scene: albedo u8, band map, ndl/vis true values, depth,
    normal xy fp16 + zsign, material.  Bands: bg 15%, occluded 15%, penumbra 15%, lit."""
    bg_h = max(2, int(H * 0.15))
    occ_h = bg_h + max(2, int(H * 0.15))
    pen_h = occ_h + max(2, int(H * 0.15))
    n = W * H
    albedo = bytearray(4 * n)
    material = bytearray(4 * n)
    normal = bytearray(4 * n)
    depth = bytearray(4 * n)
    band = bytearray(n)
    ndl_t = [0.0] * n
    vis_t = [0.0] * n
    for y in range(H):
        for x in range(W):
            i = y * W + x
            a8 = (8 + (x * 3) % 240, 8 + (y * 5) % 240, 8 + ((x + y) * 7) % 240)
            albedo[4 * i] = a8[0]
            albedo[4 * i + 1] = a8[1]
            albedo[4 * i + 2] = a8[2]
            albedo[4 * i + 3] = 255
            if y < bg_h:
                band[i] = 0
                d = 1.0
            else:
                d = 0.25 + 0.5 * ((x * 11 + y * 3) % 23) / 23.0
                if y < occ_h:
                    band[i] = 1
                    vis_t[i] = 0.0
                    ndl_t[i] = 0.0 if x % 11 == 0 else 0.05 + 0.9 * (((x * 13 + y * 7) % 31) / 30.0)
                elif y < pen_h:
                    band[i] = 2
                    vis_t[i] = (x % 9 + 1) / 10.0
                    ndl_t[i] = 0.05 + 0.9 * (((x * 13 + y * 7) % 31) / 30.0)
                else:
                    band[i] = 3
                    vis_t[i] = 1.0
                    ndl_t[i] = 0.05 + 0.9 * (((x * 13 + y * 7) % 31) / 30.0)
                if x == 30 and y >= pen_h:      # fp16 subnormal ndl (lit band, col 30)
                    ndl_t[i] = 1.0e-5
            _PACKF.pack_into(depth, 4 * i, d)
            # normal: normalize((x-cx, y-cy, 40)); xy stored fp16 RTZ, z sign in material.a
            cx, cy = (W - 1) * 0.5, (H - 1) * 0.5
            nx, ny, nz = x - cx, y - cy, 40.0
            inv = 1.0 / math.sqrt(nx * nx + ny * ny + nz * nz)
            nx, ny, nz = nx * inv, ny * inv, nz * inv
            _PACK2H.pack_into(normal, 4 * i, _store_h(f32(nx)), _store_h(f32(ny)))
            material[4 * i] = 255
            material[4 * i + 1] = 128
            material[4 * i + 2] = (x * 255) // W
            material[4 * i + 3] = 255 if nz >= 0 else 0
    return {"band": band, "albedo": albedo, "material": material, "normal": normal,
            "depth": depth, "ndl_t": ndl_t, "vis_t": vis_t,
            "bands": (bg_h, occ_h, pen_h)}


def _gen_probe(intensity_scale=1.0, subnormal=True):
    """8x8x24 SH-L1 probe payload: tight rows of 8 texels x 8B; returns bytes + bounds."""
    w, h, d = 8, 8, 24
    row = w * 8
    data = bytearray(row * h * d)
    for z in range(d):
        for y in range(h):
            for x in range(w):
                t = (z * h + y) * w + x
                c00 = (0.05 + 0.03 * ((x + z) % 5)) * intensity_scale
                c1m = 0.004 * ((x + 2 * y) % 7) / 7.0 * intensity_scale
                c10 = 0.003 * ((2 * x + y) % 5) / 5.0 * intensity_scale
                c11 = 0.002 * ((x - y) % 3) / 3.0 * intensity_scale
                if subnormal and x == 1 and y == 1 and z == 1:
                    c00 = 1.5e-7          # fp16 subnormal (RTZ -> 1.19e-7)
                _PACK4H.pack_into(data, 8 * t, _store_h(c00), _store_h(c1m),
                                  _store_h(c10), _store_h(c11))
    return bytes(data)


def _gen_on(W, H, frame, intensity=2.2, defects=(), probe_scale=1.0):
    base = _gen_base(W, H)
    lc = [f32(f32(c) * f32(intensity)) for c in (1.0, 0.98, 0.92)]
    e_scale = intensity / 2.2
    band = base["band"]
    albedo = base["albedo"]
    n = W * H
    direct = bytearray(8 * n)
    ddiff = bytearray(8 * n)
    indirect = bytearray(8 * n)
    final = bytearray(8 * n)
    tbl = HALF_TABLE
    defs = set(defects)
    probe_payload = _gen_probe(probe_scale)
    for i in range(n):
        x, y = i % W, i // W
        j = 8 * i
        if band[i] == 0:
            zb = 0x8000 if x % 7 == 0 else 0x0000          # +-0 accepted
            _PACK4H.pack_into(direct, j, 0, zb, 0, 0)
            _PACK4H.pack_into(ddiff, j, 0, 0, 0, 0)
            _PACK4H.pack_into(indirect, j, 0, 0, 0, 0)
            _PACK4H.pack_into(final, j, 0, 0, 0, HALF_ONE)
            continue
        a8 = (albedo[4 * i] / 255.0, albedo[4 * i + 1] / 255.0, albedo[4 * i + 2] / 255.0)
        ndl_b = _store_h(base["ndl_t"][i])
        vis_b = _store_h(base["vis_t"][i])
        # shader-faithful: direct = lc*(albedo*ndl + spec)*shadow, spec NOT albedo-modulated
        s = 0.1 * base["ndl_t"][i] * base["vis_t"][i] * (0.2 + 0.3 * ((x % 5) / 4.0))
        ddc = [_store_h(a8[c] * lc[c] * base["ndl_t"][i] * base["vis_t"][i]) for c in range(3)]
        drc = [_store_h(lc[c] * (a8[c] * base["ndl_t"][i] * base["vis_t"][i] + s))
               for c in range(3)]
        E = [(0.005 + 0.015 * (((x + y) % 7) / 6.0)) * e_scale] * 3
        ind = [_store_h(a8[c] * E[c]) for c in range(3)]
        # ---- planted defects that must be caught (frame-level) ----
        if "occ_nonzero" in defs and (x, y) == (7, base["bands"][1] - 9):
            drc[1] = 0x3555                                     # ~0.33 in occluded
        if "false_black" in defs and (x, y) == (9, base["bands"][2] + 4):
            ddc = [0, 0, 0]
        if "direct_lt_dd" in defs and (x, y) == (15, base["bands"][2] + 6):
            drc[0] = _store_h(tbl[ddc[0]] * 0.5)
        if "indirect_bound" in defs and (x, y) == (17, base["bands"][2] + 14):
            ind[0] = _store_h(a8[0] * 3.0)
        _PACK4H.pack_into(direct, j, drc[0], drc[1], drc[2], ndl_b)
        _PACK4H.pack_into(ddiff, j, ddc[0], ddc[1], ddc[2], vis_b)
        _PACK4H.pack_into(indirect, j, ind[0], ind[1], ind[2], HALF_ONE)
        fc = [_store_h(f32(tbl[drc[c]] + tbl[ind[c]])) for c in range(3)]
        if "comp_ulp1" in defs and (x, y) == (11, base["bands"][2] + 4):
            fc[0] += 1                                          # exactly 1 ulp
        if "comp_ulp3" in defs and (x, y) == (11, base["bands"][2] + 5):
            fc[0] += 3                                          # 3 ulp -> hard fail
        if "bg_nonzero" in defs and (x, y) == (13, 3):
            pass
        _PACK4H.pack_into(final, j, fc[0], fc[1], fc[2], HALF_ONE)
    if "bg_nonzero" in defs:
        j = 8 * (3 * W + 13)
        _PACK4H.pack_into(final, j, 0, _store_h(0.01), 0, HALF_ONE)
    if "normal_bad" in defs:
        y = base["bands"][1] - base["bands"][0] + 6
        _PACK2H.pack_into(base["normal"], 4 * (y * W + 19), _store_h(0.9), _store_h(0.9))
    bufs = {
        "direct": ("RGBA16_Float", 8, W, H, 0, bytes(direct), "rgb=direct only; a=ndl",
                   "harness linear HDR"),
        "direct_diffuse": ("RGBA16_Float", 8, W, H, 0, bytes(ddiff),
                           "rgb=lightColor*albedo*ndl*shadow; a=shadow",
                           "harness linear HDR"),
        "indirect": ("RGBA16_Float", 8, W, H, 0, bytes(indirect), "rgb=albedo*E_h; a=1",
                     "harness linear HDR"),
        "final_prepost": ("RGBA16_Float", 8, W, H, 0, bytes(final), "rgb=direct+indirect",
                          "harness linear HDR"),
        "depth": ("D32_Float", 4, W, H, 0, bytes(base["depth"]), "depth [0,1]", "ndc depth"),
        "albedo": ("RGBA8_UNorm", 4, W, H, 0, bytes(base["albedo"]), "albedo", "unorm8"),
        "normal": ("RG16_Float", 4, W, H, 0, bytes(base["normal"]), "normal xy", "unit vec"),
        "material": ("RGBA8_UNorm", 4, W, H, 0, bytes(base["material"]), "material",
                     "unorm8"),
        "probe_sh": ("RGBA16_Float", 8, 8, 8, 24, probe_payload, "SH-L1 probes",
                     "radiance x steradian"),
    }
    _write_export(_ON_DIR[0], frame, True, True, W, H, intensity, bufs)
    return base


_ON_DIR = [None]     # set by selftest to redirect the generator output
_LAYOUT = ["padded"]  # "padded" = harness v1 (row_pitch = align256), "tight" = harness v2


def _gen_off(W, H, frame, intensity=2.2, defects=(), post=True, on_dir_base=None):
    """GI-off export: M4 output = 0.15*albedo + direct.  GBuffer inputs bit-identical to
    the matching _gen_on call (same deterministic base)."""
    base = on_dir_base if on_dir_base is not None else _gen_base(W, H)
    lc = [f32(f32(c) * f32(intensity)) for c in (1.0, 0.98, 0.92)]
    band = base["band"]
    albedo = base["albedo"]
    n = W * H
    tbl = HALF_TABLE
    kA = f32(0.15)
    defs = set(defects)
    final = bytearray(8 * n if post else 4 * n)
    for i in range(n):
        x, y = i % W, i // W
        if band[i] == 0:
            vals = (0.0, 0.0, 0.0)
        else:
            a8 = (albedo[4 * i] / 255.0, albedo[4 * i + 1] / 255.0,
                  albedo[4 * i + 2] / 255.0)
            # the M4 direct term == the ON run's direct (recomputed identically)
            ndl_b = _store_h(base["ndl_t"][i])
            vis_b = _store_h(base["vis_t"][i])
            s = 0.1 * base["ndl_t"][i] * base["vis_t"][i] * (0.2 + 0.3 * ((x % 5) / 4.0))
            vals = []
            for c in range(3):
                direct_v = tbl[_store_h(lc[c] * (a8[c] * base["ndl_t"][i] *
                                                 base["vis_t"][i] + s))]
                amb = f32(a8[c] * kA)
                vals.append(f32(direct_v + amb))
        if post:
            fc = [_store_h(vals[c]) for c in range(3)]
            if "off_amb_ulp1" in defs and (x, y) == (5, base["bands"][1] - 9):
                fc[1] += 1
            if "off_amb_ulp4" in defs and (x, y) == (6, base["bands"][1] - 9):
                fc[1] += 4
            _PACK4H.pack_into(final, 8 * i, fc[0], fc[1], fc[2], HALF_ONE)
        else:
            qc = []
            for c in range(3):
                v = vals[c]
                v = 0.0 if v < 0.0 else (1.0 if v > 1.0 else v)
                qc.append(int(v * 255.0 + 0.5))
            final[4 * i] = qc[2]            # BGRA8 byte order (readback layout)
            final[4 * i + 1] = qc[1]
            final[4 * i + 2] = qc[0]
            final[4 * i + 3] = 255
    albedo_payload = bytearray(base["albedo"])
    if "off_albedo" in defs:
        albedo_payload[4 * (3 * W + 3) + 1] ^= 0x40
    fmt = "RGBA16_Float" if post else "BGRA8_UNorm"
    bpp = 8 if post else 4
    bufs = {
        "final_prepost": (fmt, bpp, W, H, 0, bytes(final),
                          "M4 output (GI off)", "harness linear HDR"),
        "depth": ("D32_Float", 4, W, H, 0, bytes(base["depth"]), "depth", "ndc depth"),
        "albedo": ("RGBA8_UNorm", 4, W, H, 0, bytes(albedo_payload), "albedo", "unorm8"),
        "normal": ("RG16_Float", 4, W, H, 0, bytes(base["normal"]), "normal xy", "unit vec"),
        "material": ("RGBA8_UNorm", 4, W, H, 0, bytes(base["material"]), "material",
                     "unorm8"),
    }
    _write_export(_OFF_DIR[0], frame, False, post, W, H, intensity, bufs)


_OFF_DIR = [None]


def _gen_linearity_b(W, H, frame, defects=()):
    """B = clean ON generation at intensity 1.1 (exactly half of 2.2 in f32) + plants."""
    return _gen_on(W, H, frame, intensity=1.1, defects=defects, probe_scale=0.5)


# ----------------------------------------------------------------------------------------
# selftest
# ----------------------------------------------------------------------------------------

def cmd_selftest(args):
    failures = []
    passes = [0]
    timing = {}
    my_argv = list(_ARGV[0] or [])

    def check(cond, msg):
        print("  %s  %s" % ("ok  " if cond else "FAIL", msg))
        if cond:
            passes[0] += 1
        else:
            failures.append(msg)

    t0 = time.time()
    print("[selftest] unit tests")
    # half table vs struct '<e' (independent decoder) over all 65536 patterns
    bad = 0
    for h in range(65536):
        a = HALF_TABLE[h]
        b = _E16.unpack(h.to_bytes(2, "little"))[0]
        if a != b and not (math.isnan(a) and math.isnan(b)):
            bad += 1
    check(bad == 0, "half table == struct '<e' decode for all 65536 patterns "
          "(subnormals, +-0, inf, nan); mismatches=%d" % bad)
    # RTZ: idempotent on every finite fp16-exact value; correct ties/overflow/subnormals
    # (half +-inf overflows to max finite 0x7BFF under RTZ, the documented exception)
    bad = sum(1 for h in range(65536)
              if not half_is_nan(h) and (h & 0x7FFF) != 0x7C00
              and rtz_fp16_bits(HALF_TABLE[h]) != h)
    check(bad == 0, "RTZ(f32(fp16 value)) == same bits for all finite patterns; "
          "mismatches=%d" % bad)
    check(rtz_fp16_bits(float("inf")) == 0x7BFF,
          "RTZ(+inf) -> max finite 0x7BFF (IEEE alternate-rounding overflow)")
    tie = 1.0 + 2.0 ** -10 + 2.0 ** -11
    check(fp16_bits(tie) == 0x3C02 and rtz_fp16_bits(tie) == 0x3C01,
          "RNE tie->even (0x3C02) vs RTZ truncate (0x3C01) at %r" % tie)
    check(fp16_bits(65520.0) == 0x7C00 and rtz_fp16_bits(65520.0) == 0x7BFF,
          "overflow: RNE->inf (0x7C00), RTZ->max finite (0x7BFF)")
    check(rtz_fp16_bits(2.0 ** -24) == 0x0001 and fp16_bits(1.5 * 2.0 ** -24) == 0x0002
          and rtz_fp16_bits(1.5 * 2.0 ** -24) == 0x0001, "subnormal RTZ/RNE boundaries")
    check(ulp_diff(0x0000, 0x8000) == 0 and ulp_diff(0x3C00, 0x3C01) == 1,
          "ulp_key: +-0 identical, adjacent normals 1 ulp")
    # PNG / PFM writers
    with tempfile.TemporaryDirectory() as td:
        rgb = bytes(range(256)) * 3        # 768 bytes = 16x16 RGB
        p = os.path.join(td, "t.png")
        write_png(p, 16, 16, rgb)
        blob = open(p, "rb").read()
        check(blob[:8] == b"\x89PNG\r\n\x1a\n" and blob[-8:-4] == b"IEND",
              "PNG signature + IEND")
        pos = blob.find(b"IDAT") + 4
        idat = blob[pos:pos + struct.unpack(">I", blob[pos - 8:pos - 4])[0]]
        rows = zlib.decompress(idat)
        okpix = all(rows[1 + y * 49:49 + y * 49] == rgb[y * 48:(y + 1) * 48]
                    for y in range(16))
        check(okpix and len(rows) == 16 * 49, "PNG payload round-trips filter-0 rows")
        f4 = array("f", [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0])   # 2x1 RGBA
        p2 = os.path.join(td, "t.pfm")
        write_pfm_rgb(p2, 2, 1, f4)
        head = open(p2, "rb").read(24)
        check(head.startswith(b"PF\n2 1\n-1.000000\n"), "PFM header (PF, scale -1.000000)")
    # truncated .bin must raise
    with tempfile.TemporaryDirectory() as td:
        man = {"schema": EXPORT_SCHEMA, "frame": 3, "width": 4, "height": 4, "gi": True,
               "buffers": {"direct": {"file": "giexp_f3_direct.bin",
                                      "format": "RGBA16_Float", "bytes_per_pixel": 8,
                                      "row_pitch": 64, "width": 4, "height": 4,
                                      "semantics": "", "units": ""}}}
        with open(os.path.join(td, "giexp_f3.json"), "w") as fh:
            json.dump(man, fh)
        open(os.path.join(td, "giexp_f3_direct.bin"), "wb").write(b"\x00" * 63)
        try:
            ExportFrame(td, 3)
            check(False, "truncated .bin raises ExportError")
        except ExportError:
            check(True, "truncated .bin raises ExportError")
        # harness v2: tight row_pitch declared but file written in readback (256 B) layout
        man["buffers"]["direct"].update({"row_pitch": 32, "readback_row_pitch": 64})
        with open(os.path.join(td, "giexp_f3.json"), "w") as fh:
            json.dump(man, fh)
        open(os.path.join(td, "giexp_f3_direct.bin"), "wb").write(b"\x00" * 256)
        try:
            ExportFrame(td, 3)
            check(False, "row_pitch/file-layout disagreement raises ExportError")
        except ExportError as exc:
            check("readback_row_pitch" in str(exc),
                  "row_pitch/file-layout disagreement raises ExportError naming "
                  "readback_row_pitch")

    root = args.keep
    cleanup = None
    if not root:
        cleanup = tempfile.TemporaryDirectory()
        root = cleanup.name
    os.makedirs(root, exist_ok=True)
    W, H, FRAME = 100, 80, 89
    exp_fail = {"occluded_zero_exact", "dd_identity", "no_false_black",
                "direct_dd_spec_structure", "direct_ge_direct_diffuse",
                "composite_bitexact", "background_zeros", "indirect_sh_bound",
                "normal_unit_disc", "gbuffer_bitmatch_on_off",
                "off_occluded_ambient_exact", "ambient_no_feedback"}

    def run_frame(on, off, out, frame=FRAME):
        argv = ["frame", "--on", on, "--frame", str(frame), "--out", out]
        if off:
            argv += ["--off", off]
        return run_cli(argv)

    print("[selftest] S1: clean GI-on + GI-off frame -> all PASS")
    s1on = os.path.join(root, "on_clean")
    s1off = os.path.join(root, "off_clean")
    _ON_DIR[0] = s1on
    _OFF_DIR[0] = s1off
    base = _gen_on(W, H, FRAME)
    _gen_off(W, H, FRAME, on_dir_base=base)
    out1 = os.path.join(root, "out_clean")
    rc = run_frame(s1on, s1off, out1)
    rep = json.load(open(os.path.join(out1, "report.json")))
    check(rc == 0 and rep["overall"] == "PASS", "S1 frame exit 0 / overall PASS "
          "(rc=%d, failed=%s)" % (rc, rep["failed_checks"]))
    counts = rep["masks"]["counts"]
    check(counts["background"] == W * 12 and counts["occluded"] == W * 12
          and counts["penumbra"] == W * 12 and counts["lit"] == W * 44,
          "S1 mask counts bg/occ/pen/lit = %s" % (counts,))
    comp = next(c for c in rep["checks"] if c["id"] == "composite_bitexact")
    check(comp["exact_fraction_rtz"] == 1.0 and comp["inferred_rt_write_rounding"] == "rtz",
          "S1 composite 100%% exact under RTZ (inferred=%s, rne=%.4f)" %
          (comp["inferred_rt_write_rounding"], comp["exact_fraction_rne"]))
    occ = next(c for c in rep["checks"] if c["id"] == "occluded_zero_exact")
    check(occ["anomaly_count"] == 0, "S1 occluded zero anomalies = 0")
    want = ["report.json", "giexp_f89_direct.pfm", "giexp_f89_direct.png",
            "giexp_f89_direct_diffuse.pfm", "giexp_f89_direct_diffuse.png",
            "giexp_f89_indirect.pfm", "giexp_f89_indirect.png",
            "giexp_f89_final_prepost.pfm", "giexp_f89_final_prepost.png",
            "giexp_f89_off_final_prepost.pfm", "giexp_f89_off_final_prepost.png",
            "mask_overlay.png", "diff_on_off_x3.png", "same-view-gi-comparison-v2.png",
            "indirect_x16.png", "indirect-structure.png"]
    check(all(os.path.isfile(os.path.join(out1, f)) for f in want),
          "S1 wrote all expected artifacts (%d files)" % len(want))
    blob = open(os.path.join(out1, "indirect-structure.png"), "rb").read(24)
    check(struct.unpack(">II", blob[16:24]) == (2 * min(640, W) + 12,
                                               14 + 2 * (12 + (min(640, W) * H) // W) + 12),
          "S1 indirect-structure.png is the 2x2 sheet geometry %s" %
          (struct.unpack(">II", blob[16:24]),))
    check(rep["legend"]["indirect_gain"] == 16.0 and
          any(im["file"] == "indirect_x16.png" for im in rep["images"]),
          "S1 report records indirect gain 16 and lists indirect_x16.png")

    print("[selftest] S1t: same content, harness-v2 TIGHT rows (+readback_row_pitch) -> "
          "identical decode, all PASS")
    s1t_on = os.path.join(root, "on_clean_tight")
    s1t_off = os.path.join(root, "off_clean_tight")
    _LAYOUT[0] = "tight"
    try:
        _ON_DIR[0] = s1t_on
        _OFF_DIR[0] = s1t_off
        base = _gen_on(W, H, FRAME)
        _gen_off(W, H, FRAME, on_dir_base=base)
    finally:
        _LAYOUT[0] = "padded"
    mt = json.load(open(os.path.join(s1t_on, "giexp_f%d.json" % FRAME)))
    check(mt["buffers"]["direct"]["row_pitch"] == W * 8 and
          mt["buffers"]["probe_sh"]["row_pitch"] == 64 and
          mt["buffers"]["probe_sh"]["readback_row_pitch"] == 256,
          "S1t manifest is tight (direct row_pitch %d, probe row_pitch %d)" %
          (mt["buffers"]["direct"]["row_pitch"], mt["buffers"]["probe_sh"]["row_pitch"]))
    fa, fb = ExportFrame(s1on, FRAME), ExportFrame(s1t_on, FRAME)
    same = all(fa.buffers[k].raw.tobytes() == fb.buffers[k].raw.tobytes()
               for k in fa.buffers)
    check(same and set(fa.buffers) == set(fb.buffers),
          "S1t padded vs tight exports decode to byte-identical tight buffers (%d buffers)"
          % len(fa.buffers))
    out1t = os.path.join(root, "out_clean_tight")
    rc = run_frame(s1t_on, s1t_off, out1t)
    rep1t = json.load(open(os.path.join(out1t, "report.json")))
    check(rc == 0 and rep1t["overall"] == "PASS", "S1t frame exit 0 (failed=%s)"
          % (rep1t["failed_checks"],))

    print("[selftest] S2: planted defects -> exactly the planted checks FAIL")
    s2on = os.path.join(root, "on_defects")
    s2off = os.path.join(root, "off_defects")
    _ON_DIR[0] = s2on
    _OFF_DIR[0] = s2off
    d = {"occ_nonzero", "false_black", "comp_ulp1", "comp_ulp3", "bg_nonzero",
         "direct_lt_dd", "indirect_bound", "normal_bad"}
    base = _gen_on(W, H, FRAME, defects=d)
    _gen_off(W, H, FRAME, on_dir_base=base,
             defects={"off_albedo", "off_amb_ulp1", "off_amb_ulp4"})
    out2 = os.path.join(root, "out_defects")
    rc = run_frame(s2on, s2off, out2)
    rep2 = json.load(open(os.path.join(out2, "report.json")))
    got_fail = set(rep2["failed_checks"])
    check(rc == 1 and rep2["overall"] == "FAIL", "S2 frame exit 1 (rc=%d)" % rc)
    check(got_fail == exp_fail, "S2 failed set == planted set\n    got      %s\n    expected %s"
          % (sorted(got_fail), sorted(exp_fail)))
    occ2 = next(c for c in rep2["checks"] if c["id"] == "occluded_zero_exact")
    check(occ2["anomaly_count"] == 1 and occ2["anomaly_count_gt_2^-24"] == 1,
          "S2 occluded anomaly detected (count=%s, >2^-24=%s)" %
          (occ2["anomaly_count"], occ2["anomaly_count_gt_2^-24"]))

    print("[selftest] S2c: isolated composite ulp plants")
    s2c = os.path.join(root, "on_comp_ulp")
    _ON_DIR[0] = s2c
    _gen_on(W, H, FRAME, defects={"comp_ulp1", "comp_ulp3"})
    out2c = os.path.join(root, "out_comp_ulp")
    rc = run_frame(s2c, None, out2c)
    rep2c = json.load(open(os.path.join(out2c, "report.json")))
    comp2 = next(c for c in rep2c["checks"] if c["id"] == "composite_bitexact")
    check(rc == 1 and set(rep2c["failed_checks"]) == {"composite_bitexact"},
          "S2c only composite fails (failed=%s)" % (rep2c["failed_checks"],))
    check(comp2["ulp1_rtz"] == 1 and comp2["max_ulp_best"] == 3 and
          comp2["ulp_gt1_count_best"] == 1 and comp2["exact_fraction_rtz"] < 1.0,
          "S2c composite: +1-ulp-from-RTZ pixel is counted (ulp1_rtz=%s; it may read as "
          "exact under RNE), +3-ulp pixel fails (max=%s gt1=%s)" %
          (comp2["ulp1_rtz"], comp2["max_ulp_best"], comp2["ulp_gt1_count_best"]))

    print("[selftest] S2d: isolated GI-off plants (clean ON)")
    s2d_on = os.path.join(root, "on_for_offdef")
    s2d_off = os.path.join(root, "off_defects_only")
    _ON_DIR[0] = s2d_on
    _OFF_DIR[0] = s2d_off
    base = _gen_on(W, H, FRAME)
    _gen_off(W, H, FRAME, on_dir_base=base,
             defects={"off_albedo", "off_amb_ulp1", "off_amb_ulp4"})
    out2d = os.path.join(root, "out_off_defects_only")
    rc = run_frame(s2d_on, s2d_off, out2d)
    rep2d = json.load(open(os.path.join(out2d, "report.json")))
    amb = next(c for c in rep2d["checks"] if c["id"] == "ambient_no_feedback")
    oa = next(c for c in rep2d["checks"] if c["id"] == "off_occluded_ambient_exact")
    check(rc == 1 and set(rep2d["failed_checks"]) ==
          {"gbuffer_bitmatch_on_off", "off_occluded_ambient_exact", "ambient_no_feedback"},
          "S2d only the off-side checks fail (failed=%s)" % (rep2d["failed_checks"],))
    check(oa["violations_rtz"] == 2 and amb["violations_rtz"] == 1 and
          amb["max_ulp_best"] >= 3,
          "S2d off ambient: exact check catches both pixels (rtz=%s); 2-ulp envelope "
          "flags only the 4-ulp pixel (viol=%s max=%s; +4-from-RTZ is 3 ulp from RNE)" %
          (oa["violations_rtz"], amb["violations_rtz"], amb["max_ulp_best"]))

    print("[selftest] S2e: identical data, GI-off manifest camera moved -> only the "
          "same-view check fails")
    s2e_off = os.path.join(root, "off_moved_camera")
    os.makedirs(s2e_off, exist_ok=True)
    for fn in os.listdir(s1off):
        with open(os.path.join(s1off, fn), "rb") as src, \
                open(os.path.join(s2e_off, fn), "wb") as dst:
            dst.write(src.read())
    mpath = os.path.join(s2e_off, "giexp_f%d.json" % FRAME)
    me = json.load(open(mpath))
    me["camera"]["position"][0] += 1.0
    with open(mpath, "w") as fh:
        json.dump(me, fh, indent=1)
    out2e = os.path.join(root, "out_moved_camera")
    rc = run_frame(s1on, s2e_off, out2e)
    rep2e = json.load(open(os.path.join(out2e, "report.json")))
    check(rc == 1 and rep2e["failed_checks"] == ["on_off_same_view_and_light"],
          "S2e only on_off_same_view_and_light fails (failed=%s)" % (rep2e["failed_checks"],))

    print("[selftest] S2b: GI-off POST=0 (BGRA8 final) clean -> PASS")
    s2b = os.path.join(root, "off_post0")
    _OFF_DIR[0] = s2b
    base = _gen_base(W, H)
    # regenerate the ON base deterministically for identical gbuffers
    _ON_DIR[0] = s1on
    base = _gen_on(W, H, FRAME)   # identical content, rewritten to s1on
    _OFF_DIR[0] = s2b
    _gen_off(W, H, FRAME, post=False, on_dir_base=base)
    out2b = os.path.join(root, "out_post0")
    rc = run_frame(s1on, s2b, out2b)
    rep2b = json.load(open(os.path.join(out2b, "report.json")))
    check(rc == 0, "S2b POST=0 off run: rc==0 (failed=%s)" % (rep2b["failed_checks"],))

    print("[selftest] S3/S4/S4b: ladder")
    def _mklog(path, corrupt=False, drop_reset=False, degenerate=False):
        resets = [0, 90, 132]
        t_low, t_high = 0.16, 0.16 * (2.2 / 0.85)
        lines = []
        frames = sorted(set(list(range(0, 160, 5)) + resets + [159]))
        for f in frames:
            if f < 90:
                T, f0 = t_low * 0.7, 0
            elif f < 132:
                T, f0 = t_low, 90
            else:
                T, f0 = t_high, 132
            m = T * (1.0 - 0.9 ** (f - f0 + 1))
            if degenerate and 90 <= f < 132:
                m = 0.0
            if corrupt and f >= 132:
                m *= 1.5
            if corrupt and f == 100:
                m *= 0.5
            lines.append("[GI_MEAN] frame %d r=%.9f g=%.9f b=%.9f"
                         % (f, m, m * 0.98, m * 0.92))
            if f in resets and not (drop_reset and f == 132):
                lines.append("[GI] frame %d RESET (light event; history zeroed)" % f)
            lines.append("... unrelated stderr noise frame %d" % f)
        open(path, "w").write("\n".join(lines) + "\n")

    l3 = os.path.join(root, "ladder_clean.log")
    _mklog(l3)
    r3 = os.path.join(root, "ladder_clean.json")
    rc = run_cli(["ladder", "--log", l3, "--out", r3])
    rep3 = json.load(open(r3))
    check(rc == 0 and rep3["overall"] == "PASS", "S3 ladder clean rc==0 (failed=%s)"
          % (rep3["failed_checks"],))
    check(rep3["reset_frames"] == [0, 90, 132], "S3 resets parsed [0,90,132]")
    t2 = rep3["segments"][0]["T"]
    t3 = rep3["segments"][1]["T"]
    dev = max(abs((t3[c] / t2[c]) / (2.2 / 0.85) - 1.0) for c in range(3))
    check(dev < 1e-6, "S3 fitted T ratio == 2.2/0.85 within 1e-6 (dev=%.2e)" % dev)
    l4 = os.path.join(root, "ladder_defect.log")
    _mklog(l4, corrupt=True, drop_reset=True)
    r4 = os.path.join(root, "ladder_defect.json")
    rc = run_cli(["ladder", "--log", l4, "--out", r4])
    rep4 = json.load(open(r4))
    seg0 = rep4["segments"][0]
    seg1 = rep4["segments"][1]
    check(rc == 1 and seg0["monotonicity_violations"] >= 1 and seg1["status"] == "FAIL",
          "S4 corrupted ladder: monotonic dip caught (%s) and dropped RESET makes the "
          "132-155 fit FAIL (status=%s, rc=%d)" %
          (seg0["monotonicity_violations"], seg1["status"], rc))
    l4b = os.path.join(root, "ladder_degenerate.log")
    _mklog(l4b, degenerate=True)
    r4b = os.path.join(root, "ladder_degenerate.json")
    rc = run_cli(["ladder", "--log", l4b, "--out", r4b])
    rep4b = json.load(open(r4b))
    seg0 = rep4b["segments"][0]
    check(rc == 0 and seg0["status"] == "degenerate: target 0",
          "S4b all-zero segment reported 'degenerate: target 0', overall still PASS")

    def _mklog_steps(path, wrong=False):
        # resets 0/45/90, intensity 2.2 -> 0.85 -> 2.2; harness prints 6 significant digits
        t1 = 0.0058
        t2 = t1 * (0.85 / 2.2) * (1.1 if wrong else 1.0)
        lines = []
        for f in sorted(set(list(range(0, 120, 5)) + [45, 90, 119])):
            T, f0 = (t1, 0) if f < 45 else ((t2, 45) if f < 90 else (t1, 90))
            m = T * (1.0 - 0.9 ** (f - f0 + 1))
            if f in (0, 45, 90):
                lines.append("[GI] frame %d RESET (light event; history zeroed)" % f)
            lines.append("[GI_MEAN] frame %d r=%.6g g=%.6g b=%.6g"
                         % (f, m, m * 0.877, m * 0.734))
        open(path, "w").write("\n".join(lines) + "\n")

    steps_args = ["--segments", "0-44,45-89,90-119", "--pair-ratios",
                  "2/1=%.12g,3/2=%.12g,3/1=1" % (0.85 / 2.2, 2.2 / 0.85),
                  "--ratio-tol", "0.02"]
    l3p = os.path.join(root, "ladder_steps.log")
    _mklog_steps(l3p)
    r3p = os.path.join(root, "ladder_steps.json")
    rc = run_cli(["ladder", "--log", l3p, "--out", r3p] + steps_args)
    rep3p = json.load(open(r3p))
    pr = {p["pair"]: p for p in rep3p["ratio"]["pairs"]}
    check(rc == 0 and rep3p["reset_frames"] == [0, 45, 90] and
          rep3p["ratio"]["status"] == "PASS" and
          pr["3/1"]["k_aligned_max_rel_dev"] == 0.0 and
          pr["2/1"]["T_ratio_max_rel_dev"] < 1e-4,
          "S3p steps ladder: resets [0,45,90], pairs PASS, 3/1 k-aligned exact (dev=%s), "
          "2/1 fitted dev %.2e" % (pr["3/1"]["k_aligned_max_rel_dev"],
                                   pr["2/1"]["T_ratio_max_rel_dev"]))
    l3q = os.path.join(root, "ladder_steps_wrong.log")
    _mklog_steps(l3q, wrong=True)
    r3q = os.path.join(root, "ladder_steps_wrong.json")
    rc = run_cli(["ladder", "--log", l3q, "--out", r3q] + steps_args)
    rep3q = json.load(open(r3q))
    pq = {p["pair"]: p["status"] for p in rep3q["ratio"]["pairs"]}
    check(rc == 1 and pq == {"2/1": "FAIL", "3/2": "FAIL", "3/1": "PASS"},
          "S3q 10%% wrong middle step: 2/1 and 3/2 FAIL, 3/1 still PASS (%s)" % (pq,))

    print("[selftest] S5/S6: linearity")
    linA = os.path.join(root, "lin_a")
    linB = os.path.join(root, "lin_b_clean")
    _ON_DIR[0] = linA
    baseA = _gen_on(W, H, FRAME)
    _ON_DIR[0] = linB
    _gen_linearity_b(W, H, FRAME)
    r5 = os.path.join(root, "lin_clean.json")
    rc = run_cli(["linearity", "--a", linA, "--b", linB, "--frame", str(FRAME),
                  "--ratio", "0.5", "--out", r5])
    rep5 = json.load(open(r5))
    check(rc == 0 and rep5["overall"] == "PASS", "S5 linearity clean rc==0 (failed=%s)"
          % (rep5["failed_checks"],))
    linB2 = os.path.join(root, "lin_b_defect")
    _ON_DIR[0] = linB2
    _gen_linearity_b(W, H, FRAME, defects={"occ_ambient"})
    # plant a constant ambient in B at occluded pixels (A == 0 there)
    p = os.path.join(linB2, "giexp_f89_direct_diffuse.bin")
    blob = bytearray(open(p, "rb").read())
    pitch = _align256(W * 8)
    occ_y = 12 + 3
    for x in range(0, W, 2):
        off = occ_y * pitch + x * 8
        v = _store_h(0.01)
        blob[off:off + 6] = struct.pack("<3H", v, v, v)
    open(p, "wb").write(bytes(blob))
    r6 = os.path.join(root, "lin_defect.json")
    rc = run_cli(["linearity", "--a", linA, "--b", linB2, "--frame", str(FRAME),
                  "--ratio", "0.5", "--out", r6])
    rep6 = json.load(open(r6))
    dd6 = next(c for c in rep6["checks"] if c["id"] == "linearity_direct_diffuse")
    check(rc == 1 and dd6["status"] == "FAIL" and dd6["zero_consistency_violations"] == 150,
          "S6 constant ambient in B caught by zero-consistency "
          "(violations=%s)" % (dd6["zero_consistency_violations"],))
    dd5 = next(c for c in rep5["checks"] if c["id"] == "linearity_direct_diffuse")
    check(dd5["per_channel"]["r"]["exact_ratio_fraction"] == 1.0 and
          dd5["max_dev_p99"] == 0.0,
          "S5 clean linearity is bit-exact x0.5 (exact fraction r=%s, dev p99=%s)" %
          (dd5["per_channel"]["r"]["exact_ratio_fraction"], dd5["max_dev_p99"]))
    r5s = os.path.join(root, "lin_swapped.json")
    rc = run_cli(["linearity", "--a", linB, "--b", linA, "--frame", str(FRAME),
                  "--ratio", "0.5", "--out", r5s])
    rep5s = json.load(open(r5s))
    dd5s = next(c for c in rep5s["checks"] if c["id"] == "linearity_direct_diffuse")
    check(rc == 1 and dd5s["swap_hint"] is not None,
          "S5s swapped --a/--b: FAIL with swap hint (%s)" % (dd5s["swap_hint"],))

    print("[selftest] S8: summary aggregation")
    s8 = os.path.join(root, "SUMMARY_pass.json")
    rc = run_cli(["summary", "--reports", os.path.join(out1, "report.json"), r3p, r5,
                  "--out", s8])
    sm = json.load(open(s8))
    check(rc == 0 and sm["overall"] == "PASS" and len(sm["entries"]) == 3 and
          sm["entries"][0]["composite"]["inferred_rounding"] == "rtz" and
          len(sm["tool_sha256"]) == 64,
          "S8 summary of 3 PASS reports -> PASS with key numbers and tool sha256")
    s8f = os.path.join(root, "SUMMARY_fail.json")
    rc = run_cli(["summary", "--reports", r5, r6, "--out", s8f])
    smf = json.load(open(s8f))
    check(rc == 1 and smf["fail_entries"] == [os.path.abspath(r6)],
          "S8 summary including a FAIL report -> FAIL naming it")

    if not args.skip_timing:
        print("[selftest] S7: %dx%d timing scenario (clean frame cmd)" %
              (args.timing_width, args.timing_height))
        TW, TH = args.timing_width, args.timing_height
        t7on = os.path.join(root, "timing_on")
        t7off = os.path.join(root, "timing_off")
        _ON_DIR[0] = t7on
        _OFF_DIR[0] = t7off
        base = _gen_on(TW, TH, FRAME)
        _gen_off(TW, TH, FRAME, on_dir_base=base)
        out7 = os.path.join(root, "out_timing")
        ts = time.time()
        rc = run_frame(t7on, t7off, out7)
        dt = time.time() - ts
        rep7 = json.load(open(os.path.join(out7, "report.json")))
        check(rc == 0 and rep7["overall"] == "PASS",
              "S7 %dx%d frame PASS in %.1fs (rc=%d, failed=%s)"
              % (TW, TH, dt, rc, rep7["failed_checks"]))
        print("[selftest] TIMING %dx%d full frame verify: %.2f s" % (TW, TH, dt))
        timing["%dx%d" % (TW, TH)] = round(dt, 3)

    if cleanup:
        cleanup.cleanup()
    total = time.time() - t0
    if args.report:
        with open(args.report, "w", encoding="utf-8") as fh:
            json.dump({"tool": TOOL_VERSION, "command": "selftest", "argv": my_argv,
                       "overall": "PASS" if not failures else "FAIL",
                       "assertions_ok": passes[0], "assertions_failed": len(failures),
                       "failures": failures, "timing_sec": timing,
                       "elapsed_sec": round(total, 3)}, fh, indent=1, sort_keys=True)
    if failures:
        print("[selftest] FAIL: %d assertion(s) failed, %d ok (%.1fs)"
              % (len(failures), passes[0], total))
        for f in failures:
            print("  - %s" % f)
        return 1
    print("[selftest] PASS: all %d assertions green (%.1fs)" % (passes[0], total))
    return 0


# ----------------------------------------------------------------------------------------
# summary: aggregate report JSONs -> SUMMARY.json (key numbers per command)
# ----------------------------------------------------------------------------------------

def tool_sha256():
    import hashlib
    with open(os.path.abspath(__file__), "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()


def _summ_frame(r):
    ck = {c["id"]: c for c in r.get("checks", [])}

    def g(cid, key):
        return ck.get(cid, {}).get(key)

    on = r["inputs"]["on"]
    off = r["inputs"].get("off")
    gb = ck.get("gbuffer_bitmatch_on_off", {}).get("per_buffer")
    gb_bytes = None
    if isinstance(gb, dict):
        gb_bytes = sum(v.get("differing_bytes", 0) for v in gb.values() if isinstance(v, dict))
    roi = (r.get("info") or {}).get("indirect_roi_stats") or {}
    contrast = (r.get("info") or {}).get("contrast") or {}
    return {
        "on": {"dir": on["dir"], "frame": on["frame"], "light": on.get("light"),
               "frames_since_reset": (on.get("gi_grid") or {}).get("frames_since_reset"),
               "reset_this_frame": (on.get("gi_grid") or {}).get("reset_this_frame")},
        "off": {"dir": off["dir"], "frame": off["frame"]} if off else None,
        "masks": r["masks"]["counts"],
        "check_status": {cid: c["status"] for cid, c in ck.items()},
        "occluded_anomaly_count": g("occluded_zero_exact", "anomaly_count"),
        "occluded_anomaly_count_gt_2^-24": g("occluded_zero_exact", "anomaly_count_gt_2^-24"),
        "dd_identity": {k: g("dd_identity", k) for k in
                        ("violation_channels", "rel_err_p50", "rel_err_p99", "max_rel_err",
                         "max_abs_err", "pixels_checked")},
        "no_false_black": {k: g("no_false_black", k) for k in
                           ("false_black_pixels_raw", "fp16_subnormal_underflow_explained",
                            "false_black_unexplained")},
        "direct_ge_dd_violations": g("direct_ge_direct_diffuse", "violation_channels"),
        "spec_structure_violations": g("direct_dd_spec_structure", "violation_pixels"),
        "composite": {"exact_fraction_rne": g("composite_bitexact", "exact_fraction_rne"),
                      "exact_fraction_rtz": g("composite_bitexact", "exact_fraction_rtz"),
                      "inferred_rounding": g("composite_bitexact",
                                             "inferred_rt_write_rounding"),
                      "max_ulp_best": g("composite_bitexact", "max_ulp_best"),
                      "ulp1_count_best": g("composite_bitexact", "ulp1_count_best"),
                      "alpha_not_one": g("composite_bitexact", "alpha_not_one_count")},
        "background_nonzero": g("background_zeros", "total_nonzero"),
        "normal_disc_violations": g("normal_unit_disc", "violations"),
        "indirect_sh_bound": {"bound": g("indirect_sh_bound", "sh_bound"),
                              "violations": g("indirect_sh_bound", "bound_violations"),
                              "negative": g("indirect_sh_bound", "negative_count")},
        "on_off_same_view_and_light": ck.get("on_off_same_view_and_light", {}).get("status"),
        "gbuffer_on_off_differing_bytes": gb_bytes,
        "off_occluded_ambient": {
            "exact_fraction_rtz": g("off_occluded_ambient_exact", "exact_fraction_rtz"),
            "exact_fraction_rne": g("off_occluded_ambient_exact", "exact_fraction_rne"),
            "inferred_rounding": g("off_occluded_ambient_exact", "inferred_rt_write_rounding"),
            "channels": g("off_occluded_ambient_exact", "channels")},
        "on_off_max_ulp": g("ambient_no_feedback", "max_ulp_best"),
        "on_off_violations": {"rtz": g("ambient_no_feedback", "violations_rtz"),
                              "rne": g("ambient_no_feedback", "violations_rne")},
        "indirect_mean_per_roi": {
            name: [((v.get("channels") or {}).get(ch) or {}).get("mean") for ch in "rgb"]
            for name, v in roi.items()},
        "indirect_occluded_gt0_fraction":
            ((r.get("info") or {}).get("indirect_misc") or {}).get("fraction"),
        "contrast_lit_over_occluded": {
            k: (contrast.get(k) or {}).get("lit_over_occluded")
            for k in ("final_on", "final_off") if contrast.get(k)},
        "images": [im["file"] for im in r.get("images", [])],
    }


def _summ_ladder(r):
    segs = []
    for s in r.get("segments", []):
        ch = s.get("channels") or {}
        segs.append({
            "segment": s.get("segment"), "f0": s.get("f0"), "status": s.get("status"),
            "T": s.get("T"),
            "rms_residual_over_T": [(ch.get(c) or {}).get("rms_residual_over_T") for c in "rgb"],
            "max_abs_residual_over_T": [(ch.get(c) or {}).get("max_abs_residual_over_T")
                                        for c in "rgb"],
            "monotonicity_violations": s.get("monotonicity_violations")})
    pairs = []
    for p in (r.get("ratio") or {}).get("pairs", []):
        al = (p.get("k_aligned") or {}).get("per_channel") or {}
        pairs.append({
            "pair": p.get("pair"), "expected": p.get("expected_ratio"),
            "status": p.get("status"), "reason": p.get("reason"),
            "T_ratio": p.get("T_ratio"), "T_ratio_max_rel_dev": p.get("T_ratio_max_rel_dev"),
            "k_aligned_mean": {c: (al.get(c) or {}).get("mean") for c in "rgb"} if al else None,
            "k_aligned_n": (al.get("r") or {}).get("n") if al else None,
            "k_aligned_max_rel_dev": p.get("k_aligned_max_rel_dev")})
    return {"log": r.get("log"), "reset_frames": r.get("reset_frames"),
            "gi_mean_count": r.get("gi_mean_count"), "segments": segs, "pairs": pairs,
            "ratio_status": (r.get("ratio") or {}).get("status"),
            "ratio_tolerance_rel": (r.get("ratio") or {}).get("tolerance_rel")}


def _summ_linearity(r):
    out = {"a": (r.get("a") or {}).get("dir"), "b": (r.get("b") or {}).get("dir"),
           "ratio": r.get("ratio"), "convention": r.get("ratio_convention"), "checks": {}}
    for c in r.get("checks", []):
        e = {"status": c.get("status")}
        if "per_channel" in c and c["id"] != "linearity_gi_mean":
            e["per_channel"] = {
                ch: {k: v.get(k) for k in ("median_ratio", "p99_ratio", "dev_p50", "dev_p99",
                                           "dev_max", "exact_ratio_fraction", "samples")}
                for ch, v in c["per_channel"].items()}
            e["zero_consistency_violations"] = c.get("zero_consistency_violations")
            e["swap_hint"] = c.get("swap_hint")
        elif c["id"] == "linearity_probe_c00":
            e.update({k: c.get(k) for k in ("median_ratio", "p99_ratio", "dev_p50", "dev_p99",
                                            "dev_max", "exact_ratio_fraction", "samples",
                                            "zero_consistency_violations")})
        elif c["id"] == "linearity_gi_mean":
            e["per_channel"] = c.get("per_channel")
            e["reason"] = c.get("reason")
        else:
            e["reason"] = c.get("reason")
        out["checks"][c["id"]] = e
    return out


def cmd_summary(args):
    entries = []
    for path in args.reports:
        r = json.load(open(path, encoding="utf-8"))
        cmd = r.get("command")
        e = {"report": os.path.abspath(path), "command": cmd, "argv": r.get("argv"),
             "overall": r.get("overall"), "failed_checks": r.get("failed_checks")}
        if cmd == "frame":
            e.update(_summ_frame(r))
        elif cmd == "ladder":
            e.update(_summ_ladder(r))
        elif cmd == "linearity":
            e.update(_summ_linearity(r))
        elif cmd == "selftest":
            e.update({k: r.get(k) for k in ("assertions_ok", "assertions_failed",
                                            "failures", "timing_sec", "elapsed_sec")})
        entries.append(e)
    fails = [e["report"] for e in entries if e.get("overall") != "PASS"]
    summary = {
        "tool": TOOL_VERSION, "tool_path": os.path.abspath(__file__),
        "tool_sha256": tool_sha256(), "python": sys.version.split()[0],
        "generated_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "argv": _ARGV[0], "entries": entries,
        "overall": "PASS" if not fails else "FAIL", "fail_entries": fails,
    }
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(summary, fh, indent=1, sort_keys=True)
    print("[summary] %s  entries=%d  fails=%d  -> %s" %
          (summary["overall"], len(entries), len(fails), args.out))
    return 0 if not fails else 1


# ----------------------------------------------------------------------------------------
# CLI
# ----------------------------------------------------------------------------------------

def build_argparser():
    p = argparse.ArgumentParser(
        prog="m6_gi_verify.py",
        description="Verify M6 GI exports of TestRHISponzaShared (see module docstring).")
    sub = p.add_subparsers(dest="cmd", required=True)

    pf = sub.add_parser("frame", help="masks + §8 checks + PFM/PNG/contact sheet")
    pf.add_argument("--on", required=True, help="GI-on export dir (giexp_f<N>.json)")
    pf.add_argument("--frame", required=True, type=int)
    pf.add_argument("--off", help="GI-off export dir (same view; M4 path)")
    pf.add_argument("--off-frame", type=int, help="default: same as --frame")
    pf.add_argument("--out", required=True, help="output dir for report + images")
    pf.add_argument("--indirect-gain", type=float, default=16.0,
                    help="linear gain for indirect_x<gain>.png / indirect-structure.png "
                         "(applied before exposure x ACES; default 16)")

    pl = sub.add_parser("ladder", help="[GI_MEAN] EMA ladder fit from a stderr log")
    pl.add_argument("--log", required=True)
    pl.add_argument("--segments", default="90-115,132-155",
                    help="static-camera segments a-b,c-d (m6 trajectory gallery hold)")
    pl.add_argument("--intensity-ratio", type=float, default=2.2 / 0.85,
                    help="expected fitted-T ratio of each segment over the previous one "
                         "(default 2.2/0.85); ignored when --pair-ratios is given")
    pl.add_argument("--pair-ratios",
                    help="explicit expected T_i/T_j per pair, 1-based segment indices, "
                         "e.g. '2/1=0.386364,3/2=2.588235,3/1=1'")
    pl.add_argument("--ratio-tol", type=float, default=0.15,
                    help="relative tolerance on fitted-T and k-aligned ratios (default "
                         "0.15 for the resized m6 trajectory; use 0.02 for same-view steps)")
    pl.add_argument("--out", required=True, help="report json path")

    pn = sub.add_parser("linearity", help="intensity A/B ratio check")
    pn.add_argument("--a", required=True, help="export dir at higher intensity")
    pn.add_argument("--b", required=True, help="export dir at lower intensity")
    pn.add_argument("--frame", required=True, type=int)
    pn.add_argument("--ratio", type=float, default=0.5, help="expected B/A (default 0.5)")
    pn.add_argument("--out", required=True, help="report json path")
    pn.add_argument("--log-a", help="optional stderr log of run A (GI_MEAN ratio)")
    pn.add_argument("--log-b", help="optional stderr log of run B")

    ps = sub.add_parser("selftest", help="synthetic exports with planted defects")
    ps.add_argument("--keep", metavar="DIR",
                    help="keep scenario dirs/logs under DIR for manual inspection")
    ps.add_argument("--skip-timing", action="store_true",
                    help="skip the full-resolution timing scenario")
    ps.add_argument("--timing-width", type=int, default=1280)
    ps.add_argument("--timing-height", type=int, default=720)
    ps.add_argument("--report", metavar="JSON",
                    help="also write assertion counts/timing as JSON (for `summary`)")

    pm = sub.add_parser("summary", help="aggregate frame/ladder/linearity/selftest report "
                                        "JSONs into one SUMMARY.json with key numbers")
    pm.add_argument("--reports", nargs="+", required=True, help="report json paths")
    pm.add_argument("--out", required=True, help="SUMMARY.json path")
    return p


_ARGV = [None]     # argv of the current invocation (recorded in every report)


def run_cli(argv):
    _ARGV[0] = list(argv)
    args = build_argparser().parse_args(argv)
    if args.cmd == "frame":
        return cmd_frame(args)
    if args.cmd == "ladder":
        return cmd_ladder(args)
    if args.cmd == "linearity":
        return cmd_linearity(args)
    if args.cmd == "selftest":
        return cmd_selftest(args)
    if args.cmd == "summary":
        return cmd_summary(args)
    return 2


def main(argv=None):
    try:
        rc = run_cli(sys.argv[1:] if argv is None else argv)
    except ExportError as exc:
        print("ERROR: %s" % exc, file=sys.stderr)
        return 2
    return rc


if __name__ == "__main__":
    sys.exit(main())
