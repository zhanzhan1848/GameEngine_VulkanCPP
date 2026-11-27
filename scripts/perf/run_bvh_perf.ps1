# BVH perf driver for the Windows self-hosted runner (Vulkan backend).
# Runs TestVulkanSponzaRenderGraph timing-only groups and emits logs + a
# summary JSON consumed by scripts/perf/parse_frame_timing.py.
#
# Mirrors scripts/perf/run_bvh_perf.sh (same groups / env / contract anchors)
# and reuses scripts/win-ci/run_f350.ps1's two-layer watchdog idea:
#   1. engine-internal: PRIMAL_SPONZA_WATCHDOG_SECS=300 (exit 7 on stall);
#   2. script-level: kill if stderr/stdout silent for -SilenceSecs, or the
#      total per-group budget -HardCapSecs elapses.
#
# Usage:
#   powershell -File scripts/perf/run_bvh_perf.ps1 -Binary <exe> `
#     [-OutDir ci-out\bvh-perf] [-Groups gi0,dyn-gi,dyn-gi-sah,dyn-gi-upper] `
#     [-Frames 420] [-SilenceSecs 300] [-HardCapSecs 900]
param(
    [Parameter(Mandatory = $true)][string]$Binary,
    [string]$OutDir = 'bvh-out',
    [string]$Groups = 'gi0,dyn-gi,dyn-gi-sah,dyn-gi-upper,dyn-gi-lbvh,dyn-gi-hlbvh',
    [int]$Frames = 420,
    [int]$SilenceSecs = 300,
    [int]$HardCapSecs = 900
)

# EAP=Stop 会让 native 子进程的任何 stderr 行（经 2>&1）变成终止 throw
# （2026-11-27 run 46017993781：解析器 GBK stdout traceback → 全脚本静默死）。
$ErrorActionPreference = 'Continue'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$OutDir = Join-Path $repoRoot $OutDir
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

if (-not (Test-Path $Binary)) { Write-Error "binary not found: $Binary" }
$binarySha = (Get-FileHash -Algorithm SHA256 $Binary).Hash.ToLower()
$gitSha = (git -C $repoRoot rev-parse HEAD).Trim()

$gpuInfo = 'unknown'
try {
    $smi = & nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>$null
    if ($LASTEXITCODE -eq 0 -and $smi) { $gpuInfo = ($smi -join '; ') }
} catch { }

# Env vars that must never leak between groups (f350 clear list + capture knobs).
$clearList = @(
    'PRIMAL_EXPERIMENT_ENV_SAH', 'PRIMAL_DDGI_GEOMETRY_UPPER_BVH',
    'DDGI_RAY_SAMPLE', 'DDGI_MEASURED_GEOMETRY', 'SPGI_DDGI_VISIBILITY',
    'PRIMAL_DDGI_GEOMETRY_VISIBILITY', 'PRIMAL_DDGI_GEOMETRY_QUERY_KERNEL',
    'PRIMAL_SC_OWNER_CARD_DIRECTORY', 'PRIMAL_VK_ALBEDO_MIPS',
    'PRIMAL_DDGI_TRAVERSAL_AUDIT', 'PRIMAL_SPGI_SKIP', 'GI_ISOLATE',
    'PRIMAL_ENV_SDF_HYBRID', 'PRIMAL_ENV_CROSS_RAY_PAIR', 'PRIMAL_ENV_TRACE_WORKGROUP',
    'PRIMAL_FRAME_TIMING_ALL', 'RG_PROF', 'PRIMAL_VK_QUERY_NONBLOCKING',
    'PRIMAL_VK_PASS_TIMESTAMP_AUDIT', 'PRIMAL_LIGHTING_DEBUG',
    'SPONZA_CAPTURE', 'SPONZA_CAPTURE_FRAME', 'SPONZA_CAPTURE_DIR', 'SPONZA_CAPTURE_FRAMES',
    'SPONZA_DIAGNOSTIC_CAPTURE', 'SPONZA_CAM_SCRIPT', 'SPONZA_REFERENCE',
    'ENV_SAH_INTERLEAVED', 'RHI_TEST_PLATFORM'
)

function Clear-PerfEnv {
    foreach ($name in $clearList) {
        Remove-Item -Path "Env:$name" -ErrorAction SilentlyContinue | Out-Null
    }
}

function Set-DynGiEnv {
    $env:DDGI_RAY_SAMPLE = '7'
    $env:DDGI_MEASURED_GEOMETRY = '1'
    $env:SPGI_DDGI_VISIBILITY = '2'
    $env:PRIMAL_DDGI_GEOMETRY_VISIBILITY = '1'
    $env:PRIMAL_DDGI_GEOMETRY_QUERY_KERNEL = '1'
    $env:PRIMAL_SC_OWNER_CARD_DIRECTORY = '1'
    $env:PRIMAL_VK_ALBEDO_MIPS = '1'
}

