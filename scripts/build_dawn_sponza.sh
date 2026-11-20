#!/usr/bin/env bash
# Isolated native Dawn engine build. Debug/DebugVulkan outputs stay untouched.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
revision="$(cat "$root/scripts/dawn_native_revision.txt")"
if [[ ! -d "$root/third_party/dawn" ]]; then
  bash "$root/scripts/setup_dawn_native.sh"
fi
[[ "$(git -C "$root/third_party/dawn" rev-parse HEAD)" == "$revision" ]] || {
  printf 'Dawn checkout differs from the pinned revision; leaving it untouched.\n' >&2
  exit 2
}
build="$root/Darwin/DawnNativeBuild"
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_VULKAN=OFF -DENABLE_WEBGPU=ON -DBUILD_SHARED_LIBS=OFF \
  -DDAWN_FETCH_DEPENDENCIES=ON -DDAWN_BUILD_TESTS=OFF -DDAWN_BUILD_SAMPLES=OFF \
  -DDAWN_BUILD_PROTOBUF=OFF -DDAWN_USE_GLFW=OFF -DDAWN_ENABLE_VULKAN=OFF \
  -DDAWN_ENABLE_DESKTOP_GL=OFF -DDAWN_ENABLE_OPENGLES=OFF \
  -DTINT_BUILD_TESTS=OFF -DTINT_BUILD_SPV_READER=OFF -DTINT_BUILD_SPV_WRITER=OFF \
  -DTINT_BUILD_GLSL_WRITER=OFF -DTINT_BUILD_HLSL_WRITER=OFF
cmake --build "$build" --target TestDawnReadback TestDawnMaterialTextures TestDawnShadows TestDawnShadowFit TestDawnSponza -j "${PRIMAL_BUILD_JOBS:-8}"
