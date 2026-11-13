# Windows parity CI - self-hosted runner setup (D3D12_runner)

This document prepares a Windows machine to run `.github/workflows/windows-parity.yml`
(runner labels: `[self-hosted, Windows, X64, D3D12, Vulkan]`). Everything the
workflow needs that is not in git is injected through machine environment
variables - no user Desktop paths are hardcoded in the workflow.

Verified reference machine (r11, commit `73d6956`): Windows 10 19045, GTX 1060
6 GB, VS2022 Community 17 (MSVC 19.39.33521), Windows SDK 10.0.22000,
Vulkan SDK 1.3.250.1, Python 3.10.9 (anaconda).

## 1. Software prerequisites

| Component | Requirement | Notes |
|---|---|---|
| Visual Studio 2022 + MSVC x64 | required | "Desktop development with C++"; the workflow configures `-G "Visual Studio 17 2022" -A x64` |
| Windows SDK | 10.0.22000+ | installed with VS |
| CMake | 3.20+ on PATH | generator must support VS 2022 |
| Python | 3.8+ on PATH (`python`) | stdlib-only scripts; no numpy/PIL needed anywhere in this CI |
| Vulkan SDK (runtime + headers) | 1.3.250+ | engine links `vulkan-1.lib`; the SDK also ships glslangValidator |
| glslangValidator | **required** | shader-mirror generation; path via `D3D12_GLSLANG` or PATH |
| spirv-cross | **required** | same, via `D3D12_SPIRV_CROSS` or PATH |
| dxc (DXC standalone) | **required** | same, via `D3D12_DXC` or PATH |
| Git | required | checkout fetches submodules recursively |
| GitHub Actions runner | registered with labels `Windows, X64, D3D12, Vulkan` | see the session note below |

Since 751b467d the D3D12 shader mirrors (`.hlsl`/`.cso`/`manifest.json`) are
**not tracked in git**. A fresh checkout has no mirrors, so the shader
toolchain (glslangValidator / spirv-cross / dxc) is a hard requirement - the
workflow fails with an actionable error if the tools are missing and no
mirrors exist. If stale mirrors happen to be present in a reused workspace,
the step degrades to a warning (same semantics as the `windows-d3d12` job in
`ci.yml`).

## 2. Runner session context (important)

`TestVulkanSponzaRenderGraph` and `TestRHISponzaShared` create real GPU
windows/swap chains. A GitHub runner installed **as a Windows service** runs
in session 0 where window creation typically fails. Run the runner as an
interactive process instead:

- either start `run.cmd` manually in the logged-in user session, or
- schedule it via Task Scheduler with "Run only when user is logged on",
- keep the machine auto-login + runner auto-start if unattended runs are
  needed.

The r11 verification ran all four f350 groups from a normal desktop session
on the reference machine.

## 3. Environment variables (machine scope)

Set these as machine-level (or user-level, visible to the runner process)
environment variables:

| Variable | Required | Meaning |
|---|---|---|
| `PRIMAL_CI_ASSET_DIR` | yes | Directory holding the four local assets of section 4 (flat layout, exact filenames). |
| `PRIMAL_CI_HARNESS_BASELINE` | yes | Directory holding the seeded M2-M6 frame baseline (section 5). If unset, the harness gate fails with seed instructions - it is never silently skipped. |
| `D3D12_GLSLANG` / `D3D12_SPIRV_CROSS` / `D3D12_DXC` | if not on PATH | Absolute paths to glslangValidator / spirv-cross / dxc. Read natively by `scripts/build_d3d12_shaders.py`. |
| `PRIMAL_CI_OFFLINE_DEPS` | optional | Directory with `agility-sdk\build\native\include\d3d12.h` (NuGet layout) and `zlib-src\CMakeLists.txt`. Injected as `-DPRIMAL_D3D12_AGILITY_SDK_DIR` / `-DPRIMAL_ZLIB_SOURCE_DIR`. If unset, CMake downloads Agility 1.611.2 from nuget.org (SHA-pinned) and FetchContent-pulls zlib - needs outbound network. |
| `PRIMAL_CI_SDK_MODULES` | optional | `diagnostic-sdk-modules` directory injected as `-DCMAKE_MODULE_PATH` (e.g. for finding DirectXTex/FBX config packages). Optional when `BUILD_CONTENT_TOOLS=OFF`. |
| `PRIMAL_CI_BUILD_DIR` | optional | Build directory inside the checkout (default `build-winparity`). Reused across runs (checkout uses `clean: false`) for incremental builds. |
| `PRIMAL_CI_BIN_DIR` | optional | Explicit directory of the built test exes; otherwise the workflow probes `build\bin\Release`, `Windows\Release`, `Windows\x64\Release`, `build\Release`, `build\Tests\Release`. |

Do **not** set `PRIMAL_VK_HANG_DIAG` on the runner: the f350 groups must run
the official default path.

## 4. Local assets (SHA-gated staging)

The workflow's "Stage local assets" step copies these from `PRIMAL_CI_ASSET_DIR`
into `EngineTest/assets/` and verifies each SHA256. None of them are in git
(`*.model` and heavy assets are ignored; the SDF cache is a bake product).

| File | Bytes | SHA256 |
|---|---|---|
| `Sponza.model` | 7,491,644 | `b0c1f2d5e583508aab922c6e4c2c38cf2a564f952709f45827ac10d4c4757390` |
| `SponzaTangentFrames.model` | 189,764,292 | `974efb3a95d7773519691ee937c493fcb306dd717066e42431c691ea7af66d5e` |
| `sunset.hdr` | 1,814,489 | `49627ecb64a77e12567c8c3853cb63248a14013fd7553ebffb904f8fd428ffec` |
| `Sponza_global_sdf.v3.cache` | 113,246,264 | `0f00339ae8fc472a0f5b003bc060ab1f7a1b64e2c7d5b5e3d6b93146214ec581` |

