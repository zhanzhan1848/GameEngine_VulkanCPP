#!/usr/bin/env bash
# Shared implementation for measured, tangent-frame and geometry-visibility
# candidates. Compatibility symlink names select the original profiles.
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
profile="$(basename -- "${BASH_SOURCE[0]}")"
case "$profile" in
  run_vulkan_sponza_geometry_visibility.sh)
    printf '[Vulkan GI geometry visibility] exact opaque connection queries enabled; additional GPU cost.\n' >&2
    export PRIMAL_DDGI_GEOMETRY_VISIBILITY=1
    ;;
  run_vulkan_sponza_tangent_frame.sh|run_vulkan_sponza_measured.sh) ;;
  *) printf 'Unknown Vulkan Sponza profile: %s\n' "$profile" >&2; exit 2 ;;
esac
if [[ "$profile" != run_vulkan_sponza_measured.sh ]]; then
  model="${PRIMAL_TEST_MODEL:-SponzaTangentFrames.model}"
  if [[ ! -f "$repo_root/EngineTest/assets/$model" ]]; then
    printf 'Regenerate %s with ContentToolsCLI before running the tangent-frame candidate.\n' "$model" >&2
    exit 2
  fi
  printf '[Vulkan tangent-frame candidate] normal textures enabled; model=%s.\n' "$model" >&2
  export PRIMAL_TEST_MODEL="$model" PRIMAL_VK_TANGENT_FRAME=1
fi
binary="$repo_root/Darwin/Debug/TestVulkanSponzaRenderGraph"
if [[ ! -x "$binary" ]]; then
  printf 'Build TestVulkanSponzaRenderGraph in Darwin/DebugVulkan first.\n' >&2
  exit 2
fi
cd "$repo_root"
printf '[Vulkan GI candidate] measured geometry + owner directory; albedo_mips=%s.\n' "${PRIMAL_VK_ALBEDO_MIPS:-1}" >&2
exec env -u PRIMAL_SPGI_SKIP \
  DDGI_RAY_SAMPLE=7 DDGI_MEASURED_GEOMETRY=1 \
  PRIMAL_SC_OWNER_CARD_DIRECTORY=1 SPGI_DDGI_VISIBILITY=2 \
  PRIMAL_VK_ALBEDO_MIPS="${PRIMAL_VK_ALBEDO_MIPS:-1}" \
  "$binary" "$@"
