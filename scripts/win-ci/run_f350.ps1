# run_f350.ps1 - Four TestVulkanSponzaRenderGraph f350 capture groups for the
# Windows parity CI (D3D12 GI-on/GI-off, Vulkan GI-on/GI-off).
#
# Reproduces the official r11 launchers (run_win_d3d12_f350.cmd family):
# same env matrix, same 400-frame run with capture at frame 350, same asset
# gates, same 300 s no-progress watchdog. PRIMAL_VK_HANG_DIAG is explicitly
# NOT set (official default path).
#
# This script never exits non-zero: every group runs to completion (or to its
# watchdog), the per-group status goes into $OutDir\f350_gates.json, and the
# workflow's parity_report step turns those records into pass/fail gates.
#
# Watchdogs (two layers, matching the r11 launcher contract):
#   1. engine-internal: PRIMAL_SPONZA_WATCHDOG_SECS=300 (exit 7 on stall);
#   2. this script: kill the process if its stderr log has not been written
#      for 300 s, or if the group exceeds 1500 s total.
#
# This file is ASCII-only with CRLF line endings (cp936-safe).

param(
    [Parameter(Mandatory = $true)][string]$Repo,
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [int]$NoProgressSecs = 300,
    [int]$HardCapSecs = 1500
)

$ErrorActionPreference = 'Continue'

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

if (-not (Test-Path $Exe)) {
    Write-Output "[f350] FATAL exe not found: $Exe"
    Set-Content -Path (Join-Path $OutDir 'f350_gates.json') -Value '[]' -Encoding UTF8
    exit 0
}

# Ordered group list. gi = 1 -> GI on, 0 -> GI off (GI_ISOLATE=all).
$groups = @(
    @{ name = 'd3d12-gion';  api = 'd3d12';  gi = 1; ppm = 'art2_win_d3d12_f350.ppm'  },
    @{ name = 'd3d12-gioff'; api = 'd3d12';  gi = 0; ppm = 'art2_win_d3d12_gioff_f350.ppm' },
    @{ name = 'vulkan-gion'; api = 'vulkan'; gi = 1; ppm = 'art2_win_vulkan_f350.ppm' },
    @{ name = 'vulkan-gioff'; api = 'vulkan'; gi = 0; ppm = 'art2_win_vulkan_gioff_f350.ppm' }
)

# Asset gates (same files the r11 launchers hard-require before running).
$assetGates = @(
    'EngineTest\assets\SponzaTangentFrames.model',
    'EngineTest\assets\Sponza.model',
    'EngineTest\assets\textures\hdr\sunset.hdr',
    'EngineTest\assets\Sponza_global_sdf.v3.cache'
)

$results = @()

