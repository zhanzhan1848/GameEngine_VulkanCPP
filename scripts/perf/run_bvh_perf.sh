#!/usr/bin/env bash
# BVH perf driver — runs TestVulkanSponzaRenderGraph timing-only groups and
# builds a report via scripts/perf/parse_frame_timing.py.
#
# Groups (sequential, each an isolated process):
#   gi0          no GI knobs — pipeline floor
#   dyn-gi       canonical Dynamic GI (f350 Vulkan GI-on env)
#   dyn-gi-sah   dyn-gi + PRIMAL_EXPERIMENT_ENV_SAH=1 (12-bin SAH BLAS candidate)
#   dyn-gi-upper dyn-gi + PRIMAL_DDGI_GEOMETRY_UPPER_BVH=1 (upper SAH-16 reorder)
#
# Usage:
#   scripts/perf/run_bvh_perf.sh [--frames N] [--groups g1,g2] [--warm N]
#                                [--interleaved] [--build] [--tag NAME]
# Results land in perf-out/<utc-timestamp>[-tag]/ : <group>.log, report.json,
# report.md, meta.txt.
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"

frames=420
groups="gi0,dyn-gi,dyn-gi-sah,dyn-gi-upper"
warm=60
tag=""
do_build=0
interleaved=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --frames) frames="$2"; shift 2 ;;
    --groups) groups="$2"; shift 2 ;;
    --warm) warm="$2"; shift 2 ;;
    --tag) tag="$2"; shift 2 ;;
    --build) do_build=1; shift ;;
    --interleaved) interleaved=1; shift ;;
    *) printf 'unknown option: %s\n' "$1" >&2; exit 3 ;;
  esac
done

if [[ "$do_build" == 1 ]]; then
  # BUILD_CONTENT_TOOLS=OFF: perf targets need only Engine; the ContentTools
  # submodule checks (FBX/VHACD/eigen) are irrelevant to this tree.
  cmake -S "$repo_root" -B "$repo_root/Darwin/DebugVulkan" -DCMAKE_BUILD_TYPE=Debug -DENABLE_VULKAN=ON -DBUILD_CONTENT_TOOLS=OFF
  cmake --build "$repo_root/Darwin/DebugVulkan" --target TestVulkanSponzaRenderGraph -j8
fi

binary="${BVH_PERF_BINARY:-}"
if [[ -z "$binary" ]]; then
  for candidate in \
    "$repo_root/Darwin/DebugVulkan/TestVulkanSponzaRenderGraph" \
    "$repo_root/Darwin/Debug/TestVulkanSponzaRenderGraph"; do
    if [[ -x "$candidate" ]]; then binary="$candidate"; break; fi
  done
fi
if [[ -z "$binary" || ! -x "$binary" ]]; then
  printf 'TestVulkanSponzaRenderGraph not found; build first or set BVH_PERF_BINARY.\n' >&2
  exit 2
fi

stamp="$(date -u +%Y%m%d-%H%M%S)"
out_dir="$repo_root/perf-out/${stamp}${tag:+-$tag}"
mkdir -p "$out_dir"
cd "$repo_root"   # assets and .primal-cache resolve relative to repo root

# Env vars that must never leak between groups (f350 clear list + capture knobs).
clear_list=(
  PRIMAL_EXPERIMENT_ENV_SAH PRIMAL_DDGI_GEOMETRY_UPPER_BVH
  DDGI_RAY_SAMPLE DDGI_MEASURED_GEOMETRY SPGI_DDGI_VISIBILITY
  PRIMAL_DDGI_GEOMETRY_VISIBILITY PRIMAL_DDGI_GEOMETRY_QUERY_KERNEL
  PRIMAL_SC_OWNER_CARD_DIRECTORY PRIMAL_VK_ALBEDO_MIPS
  PRIMAL_DDGI_TRAVERSAL_AUDIT PRIMAL_SPGI_SKIP GI_ISOLATE
  PRIMAL_ENV_SDF_HYBRID PRIMAL_ENV_CROSS_RAY_PAIR PRIMAL_ENV_TRACE_WORKGROUP
  PRIMAL_FRAME_TIMING_ALL RG_PROF PRIMAL_VK_QUERY_NONBLOCKING
  PRIMAL_VK_PASS_TIMESTAMP_AUDIT PRIMAL_LIGHTING_DEBUG
  SPONZA_CAPTURE SPONZA_CAPTURE_FRAME SPONZA_CAPTURE_DIR SPONZA_CAPTURE_FRAMES
  SPONZA_DIAGNOSTIC_CAPTURE SPONZA_CAM_SCRIPT SPONZA_REFERENCE
  ENV_SAH_INTERLEAVED RHI_TEST_PLATFORM
)

