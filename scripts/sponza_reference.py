#!/usr/bin/env python3
"""Run a bounded, locked-camera Sponza reference capture (macOS)."""
import argparse
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
    args = parser.parse_args()
    if args.frame < 1 or args.timeout <= 0:
        parser.error("frame and timeout must be positive")
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
    expected_size = None
    succeeded = False
    with log.open("wb") as stream:
        process = subprocess.Popen([str(binary)], cwd=root, env=env,
                                   stdout=stream, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline and process.poll() is None:
                if capture.exists():
                    with capture.open("rb") as image:
                        magic, size, maximum = image.readline(), image.readline(), image.readline()
                        if magic == b"P6\n" and maximum == b"255\n":
                            width, height = map(int, size.split())
                            expected_size = image.tell() + width * height * 3
                    if expected_size and capture.stat().st_size == expected_size:
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
