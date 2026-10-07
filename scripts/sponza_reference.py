#!/usr/bin/env python3
"""Run a bounded, locked-camera Sponza reference capture (macOS)."""
import argparse
import hashlib
import json
import math
import re
import os
from pathlib import Path
import subprocess
import time


def readbacks_ready(paths, started_wall):
    """Require fresh, explicitly completed metadata; a finished PPM is separate."""
    for path in paths:
        try:
            if path.stat().st_mtime < started_wall:
                return False
            record = json.loads(path.read_text())
            if not isinstance(record, dict):
                return False
            if record.get('complete') is False or record.get('readback_completed') is False or record.get('completion_checked') is False:
                return False
            if record.get('complete') is not True and record.get('readback_completed') is not True:
                return False
        except (OSError, ValueError):
            return False
    return True


def _sha256(path):
    """P0-A: file hash (hex, first 16 chars) for capture provenance."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()[:16]


def _parse_rgb(value):
    """'r,g,b' -> [r,g,b] (non-negative floats), else None. Mirrors SponzaReference.h::ReadRGB."""
    try:
        parts = [float(v) for v in value.split(",")]
    except ValueError:
        return None
    if len(parts) != 3 or not all(map(math.isfinite, parts)) or any(v < 0 for v in parts):
        return None
    return parts


def _env_float(env, name, default):
    try:
        return float(env.get(name, default))
    except (TypeError, ValueError):
        return default


# sun-audit P0: 序列化"解析后的有效参数"。旧 capture.json 只记环境变量白名单，
# 漏 PRIMAL_DIRECT_LIGHT / PRIMAL_SC_DIRECT_SCALE 时被误读成"功能关闭"。
# 这里按 SponzaReference.h + 测试初始化的同一解析顺序推导（默认值 → env 覆盖），
# 运行后另有 engine_reported（二进制 [SunContract] 真值：含曝光 UBO 实际值、
# view/projection 矩阵与各 pass 尺寸）做交叉核对。
def effective_params(env, args):
    reference = env.get("SPONZA_REFERENCE") == "1" and not args.default_lighting
    if reference:
        params = {"light_direction": [0.12, -0.97, 0.22],
                  "direct_light_color": [3.0, 2.64, 2.16],
                  "ddgi_probe_light_color": [4.5, 3.96, 3.24],
                  "camera": [-12.5, 1.7, -0.7, 14.0, 12.0]}
    else:
        params = {"light_direction": [0.5, -0.7, 0.3],
                  "direct_light_color": [3.0, 3.0, 3.0],
                  "ddgi_probe_light_color": [5.0, 5.0, 5.0],
                  "camera": [0.0, 5.0, 0.0, 0.0, 0.0]}
    if value := env.get("SPGI_CAM"):
        try:
            camera = [float(x) for x in value.split(",")]
            if len(camera) == 5 and all(map(math.isfinite, camera)):
                params["camera"] = camera
        except ValueError:
            pass
    if value := env.get("PRIMAL_LIGHT_DIR"):
        try:
            v = [float(x) for x in value.split(",")]
            if len(v) == 3 and all(map(math.isfinite, v)) and sum(x * x for x in v) > 1e-8:
                params["light_direction"] = v
        except ValueError:
            pass
    for key, target in (("PRIMAL_DIRECT_LIGHT", "direct_light_color"),
                        ("PRIMAL_DDGI_LIGHT", "ddgi_probe_light_color")):
        if value := env.get(key):
            if rgb := _parse_rgb(value):
                params[target] = rgb
    params["sc_direct_scale"] = float(env.get("PRIMAL_SC_DIRECT_SCALE", 50.0))
    params["sun_angular_diameter_deg"] = float(env.get("PRIMAL_SUN_ANGULAR_DIAMETER", 0.5357))
    params["camera_fov_degrees"] = _env_float(env, "SPGI_FOV", 75.0 if reference else 60.0)
    params["floor_albedo_scale"] = _env_float(
        env, "SPONZA_FLOOR_ALBEDO_SCALE", 0.5 if reference and args.backend == "vulkan" else 1.0)
    params["shadow_mode"] = ("white" if args.mode == "unshadowed"
                             else args.mode if args.mode not in ("full", "direct")
                             else "pipeline-default")
    # 2026-09-17 用户反馈后 bloom 默认关（PRIMAL_BLOOM=1 显式开）
    params["bloom_default"] = env.get("PRIMAL_BLOOM") == "1"
    params["froxel_fog"] = env.get("FROXEL") == "1"
    # sun-audit P2: 材质消融（非法值按引擎语义记 off）
    ablation = env.get("PRIMAL_MATERIAL_ABLATION", "")
    params["material_ablation"] = (
        ablation if ablation in ("gray_albedo", "flat_normal", "fixed_roughness") else "off")
    # 调色/曝光：保留旧 presence 字段供历史对比，effective 字段按
    # ToneMappingPass 的当前规则记录（Vulkan 默认开，=0 关闭）。曝光 UBO 实际值
    # = 路径基值 × 2^EV，路径基值按平台在引擎内决定 → 见 engine_reported。
    params["tone"] = {
        "ev": _env_float(env, "PRIMAL_TONE_EV", 0.0),
        "adapt_presence_on": "PRIMAL_TONE_ADAPT" in env,
        "adapt_effective_on": args.backend == "vulkan" and env.get("PRIMAL_TONE_ADAPT") != "0",
        "aces_forced": "PRIMAL_TONE_ACES" in env,
        "passthrough_forced": "PRIMAL_TONE_PASSTHROUGH" in env,
        "contrast": _env_float(env, "PRIMAL_TONE_CONTRAST", 1.0),
        "saturation": _env_float(env, "PRIMAL_TONE_SAT", 1.15),
        "vignette": _env_float(env, "PRIMAL_TONE_VIGNETTE", 0.45),
        "key": _env_float(env, "PRIMAL_TONE_KEY", 0.45),
        "bloom_intensity": _env_float(env, "PRIMAL_BLOOM_INTENSITY", 4.0),
    }
    return params


def merge_engine_reported(capture_path, log_path, ppm_path=None):
    """P0: 把二进制 [SunContract] 单行 JSON 并入 capture.json（engine_reported）。

    引擎真值优先于脚本侧推导：它反映实际进入 UBO 的数值（含测试内代码
    路径的任何后续修改）。引擎在第 1 帧与第 30 帧各输出一行（首帧保证最短
    采集也有 provenance，第 30 帧是 tone 上游标志/DDGI volume 就绪的稳态值）
    ——取最后一行。另记录实际采集分辨率（PPM 头真值）。旧二进制没有该行时
    保留脚本推导，标注 null。"""
    try:
        capture = json.loads(capture_path.read_text())
    except (OSError, json.JSONDecodeError):
        return
    reported = None
    try:
        for line in log_path.read_text(errors="replace").splitlines():
            if line.startswith("[SunContract] "):
                reported = json.loads(line[len("[SunContract] "):])
        capture["engine_reported"] = reported
    except (OSError, json.JSONDecodeError):
        capture["engine_reported"] = None
    # GPU names need not distinguish ICDs; use the engine's selected device.
    driver_pattern = re.compile(
        r"\[VulkanDevice\] Driver: driver_id=(\d+) api_version=(\d+) "
        r"driver_version=(\d+) vendor_id=(\d+) device_id=(\d+) name=(.*?) info=(.*)"
    )
    try:
        matches = list(driver_pattern.finditer(log_path.read_text(errors="replace")))
        if matches:
            match = matches[-1]
            keys = ("driver_id", "api_version", "driver_version", "vendor_id", "device_id")
            driver = {key: int(match.group(i + 1)) for i, key in enumerate(keys)}
            driver.update(name=match.group(6), info=match.group(7), source="engine_selected_physical_device")
            capture.setdefault("provenance", {})["selected_vulkan_driver"] = driver
    except OSError:
        pass
    if ppm_path and ppm_path.is_file():
        try:
            with ppm_path.open("rb") as f:
                magic = f.readline()
                if magic == b"P6\n":
                    capture["capture_size"] = [int(v) for v in f.readline().split()]
        except (OSError, ValueError):
            pass
    capture_path.write_text(json.dumps(capture, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("backend", choices=("metal", "vulkan"))
    parser.add_argument("--output", type=Path, default=Path("/tmp/sponza-reference"))
    parser.add_argument("--frame", type=int, default=360)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--min-runtime", type=float, default=0, help="keep rendering this many seconds for regression checks")
    parser.add_argument("--readback-json", type=Path, action="append", default=[], help="wait for each fresh JSON completion record before terminating; repeat for independent readbacks")
    parser.add_argument("--camera", help="x,y,z,pitch,yaw; overrides the reference camera")
    parser.add_argument("--fov", type=float, help="vertical field of view in degrees (30..120)")
    parser.add_argument("--default-lighting", action="store_true", help="use test-scene sunlight instead of reference lighting")
    parser.add_argument("--mode", choices=("full", "direct", "unshadowed", "normal", "ndotl", "shadow", "albedo"), default="full")
    args = parser.parse_args()
    if args.frame < 1 or args.timeout <= 0 or not 0 <= args.min_runtime < args.timeout:
        parser.error("frame and timeout must be positive")
    if args.camera:
        try:
            camera = [float(v) for v in args.camera.split(",")]
            if len(camera) != 5 or not all(map(math.isfinite, camera)):
                raise ValueError()
        except ValueError:
            parser.error("camera must contain five finite numbers: x,y,z,pitch,yaw")
    if args.fov is not None and (not math.isfinite(args.fov) or not 30 <= args.fov <= 120):
        parser.error("fov must be finite and in the range 30..120 degrees")
    root = Path(__file__).resolve().parents[1]
    name = "TestModularPipeline" if args.backend == "metal" else "TestVulkanSponzaRenderGraph"
    # Both CMake trees currently emit their executables into Darwin/Debug.
    binary = root / "Darwin/Debug" / name
    if not binary.is_file():
        parser.error(f"build {name} first: {binary}")
    output = args.output.resolve() / f"{args.backend}-{time.time_ns()}"
    output.mkdir(parents=True)
    capture = output / "frame.ppm"
    log = output / "render.log"
    env = dict(os.environ, SPONZA_REFERENCE="1", SPGI_CAM_LOCK="1",
               SPONZA_CAPTURE=str(capture), SPONZA_CAPTURE_FRAME=str(args.frame))
    if args.default_lighting:
        env["SPONZA_REFERENCE"] = "0"
    # The repaired SC offline-SDF transport removes false floor bounce, so
    # auto exposure raises the whole reference frame by ~0.15 EV. Calibrate
    # only this locked Vulkan reference capture; never change engine defaults
    # or override a caller-provided exposure.
    if args.backend == "vulkan" and env["SPONZA_REFERENCE"] == "1":
        env.setdefault("PRIMAL_TONE_EV", "-0.15")
    if args.camera:
        env["SPGI_CAM"] = args.camera
    if args.fov is not None:
        env["SPGI_FOV"] = str(args.fov)
    if args.mode != "full":
        if args.backend != "vulkan":
            parser.error("lighting diagnostics currently require Vulkan")
        env["GI_ALL_OFF"] = "1"
        if args.mode == "unshadowed":
            env["PRIMAL_SHADOW_WHITE"] = "1"
        elif args.mode != "direct":
            env["PRIMAL_LIGHTING_DEBUG"] = args.mode
    # P0-A（sponza-gap-audit §6）: 白名单覆盖全部表现层开关。capture.json
    # 的 environment 只记录会进入子进程的有效变量；缺白名单会让 metadata
    # 缺字段被误读成"功能关闭"。GI_ISOLATE 命中 "GI_" 前缀；SPGI_ 前缀
    # 覆盖 SPGI_CAM/CAM_LOCK 与诊断开关。
    # P0-A provenance: which binary x which assets x which shaders produced
    # this capture — makes every capture reproducible/attributable.
    provenance = {"binary_sha256_16": _sha256(binary),
                  "binary_mtime": int(binary.stat().st_mtime)}
    try:
        head = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=root,
                              capture_output=True, text=True, check=True).stdout.strip()
        dirty = subprocess.run(["git", "status", "--porcelain"], cwd=root,
                               capture_output=True, text=True, check=True).stdout
        provenance["git_head"] = head
        provenance["git_dirty_files"] = len(dirty.splitlines())
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass
    assets_dir = root / "EngineTest/assets"
    asset_paths = []
    # P0-A: 模型路径与测试同源——PRIMAL_TEST_MODEL 可替换消费的 .model
    model_name = os.environ.get("PRIMAL_TEST_MODEL", "Sponza.model")
    model = assets_dir / model_name
    if model.is_file():
        asset_paths.append(model)
    sponza_models = assets_dir / "models/Sponza"
    if sponza_models.is_dir():
        asset_paths.extend(sorted(p for p in sponza_models.iterdir() if p.suffix == ".png"))
    provenance["assets"] = {str(p.relative_to(root)): _sha256(p) for p in asset_paths}
    spv_dir = root / "Engine/Graphics/Vulkan/shaders"
    if spv_dir.is_dir():
        provenance["shaders"] = {str(p.relative_to(root)): _sha256(p)
                                 for p in sorted(spv_dir.rglob("*.spv"))}
    # sun-audit P0: 已消费 HDR/SDF 清单（测试 InitializeIBL / OfflineSDFMerger
    # 的实际输入；存在才记，缺失本身也是 provenance——IBL/SDF 降级会被读到）。
    consumed = {}
    hdr = assets_dir / "textures/hdr/sunset.hdr"
    consumed[str(hdr.relative_to(root))] = _sha256(hdr) if hdr.is_file() else None
    sdf_cache = assets_dir / "Sponza_global_sdf.v3.cache"
    consumed[str(sdf_cache.relative_to(root))] = _sha256(sdf_cache) if sdf_cache.is_file() else None
    provenance["consumed_env_inputs"] = consumed
    # Record the selected SC files even when they live outside the repository.
    # These are declared inputs; successful SC upload/snapshot audits separately
    # establish consumption. Do not hash the default asset instead of an override.
    declared_sc = {}
    for name in ("PRIMAL_SC_STATIC_PAGES", "PRIMAL_SC_BOUNDARY_SAMPLES"):
        value = env.get(name)
        if value:
            path = Path(value)
            if not path.is_absolute():
                path = root / path
            declared_sc[name] = {"path": str(path.resolve()), "present": path.is_file(),
                                 "sha256_16": _sha256(path) if path.is_file() else None}
    provenance["declared_sc_file_inputs"] = declared_sc

    # sun-audit P0: 编译参数真值——从二进制所在构建树的 CMakeCache 读取
    # （编译器/构建类型/flags/Vulkan 开关），使"源码 hash + 二进制 hash"可
    # 追溯到构建配置。
    build_config = {}
    cache = binary.parent / "CMakeCache.txt"
    if cache.is_file():
        wanted = {"CMAKE_BUILD_TYPE", "CMAKE_CXX_COMPILER", "CMAKE_CXX_COMPILER_ID",
                  "CMAKE_CXX_FLAGS", "CMAKE_CXX_FLAGS_DEBUG",
                  "CMAKE_CXX_FLAGS_RELWITHDEBINFO", "ENABLE_VULKAN",
                  "CMAKE_OSX_ARCHITECTURES"}
        try:
            for line in cache.read_text(errors="replace").splitlines():
                # CMakeCache 行格式 KEY:TYPE=VALUE——剥掉 :TYPE 再匹配
                if "=" in line:
                    k, v = line.split("=", 1)
                    if k.split(":", 1)[0] in wanted:
                        build_config[k.split(":", 1)[0]] = v
        except OSError:
            pass
    provenance["build_config"] = build_config
    env_prefixes = (
        "MVK_CONFIG_", "VK_DRIVER_FILES", "VK_ICD_FILENAMES",
        "SC_",  # Surface Cache diagnostic/continuous inputs passed to the child.
        "SPONZA_", "SPGI_", "GI_", "DDGI_", "PRIMAL_LIGHT", "PRIMAL_SHADOW",
        "PRIMAL_PROBE_",
        # 表现层（audit §4.6–4.9）：体积雾 / 反射 / 调色与曝光 / Bloom / 模糊
        "FROXEL", "SSR", "TONE_OFF", "PRIMAL_TONE_", "PRIMAL_BLOOM",
        "BLUR_OFF",
        # GI 与诊断
        "SSGI_", "PRIMAL_SPGI", "PRIMAL_LIGHTING_DEBUG",
        "PRIMAL_DDGI_",  # T4.3 detail-SDF refine 门控（SPGI/DDGI 共用）
        # RenderGraph / 资源导入诊断
        "PRIMAL_RG_",
        # 测试通道
        "PRIMAL_TEST_",
        # sun-audit P0: 直射色 / SC 标度 / 太阳角直径不在 PRIMAL_LIGHT 前缀内，
        # 漏记会让颜色 A/B 的 metadata 看起来"没改"。
        "PRIMAL_DIRECT", "PRIMAL_SC_", "PRIMAL_SUN_",
        # sun-audit P2: 材质消融诊断开关 / 同源天空 IBL / 环境镜面解耦变体
        "PRIMAL_MATERIAL_", "PRIMAL_SKY_", "PRIMAL_SPEC_OCC",
        # sun-audit P3: RenderGraph pass 计时门控
        "RG_PROF",
    )
    (output / "capture.json").write_text(json.dumps({
        "backend": args.backend, "mode": args.mode, "frame": args.frame,
        "camera": args.camera, "fov": args.fov,
        "default_lighting": args.default_lighting,
        "min_runtime": args.min_runtime,
        "effective_params": effective_params(env, args),
        "environment": {k: v for k, v in env.items() if k.startswith(env_prefixes)},
        "provenance": provenance,
    }, indent=2) + "\n")
    expected_size = None
    succeeded = False
    with log.open("wb") as stream:
        started_wall = time.time()
        process = subprocess.Popen([str(binary)], cwd=root, env=env,
                                   stdout=stream, stderr=subprocess.STDOUT)
        try:
            started = time.monotonic()
            deadline = started + args.timeout
            while time.monotonic() < deadline and process.poll() is None:
                if capture.exists():
                    with capture.open("rb") as image:
                        magic, size, maximum = image.readline(), image.readline(), image.readline()
                        if magic == b"P6\n" and maximum == b"255\n":
                            width, height = map(int, size.split())
                            expected_size = image.tell() + width * height * 3
                    if expected_size and capture.stat().st_size == expected_size:
                        if time.monotonic() - started >= args.min_runtime and readbacks_ready(args.readback_json, started_wall):
                            succeeded = True
                            break
                time.sleep(0.5)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    # P0: 无论成败都并入引擎侧真值——失败采集的 provenance 同样要可核查。
    merge_engine_reported(output / "capture.json", log, output / "frame.ppm")
    metadata = json.loads((output / 'capture.json').read_text())
    metadata['readback_wait'] = dict(required=[str(p.resolve()) for p in args.readback_json], ready=readbacks_ready(args.readback_json, started_wall), capture_succeeded=succeeded, scope='fresh JSON completion only; individual audits must verify frame/source identity')
    (output / 'capture.json').write_text(json.dumps(metadata, indent=2)+'\n')
    if not succeeded:
        print(f"Capture failed or timed out; inspect {log}")
        return 1
    png = output / "frame.png"
    converted = subprocess.run(["sips", "-s", "format", "png", str(capture), "--out", str(png)],
                               stdout=subprocess.DEVNULL, check=False)
    print(png if converted.returncode == 0 else capture)
    print(f"Log: {log}")
    errors = log.read_text(errors="replace").count("[Vulkan validation] ERROR:")
    if errors:
        print(f"Rendering has {errors} validation error messages; capture success is not a clean validation pass.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