run_group() {
  local name="$1"
  local value
  for value in "${clear_list[@]}"; do unset "$value" || true; done

  case "$name" in
    gi0) ;;
    dyn-gi|dyn-gi-sah|dyn-gi-upper)
      export DDGI_RAY_SAMPLE=7
      export DDGI_MEASURED_GEOMETRY=1
      export SPGI_DDGI_VISIBILITY=2
      export PRIMAL_DDGI_GEOMETRY_VISIBILITY=1
      export PRIMAL_DDGI_GEOMETRY_QUERY_KERNEL=1
      export PRIMAL_SC_OWNER_CARD_DIRECTORY=1
      export PRIMAL_VK_ALBEDO_MIPS=1
      [[ "$name" == "dyn-gi-sah" ]] && export PRIMAL_EXPERIMENT_ENV_SAH=1
      [[ "$name" == "dyn-gi-upper" ]] && export PRIMAL_DDGI_GEOMETRY_UPPER_BVH=1
      ;;
    interleaved)
      # Same-process A/B/B/A protocol; needs the extra block budget itself.
      export ENV_SAH_INTERLEAVED=1
      if [[ "$frames" -lt 480 ]]; then frames=480; fi
      ;;
    *) printf 'unknown group: %s\n' "$name" >&2; return 9 ;;
  esac

  export PRIMAL_FRAME_TIMING=1
  export SPONZA_TIMING_ONLY=1
  export SPONZA_MAX_FRAMES="$frames"
  export SPGI_CAM_LOCK=1
  export SPGI_CAM='-12.5,1.7,-0.7,14,2'
  export PRIMAL_SPONZA_WATCHDOG_SECS=300

  printf '[run_bvh_perf] group=%s frames=%s binary=%s\n' "$name" "$frames" "$binary" >&2
  "$binary" > "$out_dir/$name.log" 2>&1
}

binary_sha="$(shasum -a 256 "$binary" | awk '{print $1}')"
git_sha="$(git -C "$repo_root" rev-parse HEAD)"

{
  echo "generated_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "binary=$binary"
  echo "binary_sha256=$binary_sha"
  echo "git_sha=$git_sha"
  echo "frames=$frames"
  echo "groups=$groups"
  echo "cam=SPGI_CAM=-12.5,1.7,-0.7,14,2 SPGI_CAM_LOCK=1"
  echo "host=$(uname -m) $(sysctl -n machdep.cpu.brand_string 2>/dev/null || true)"
} > "$out_dir/meta.txt"

failed_groups=()
IFS=',' read -r -a group_array <<< "$groups"
for g in "${group_array[@]}"; do
  rc=0
  run_group "$g" || rc=$?
  if [[ "$rc" -ne 0 ]]; then
    printf '[run_bvh_perf] group=%s FAILED rc=%s\n' "$g" "$rc" >&2
    failed_groups+=("$g")
    continue
  fi
  if ! grep -q "SPONZA_MAX_FRAMES=$frames reached" "$out_dir/$g.log"; then
    printf '[run_bvh_perf] group=%s missing clean-exit anchor\n' "$g" >&2
    failed_groups+=("$g")
  fi
done

log_args=()
for g in "${group_array[@]}"; do
  log_args+=("--log" "$g=$out_dir/$g.log")
done

# shellcheck disable=SC2086
python3 "$repo_root/scripts/perf/parse_frame_timing.py" \
  "${log_args[@]}" --warm "$warm" \
  $( [[ "$interleaved" == 1 ]] && echo --interleaved ) \
  --git-sha "$git_sha" --binary-sha "$binary_sha" \
  --out-json "$out_dir/report.json" --out-md "$out_dir/report.md"

printf '[run_bvh_perf] report: %s\n' "$out_dir/report.md"
if [[ "${#failed_groups[@]}" -gt 0 ]]; then
  printf '[run_bvh_perf] failed groups: %s\n' "${failed_groups[*]}" >&2
  exit 1
fi
