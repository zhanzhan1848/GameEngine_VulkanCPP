#!/usr/bin/env python3
"""Run a bounded, locked-camera Sponza reference capture (macOS)."""
import argparse
import json
import math
import os
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("backend", choices=("metal", "vulkan"))
    parser.add_argument("--output", type=Path, default=Path("/tmp/sponza-reference"))
    parser.add_argument("--frame", type=int, default=360)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--min-runtime", type=float, default=0, help="keep rendering this many seconds for regression checks")
    parser.add_argument("--camera", help="x,y,z,pitch,yaw; overrides the reference camera")
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
    if args.camera:
        env["SPGI_CAM"] = args.camera
    if args.mode != "full":
        if args.backend != "vulkan":
            parser.error("lighting diagnostics currently require Vulkan")
        env["GI_ALL_OFF"] = "1"
        if args.mode == "unshadowed":
            env["PRIMAL_SHADOW_WHITE"] = "1"
        elif args.mode != "direct":
            env["PRIMAL_LIGHTING_DEBUG"] = args.mode
    (output / "capture.json").write_text(json.dumps({
        "backend": args.backend, "mode": args.mode, "frame": args.frame,
        "camera": args.camera, "default_lighting": args.default_lighting,
        "min_runtime": args.min_runtime,
        "environment": {k: v for k, v in env.items() if k.startswith(("SPONZA_", "SPGI_", "GI_", "PRIMAL_LIGHT", "PRIMAL_SHADOW", "PRIMAL_PROBE_"))},
    }, indent=2) + "\n")
    expected_size = None
    succeeded = False
    with log.open("wb") as stream:
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
                        if time.monotonic() - started >= args.min_runtime:
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
