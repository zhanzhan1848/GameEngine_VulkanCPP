#!/usr/bin/env bash
# CI 用：物化 third_party/meshoptimizer（ASCII-only，cp936 安全）。
#
# The superproject pins meshoptimizer at 4d218f333e... which is a local-only
# patch commit ("disable cone culling for anti-aligned triangle normals",
# authored on this machine, never pushed to any remote). Fresh CI checkouts
# therefore cannot materialize it. Engine code and unit tests never compile
# meshopt (ContentTools owns that, and ContentTools is offline in CI); the
# only requirement is the root CMakeLists meshoptimizer.h existence guard.
# Fall back to the pin's nearest reachable upstream ancestor.
#
# Usage: run from the repository root after checkout.

set -euo pipefail

PIN="4d218f333e"
FALLBACK="26ef1c8be3f554d36163c9f7e4eb7a54823e8735"
UPSTREAM="https://github.com/zeux/meshoptimizer.git"

if [ -f "third_party/meshoptimizer/src/meshoptimizer.h" ]; then
  echo "meshoptimizer already materialized; nothing to do"
  exit 0
fi

echo "::warning::meshoptimizer pin ${PIN} is a local-only commit; falling back to upstream ${FALLBACK}"
mkdir -p third_party/meshoptimizer
if [ ! -e "third_party/meshoptimizer/.git" ]; then
  git -C third_party/meshoptimizer init -q
  git -C third_party/meshoptimizer remote add origin "${UPSTREAM}"
else
  git -C third_party/meshoptimizer remote set-url origin "${UPSTREAM}"
fi
git -C third_party/meshoptimizer fetch --depth 1 origin "${FALLBACK}"
git -C third_party/meshoptimizer checkout --detach --force FETCH_HEAD
test -f "third_party/meshoptimizer/src/meshoptimizer.h"
echo "meshoptimizer fallback materialized at ${FALLBACK}"