`sunset.hdr` lands in `EngineTest/assets/textures/hdr/`, the other three in
`EngineTest/assets/`. The Mac Vulkan reference PPM for the PSNR gate is
tracked in-repo at `.github/ci-assets/ref_mac_vulkan_RG_f350.ppm`
(SHA256 `f0b032b3b006f6655e510f77c0ece832341678215df9fb3b2cd5272fa9477158`,
verified at runtime by `scripts/win-ci/parity_report.py`).

Optional: `Sponza_pipeline_signed_v5.model` (150,864,704 B, SHA256
`578958bd2dc4fac5ceb2419b80194c00a795a9645f2f5b6c485bc6f4c101a09d`) is only
needed as a bake fallback when the v3 SDF cache is absent; the CI does not
require it.

## 5. Seeding the harness baseline (one-time per engine baseline)

The 276-frame M2-M6 byte gate compares against a runner-local baseline, not
git. Seed it once on a known-good commit (r11 `73d6956` or the merged dev tip
`05e44628`/`b8bb56c3`), after building normally:

```bat
set PRIMAL_CI_HARNESS_BASELINE=C:\primal-ci\harness-baseline
cd /d C:\path\to\checkout
git checkout <known-good-sha>
python scripts\win-ci\run_harness.py --repo . ^
  --exe build-winparity\bin\Release\TestRHISponzaShared.exe ^
  --out ci-out\harness --seed
```

`--seed` copies the produced `*.ppm`/`*.pgm` frames into
`%PRIMAL_CI_HARNESS_BASELINE%\<run-name>\` and writes `seed.json`. Re-seed
with `--force` only when a deliberate, reviewed engine change alters harness
frames - never to make a failing gate green.

Official r11 reference counts: 18 runs, 276 frames total
(m6-full 73 / m6-fixed 29 / m6-fixed-gi0 4 / mN-full 12 / mN-fixed 2 per
platform). The runner prints per-run counts and warns (advisory) on
mismatch; the gate itself is byte-identity against this seeded baseline.

## 6. Disk, VRAM, runtime budget

- Disk: checkout + submodules ~8 GB, build ~15-25 GB, assets ~300 MB,
  baseline ~780 MB, per-run outputs ~1.5 GB (harness frames + exports are
  kept on disk; only reports/logs/PPM samples are uploaded).
- VRAM: the reference GPU is 6 GB (GTX 1060). Known limitation (r11,
  unfixed): Vulkan double-residency makes f350 run at ~1.1 s/frame
  (~8 min for 400 frames per Vulkan group). The job timeout is 180 min.
- Expect the four f350 groups to take ~17 min on the reference machine when
  Vulkan completes; D3D12 groups abort within seconds today (device removal).

## 7. cp936 / code page notes

Every script this workflow runs (`scripts/win-ci/*.py`, `run_f350.ps1`, and
all inline step bodies) is ASCII-only. Do not add Chinese comments to `.ps1`
files or inline step bodies: the runner console is cp936 and the r9/r10
history showed cmd misparses non-ASCII comments (three `<` in a UTF-8 REM
line were read as redirection). The working copy of `run_f350.ps1` is CRLF;
git may normalize it to LF in-repo and re-materialize it with either ending
depending on the runner's `core.autocrlf` - both forms parse identically in
Windows PowerShell 5.1 and PowerShell 7 (verified with the parser and a full
stub run; the cp936 hazards are encoding and `<`/`>` in comments, not LF).
The Python helpers are also ASCII (the tracked `compare_f350.py` copy keeps
the upstream tool byte-identical, UTF-8 docstrings included - Python source
encoding is unaffected by the console code page).

## 8. Current parity state (what a red job means today)

As of r11 (`73d6956`, merged into dev):

- **D3D12 GI-on / GI-off f350: no frames** (post-ParticlePass device-removal
  cascade). Capture gate FAIL is the expected, honest result.
- **Windows Vulkan GI-on f350: ~34.43 dB / SSIM 0.9555** vs the Mac
  reference - SDF root cause cleared, residual is the gather-resolution
  family. The PSNR gate stays at **43.5 dB** and is deliberately NOT lowered:
  the summary marks this FAIL until real parity lands.
- **Windows Vulkan GI-off f350: r11 crashed (0xC0000005) near frame 301**;
  capture may be missing -> capture gate FAIL.
- Harness M2-M6 (276 frames) and M6 strict (20/20) were fully green on r11
  and are expected to stay green on the merged dev tip.

A green job therefore requires: build + shader gate + units + harness byte
gate + M6 strict + four f350 captures + four PSNR >= 43.5 dB. Anything red
is reported as-is in `$GITHUB_STEP_SUMMARY` and the uploaded artifacts.

## 9. First-run checklist

1. Install prerequisites (section 1); register the runner interactively
   (section 2) with labels `Windows, X64, D3D12, Vulkan`.
2. Populate `PRIMAL_CI_ASSET_DIR` (section 4) and set the env vars
   (section 3).
3. Trigger `workflow_dispatch` once: expect configure/build/shader/units to
   pass, all four f350 groups to run, and the final "Gate verdict" step to be
   red: on the first run the harness baseline is empty (the gate prints the
   exact seed command), and the parity block reports 34.43 dB / missing
   D3D12 captures - both below the frozen gates.
4. Seed the baseline on this build (section 5) if - and only if - the 18
   harness runs all exited 0.
5. Re-trigger; from now on the harness byte gate is live and the only
   expected red is the f350/PSNR parity block above (the "Gate verdict" step
   turns the job red; per-gate detail is in the step summary and the
   uploaded artifacts).