foreach ($g in $groups) {
    $missing = @()
    foreach ($a in $assetGates) {
        if (-not (Test-Path (Join-Path $Repo $a))) { $missing += $a }
    }
    if ($missing.Count -gt 0) {
        Write-Output "[f350] $($g.name) SKIP - missing asset gates:"
        foreach ($m in $missing) { Write-Output "[f350]   $m" }
        $results += [pscustomobject]@{
            group = $g.name; exit_code = 'asset-gate'; ppm = $g.ppm;
            ppm_exists = $false; ppm_sha256 = ''; watchdog = $false;
            anchors = @(); missing_assets = $missing
        }
        continue
    }

    # --- env matrix (verbatim from the r11 launchers) ---
    Remove-Item Env:PRIMAL_VK_HANG_DIAG -ErrorAction SilentlyContinue
    Remove-Item Env:PRIMAL_SPGI_SKIP -ErrorAction SilentlyContinue
    Remove-Item Env:GI_ISOLATE -ErrorAction SilentlyContinue
    Remove-Item Env:PRIMAL_DDGI_GEOMETRY_QUERY_KERNEL -ErrorAction SilentlyContinue
    Remove-Item Env:RHI_TEST_PLATFORM -ErrorAction SilentlyContinue

    $env:PRIMAL_DDGI_GEOMETRY_VISIBILITY = '1'
    $env:PRIMAL_TEST_MODEL = 'SponzaTangentFrames.model'
    $env:PRIMAL_VK_TANGENT_FRAME = '1'
    $env:DDGI_RAY_SAMPLE = '7'
    $env:DDGI_MEASURED_GEOMETRY = '1'
    $env:PRIMAL_SC_OWNER_CARD_DIRECTORY = '1'
    $env:SPGI_DDGI_VISIBILITY = '2'
    $env:PRIMAL_VK_ALBEDO_MIPS = '1'
    $env:PRIMAL_DDGI_TRAVERSAL_AUDIT = '0'
    $env:SPONZA_MAX_FRAMES = '400'
    $env:SPONZA_CAPTURE_FRAME = '350'
    $env:PRIMAL_SPONZA_WATCHDOG_SECS = "$NoProgressSecs"

    $ppmPath = Join-Path $OutDir $g.ppm
    $env:SPONZA_CAPTURE = $ppmPath

    if ($g.api -eq 'd3d12') { $env:RHI_TEST_PLATFORM = 'd3d12' }
    if ($g.gi -eq 1) { $env:PRIMAL_DDGI_GEOMETRY_QUERY_KERNEL = '1' }
    else { $env:GI_ISOLATE = 'all' }

    $so = Join-Path $OutDir ($g.name + '.stdout.log')
    $se = Join-Path $OutDir ($g.name + '.stderr.log')
    if (Test-Path $ppmPath) { Remove-Item $ppmPath -Force }

    Write-Output "[f350] group=$($g.name) api=$($g.api) gi=$($g.gi) ppm=$ppmPath"
    $start = Get-Date
    $proc = Start-Process -FilePath $Exe -WorkingDirectory $Repo `
        -RedirectStandardOutput $so -RedirectStandardError $se -PassThru
    $watchdogFired = $false
    while (-not $proc.HasExited) {
        Start-Sleep -Seconds 10
        $elapsed = ((Get-Date) - $start).TotalSeconds
        $lastWrite = (Get-Item $se -ErrorAction SilentlyContinue).LastWriteTime
        if ($null -ne $lastWrite) {
            $idle = ((Get-Date) - $lastWrite).TotalSeconds
        } else {
            $idle = $elapsed
        }
        if ($idle -gt $NoProgressSecs) {
            Write-Output "[f350] $($g.name) watchdog: no log progress for $([int]$idle)s - killing"
            $watchdogFired = $true
            try { $proc.Kill() } catch { }
            break
        }
        if ($elapsed -gt $HardCapSecs) {
            Write-Output "[f350] $($g.name) watchdog: hard cap $HardCapSecs s exceeded - killing"
            $watchdogFired = $true
            try { $proc.Kill() } catch { }
            break
        }
    }
    # Bounded wait: after Kill(), WaitForExit() without a timeout would block
    # until every redirected stream closes (a lingering grandchild can hold
    # the pipes for minutes). 5 s is enough to reap the killed process.
    try { [void]$proc.WaitForExit(5000) } catch { }
    if (-not $proc.HasExited) {
        Write-Output "[f350] $($g.name) note: handle still open after kill - continuing without reaping"
    }
    $rc = $proc.ExitCode
    if ($null -eq $rc) { $rc = 'killed' }
    $ppmExists = Test-Path $ppmPath
    $sha = ''
    if ($ppmExists) {
        $sha = (Get-FileHash -Algorithm SHA256 -Path $ppmPath).Hash.ToLower()
    }

    $anchors = @()
    if (Test-Path $se) {
        $content = Get-Content $se -Raw
        foreach ($a in @('\[Part37\] Loading HDR', 'Frame 350 captured',
                         'SPONZA_MAX_FRAMES=400 reached', '\[WATCHDOG\]')) {
            if ($content -match $a) { $anchors += ($a -replace '\\', '') }
        }
    }
    Write-Output "[f350] $($g.name) exit=$rc ppm_exists=$ppmExists watchdog=$watchdogFired sha256=$sha"

    $results += [pscustomobject]@{
        group = $g.name; api = $g.api; gi = $g.gi;
        exit_code = $rc; ppm = $g.ppm; ppm_exists = $ppmExists;
        ppm_sha256 = $sha; watchdog = $watchdogFired; anchors = $anchors;
        missing_assets = @()
    }
}

$json = ConvertTo-Json -InputObject $results -Depth 4
Set-Content -Path (Join-Path $OutDir 'f350_gates.json') -Value $json -Encoding UTF8
Write-Output '[f350] all groups done; gates written to f350_gates.json'
exit 0
