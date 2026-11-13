# CI helper: materialize third_party/meshoptimizer (ASCII-only, cp936-safe).
#
# The superproject pins meshoptimizer at 4d218f333e... which is a local-only
# patch commit (authored on the dev machine, never pushed to any remote).
# Fresh CI checkouts cannot materialize it. Engine code and unit tests never
# compile meshopt (ContentTools owns that, and ContentTools is offline in CI);
# the only requirement is the root CMakeLists meshoptimizer.h existence guard.
# Fall back to the pin's nearest reachable upstream ancestor.
#
# Usage: run from the repository root after checkout.

$ErrorActionPreference = 'Stop'
$pin      = '4d218f333e'
$fallback = '26ef1c8be3f554d36163c9f7e4eb7a54823e8735'
$upstream = 'https://github.com/zeux/meshoptimizer.git'

if (Test-Path 'third_party/meshoptimizer/src/meshoptimizer.h') {
  Write-Host 'meshoptimizer already materialized; nothing to do'
  exit 0
}

Write-Host "::warning::meshoptimizer pin $pin is a local-only commit; falling back to upstream $fallback"
New-Item -ItemType Directory -Force 'third_party/meshoptimizer' | Out-Null
if (-not (Test-Path 'third_party/meshoptimizer/.git')) {
  git -C 'third_party/meshoptimizer' init -q
  git -C 'third_party/meshoptimizer' remote add origin $upstream
} else {
  git -C 'third_party/meshoptimizer' remote set-url origin $upstream
}
git -C 'third_party/meshoptimizer' fetch --depth 1 origin $fallback
if ($LASTEXITCODE -ne 0) { Write-Host '::error::meshoptimizer fallback fetch failed'; exit 1 }
git -C 'third_party/meshoptimizer' checkout --detach --force FETCH_HEAD
if ($LASTEXITCODE -ne 0) { Write-Host '::error::meshoptimizer fallback checkout failed'; exit 1 }
if (-not (Test-Path 'third_party/meshoptimizer/src/meshoptimizer.h')) {
  Write-Host '::error::meshoptimizer fallback did not produce src/meshoptimizer.h'
  exit 1
}
Write-Host "meshoptimizer fallback materialized at $fallback"
