#!/usr/bin/env bash
# Reproducible standalone Dawn dependency for native WGSL validation on macOS.
# Does not configure or overwrite the engine's Metal/Vulkan output trees.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
revision="$(cat "$root/scripts/dawn_native_revision.txt")"
source_dir="$root/third_party/dawn"
build_dir="$root/Darwin/DawnDependency"
if [[ ! -e "$source_dir" ]]; then
  git init "$source_dir"
  git -C "$source_dir" remote add origin https://github.com/google/dawn.git
  git -C "$source_dir" fetch --depth 1 origin "$revision"
  git -C "$source_dir" checkout --detach FETCH_HEAD
fi
if [[ "$(git -C "$source_dir" rev-parse HEAD)" != "$revision" ]]; then
  printf 'Existing Dawn checkout differs from pinned revision %s; leaving it untouched.\n' "$revision" >&2
  exit 2
fi
cmake -S "$source_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release \
  -DDAWN_FETCH_DEPENDENCIES=ON -DDAWN_BUILD_TESTS=OFF -DDAWN_BUILD_SAMPLES=OFF \
  -DDAWN_BUILD_PROTOBUF=OFF -DDAWN_USE_GLFW=OFF -DDAWN_ENABLE_VULKAN=OFF \
  -DDAWN_ENABLE_DESKTOP_GL=OFF -DDAWN_ENABLE_OPENGLES=OFF \
  -DTINT_BUILD_TESTS=OFF -DTINT_BUILD_SPV_READER=OFF -DTINT_BUILD_SPV_WRITER=OFF \
  -DTINT_BUILD_GLSL_WRITER=OFF -DTINT_BUILD_HLSL_WRITER=OFF
cmake --build "$build_dir" --target webgpu_dawn -j "${PRIMAL_BUILD_JOBS:-8}"
