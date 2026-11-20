#!/usr/bin/env bash
# Opt-in native measured DDGI producer and strict SPGI world/spatial consumers.
# Accepted bounded Sponza visual/motion reference, 2026-11-19.
# Historical acceptance and limitations are documented in Google Docs:
# https://docs.google.com/document/d/1VzGj_8t_PlDNxyefuI2duc-zAr_S7Hhs64e_LYqBaoI
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
export PRIMAL_METAL_MEASURED_GEOMETRY=1
export PRIMAL_METAL_GATHER_GEOMETRY="${PRIMAL_METAL_GATHER_GEOMETRY:-1}"
# Match the frozen Vulkan receiver grid: logical viewport / 2 base, then
# half-resolution Gather (320x180 at a 1280x720 logical viewport).
if [[ "$PRIMAL_METAL_GATHER_GEOMETRY" == 1 ]]; then
  export PRIMAL_DDGI_GATHER_BASE="${PRIMAL_DDGI_GATHER_BASE:-logical}"
  export PRIMAL_DDGI_GATHER_BASE_DIV="${PRIMAL_DDGI_GATHER_BASE_DIV:-2}"
fi
export DDGI_RAY_SAMPLE=7
export DDGI_MEASURED_GEOMETRY=1
export PRIMAL_SC_OWNER_CARD_DIRECTORY=1
exec bash "$root/scripts/run_metal_sponza_reference.sh" "$@"