function Run-Group([string]$name) {
    Clear-PerfEnv
    switch ($name) {
        'gi0' { }
        { $_ -in 'dyn-gi', 'dyn-gi-sah', 'dyn-gi-upper', 'dyn-gi-lbvh', 'dyn-gi-hlbvh' } {
            Set-DynGiEnv
            if ($_ -eq 'dyn-gi-sah') { $env:PRIMAL_EXPERIMENT_ENV_SAH = '1' }
            if ($_ -eq 'dyn-gi-upper') { $env:PRIMAL_DDGI_GEOMETRY_UPPER_BVH = '1' }
            # Unified builder switch (ExperimentalBVHSelection.h); the sah group
            # keeps the legacy knob so the old path stays exercised.
            if ($_ -eq 'dyn-gi-lbvh') { $env:PRIMAL_GEOMETRY_BVH_IMPL = 'lbvh' }
            if ($_ -eq 'dyn-gi-hlbvh') { $env:PRIMAL_GEOMETRY_BVH_IMPL = 'hlbvh' }
        }
        default {
            Write-Host "[run_bvh_perf] unknown group: $name"
            return 9
        }
    }

    $env:PRIMAL_FRAME_TIMING = '1'
    $env:SPONZA_TIMING_ONLY = '1'
    $env:SPONZA_MAX_FRAMES = "$Frames"
    $env:SPGI_CAM_LOCK = '1'
    $env:SPGI_CAM = '-12.5,1.7,-0.7,14,2'
    $env:PRIMAL_SPONZA_WATCHDOG_SECS = '300'

    $logPath = Join-Path $OutDir "$name.log"
    Write-Host "[run_bvh_perf] group=$name frames=$Frames binary=$Binary"

    $proc = Start-Process -FilePath $Binary -WorkingDirectory $repoRoot `
        -RedirectStandardOutput $logPath -RedirectStandardError "$logPath.err" `
        -PassThru -NoNewWindow
    $started = Get-Date
    $lastOutputBytes = -1
    $lastProgress = $started
    $exitCode = $null

    while ($true) {
        Start-Sleep -Seconds 5
        if ($proc.HasExited) { $exitCode = $proc.ExitCode; break }
        $now = Get-Date
        if (($now - $started).TotalSeconds -gt $HardCapSecs) {
            # Kill(bool) 树杀重载在 Windows PowerShell 5.1/.NET Framework 不存在，
            # 看门狗路径自身会炸——统一 taskkill 树杀。
            Write-Host "::error::watchdog hardcap group=$name secs=$HardCapSecs"
            taskkill /PID $proc.Id /T /F 2>$null | Out-Null
            try { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue } catch { }
            $exitCode = -1
            break
        }
        $bytes = 0
        foreach ($f in @($logPath, "$logPath.err")) {
            if (Test-Path $f) { $bytes += (Get-Item $f).Length }
        }
        if ($bytes -ne $lastOutputBytes) {
            $lastOutputBytes = $bytes
            $lastProgress = $now
        } elseif (($now - $lastProgress).TotalSeconds -gt $SilenceSecs) {
            Write-Host "::error::watchdog silent group=$name secs=$SilenceSecs"
            taskkill /PID $proc.Id /T /F 2>$null | Out-Null
            try { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue } catch { }
            $exitCode = -2
            break
        }
    }

    $anchorOk = $false
    if (Test-Path $logPath) {
        $hit = Select-String -Path $logPath -Pattern "SPONZA_MAX_FRAMES=$Frames reached" -SimpleMatch:$false
        if ($hit) { $anchorOk = $true }
    }
    Write-Host ("[run_bvh_perf] group={0} exit={1} anchor={2}" -f $name, $exitCode, $anchorOk)
    $exitClean = ($null -eq $exitCode -or $exitCode -eq 0)
    if ($exitClean -and $anchorOk) { return 0 }
    if (-not $anchorOk) { Write-Host "::error::anchor-missing group=$name exit=$exitCode" }
    else { Write-Host "::error::group-failed $name exit=$exitCode" }
    # 失败组的日志尾巴打注解（免贴日志通道，对齐 configure 步骤）。
    if (Test-Path "$logPath.err") {
        Get-Content "$logPath.err" -ErrorAction SilentlyContinue | Select-Object -Last 4 |
          ForEach-Object { Write-Host "::error::gerr[$name] $_" }
    }
    Get-Content $logPath -ErrorAction SilentlyContinue | Select-Object -Last 4 |
      ForEach-Object { Write-Host "::error::gout[$name] $_" }
    return 1
}

Set-Content -Path (Join-Path $OutDir 'meta.txt') -Value @"
generated_utc=$((Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))
binary=$Binary
binary_sha256=$binarySha
git_sha=$gitSha
frames=$Frames
groups=$Groups
gpu=$gpuInfo
"@

$groupList = $Groups -split ','
$failed = @()
foreach ($g in $groupList) {
    $rc = Run-Group $g
    if ("$rc" -ne "0") {
        $failed += "$g(rc=$rc)"
        Write-Host "::error::group-failed-summary $g rc=$rc"
    }
}

$meta = Get-Content (Join-Path $OutDir 'meta.txt') -Raw
$envArgs = @()
foreach ($g in $groupList) { $envArgs += @('--log', "$g=" + (Join-Path $OutDir "$g.log")) }
$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) { $python = Get-Command py -ErrorAction SilentlyContinue }
if (-not $python) { Write-Host '::error::python not found on runner'; exit 1 }
$pyOut = & $python.Source "$repoRoot/scripts/perf/parse_frame_timing.py" @envArgs `
    --warm 60 --git-sha $gitSha --binary-sha $binarySha `
    --out-json (Join-Path $OutDir 'report.json') --out-md (Join-Path $OutDir 'report.md') 2>&1
$parseCode = $LASTEXITCODE
$pyOut | Out-File (Join-Path $OutDir 'parse.log') -Encoding utf8
Write-Output "python parse exit: $parseCode"
if ($parseCode -ne 0) {
    Get-Content (Join-Path $OutDir 'parse.log') | Select-Object -Last 6 |
      ForEach-Object { Write-Host "::error::parse $_" }
}

Write-Host "::notice::bvh-final failed=$($failed -join '+') parse=$parseCode"
if ($failed.Count -gt 0) {
    Write-Output "[run_bvh_perf] FAILED groups: $($failed -join ', ')"
    exit 1
}
if ($parseCode -ne 0) { exit 1 }
Write-Output "[run_bvh_perf] all groups ok; report: $(Join-Path $OutDir 'report.md')"
exit 0
