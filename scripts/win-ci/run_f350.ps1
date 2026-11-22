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
    @{ name = 'vulkan-gion'; api = 'vulkan'; gi = 1; ppm = 'art2_win_vulkan_f350.ppm'  },
    @{ name = 'vulkan-gioff'; api = 'vulkan'; gi = 0; ppm = 'art2_win_vulkan_gioff_f350.ppm' }
)

# R29 诊断消融组（PRIMAL_F350_TAAOFF_DIAG=1 时启用；写独立 diag_gates.json，
# 不进主 f350_gates.json——parity_report.py FATAL "expected 4 groups" 不受影响）。
# TAA_OFF=1：composite（实测 0.0149 有内容）直通 tonemap→FinalBlit→捕获——
# 捕获非零即证明 composite→捕获 断链=TAA 单点且 tonemap/blit 链活
#（run 44101462839 判读：taa_output=0 而上游 composite 有内容）。
$diagGroups = @()
if ($env:PRIMAL_F350_TAAOFF_DIAG -eq '1') {
    $diagGroups += @{ name = 'd3d12-gion-taaoff'; api = 'd3d12'; gi = 1;
                      ppm = 'art2_win_d3d12_taaoff_f350.ppm'; diag = 1 }
}
# R33 因果对分组（PRIMAL_F350_SHADOW_RENDER_OFF_DIAG=1）：PRIMAL_SHADOW_RENDER_OFF=1
# 跳过阴影 GPU 渲染录制——若 global_sdf/spgi/ddgi/indirect 复活即坐实
# "R31 起阴影 GPU 路径插入后 UAV compute 写全灭"为 barrier/状态机污染。
if ($env:PRIMAL_F350_SHADOW_RENDER_OFF_DIAG -eq '1') {
    $diagGroups += @{ name = 'd3d12-gion-shroff'; api = 'd3d12'; gi = 1;
                      ppm = 'art2_win_d3d12_shroff_f350.ppm'; diag = 2 }
}
# R36b 时序判别组（PRIMAL_F350_FRAME50_DUMP_DIAG=1）：BLACK_STAGE_FRAME=50
# ——SPGI/blit/GlobalSDF 在 frame 50 是否有内容。frame50 活而 frame350 零 =
# 中途死亡（与遥测/事件对齐）；两帧皆零 = 从未工作（init/首次 dispatch 面）。
# R36b→R39b 时序判别组（PRIMAL_F350_FRAME50_DUMP_DIAG=1）：BLACK_STAGE_FRAME=4
# ——voxelization/SPGI/blit 首次 dispatch 后立即读（frame 4）：
# frame4 即零 = 首写失败（init/首 dispatch 面收敛）；frame4 活而 350 零 = 中途覆写。
if ($env:PRIMAL_F350_FRAME50_DUMP_DIAG -eq '1') {
    $diagGroups += @{ name = 'd3d12-gion-frame4'; api = 'd3d12'; gi = 1;
                      ppm = 'art2_win_d3d12_frame4_f350.ppm'; diag = 3 }
}
# R48 deferred 采样链拆分判别（PRIMAL_F350_LIGHTING_ALBEDO_DIAG=1 /
# PRIMAL_F350_LIGHTING_SHADOW_DIAG=1）：PRIMAL_LIGHTING_DEBUG 放开 D3D12
# 后，z=4 → deferred_out=albedo 采样（采样链死活），z=3 → shadowVisibility
# （vis 采样死活）。R46 后 shadow_vis 已复活而 deferred_out 仍零的分流面。
if ($env:PRIMAL_F350_LIGHTING_ALBEDO_DIAG -eq '1') {
    $diagGroups += @{ name = 'd3d12-gion-albedo'; api = 'd3d12'; gi = 1;
                      ppm = 'art2_win_d3d12_albedo_f350.ppm'; diag = 4 }
}
if ($env:PRIMAL_F350_LIGHTING_SHADOW_DIAG -eq '1') {
    $diagGroups += @{ name = 'd3d12-gion-shadowdiag'; api = 'd3d12'; gi = 1;
                      ppm = 'art2_win_d3d12_shadowdiag_f350.ppm'; diag = 5 }
}
# R49 Load 对照组（texelFetch 绕过采样器，与 z=4/z=3 Sample 对照）
if ($env:PRIMAL_F350_LIGHTING_ALBEDOLOAD_DIAG -eq '1') {
    $diagGroups += @{ name = 'd3d12-gion-albedoload'; api = 'd3d12'; gi = 1;
                      ppm = 'art2_win_d3d12_albedoload_f350.ppm'; diag = 6 }
}
if ($env:PRIMAL_F350_LIGHTING_SHADOWLOAD_DIAG -eq '1') {
    $diagGroups += @{ name = 'd3d12-gion-shadowload'; api = 'd3d12'; gi = 1;
                      ppm = 'art2_win_d3d12_shadowload_f350.ppm'; diag = 7 }
}
# R50 零依赖组（z=8 常数 / z=9 屏幕梯度）——fragment 是否产生过
if ($env:PRIMAL_F350_LIGHTING_CONST_DIAG -eq '1') {
    $diagGroups += @{ name = 'd3d12-gion-constdiag'; api = 'd3d12'; gi = 1;
                      ppm = 'art2_win_d3d12_constdiag_f350.ppm'; diag = 8 }
}
if ($env:PRIMAL_F350_LIGHTING_GRADIENT_DIAG -eq '1') {
    $diagGroups += @{ name = 'd3d12-gion-gradient'; api = 'd3d12'; gi = 1;
                      ppm = 'art2_win_d3d12_gradient_f350.ppm'; diag = 9 }
}
# R61 GBV 组（PRIMAL_F350_GBV_DIAG=1）：PRIMAL_D3D12_GBV=1 + 12 帧短跑——
# GPU-based validation 对 deferred pass 的描述符/资源状态做 GPU 侧校验，
# 错误经帧首 InfoQueue drain 落日志（GBV 慢 ~10-100x，短跑防超时）。
if ($env:PRIMAL_F350_GBV_DIAG -eq '1') {
    $diagGroups += @{ name = 'd3d12-gbv'; api = 'd3d12'; gi = 1;
                      ppm = 'art2_win_d3d12_gbv_f350.ppm'; diag = 10 }
}

# Asset gates (same files the r11 launchers hard-require before running).
$assetGates = @(
    'EngineTest\assets\SponzaTangentFrames.model',
    'EngineTest\assets\Sponza.model',
    'EngineTest\assets\textures\hdr\sunset.hdr',
    'EngineTest\assets\Sponza_global_sdf.v3.cache'
)

$results = @()
$diagResults = @()

foreach ($g in ($groups + $diagGroups)) {
    $missing = @()
    foreach ($a in $assetGates) {
        if (-not (Test-Path (Join-Path $Repo $a))) { $missing += $a }
    }
    if ($missing.Count -gt 0) {
        Write-Output "[f350] $($g.name) SKIP - missing asset gates:"
        foreach ($m in $missing) { Write-Output "[f350]   $m" }
        $skipEntry = [pscustomobject]@{
            group = $g.name; exit_code = 'asset-gate'; ppm = $g.ppm;
            ppm_exists = $false; ppm_sha256 = ''; watchdog = $false;
            anchors = @(); missing_assets = $missing
        }
        if ($g.diag) { $diagResults += $skipEntry } else { $results += $skipEntry }
        continue
    }

    # --- env matrix (verbatim from the r11 launchers) ---
    Remove-Item Env:PRIMAL_VK_HANG_DIAG -ErrorAction SilentlyContinue
    Remove-Item Env:PRIMAL_SPGI_SKIP -ErrorAction SilentlyContinue
    Remove-Item Env:GI_ISOLATE -ErrorAction SilentlyContinue
    Remove-Item Env:PRIMAL_DDGI_GEOMETRY_QUERY_KERNEL -ErrorAction SilentlyContinue
    Remove-Item Env:RHI_TEST_PLATFORM -ErrorAction SilentlyContinue
    Remove-Item Env:PRIMAL_LIGHTING_DEBUG -ErrorAction SilentlyContinue

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

    if ($g.api -eq 'd3d12') {
        $env:RHI_TEST_PLATFORM = 'd3d12'
        # R16：D3D12 组启用逐调用 trace——定位 init 期静默设备移除的
        # 确切 API（移除原因已实测 DXGI_ERROR_INVALID_CALL，详见
        # taskG_giud_draw_invalid_call_r16.md）
        $env:PRIMAL_D3D12_API_TRACE = '1'
        # R23：黑帧六阶段内容 dump（gbuffer/deferred/fusion 前后/
        # tonemap/捕获链，taskI §6 判定表定界全黑阶段）
        $env:PRIMAL_D3D12_BLACK_STAGE_DUMP = '1'
        # R27：R25 探针启用（taskK §5 第 4 项当时漏落地）——绑定句柄真值
        # + scene_cb/view_cb 写后回读，定 deferred set 描述符内容链
        $env:PRIMAL_D3D12_DEFERRED_PROBE = '1'
        # R27：TEMP-DIAG CPU 侧真值（vp/invVP/SCENE_MAT0/SCENE_MISC——
        # DeferredLightingModule 内既有 env 门控，零代码）
        $env:PRIMAL_DUMP_MATRIX = '1'
    }
    if ($g.gi -eq 1) { $env:PRIMAL_DDGI_GEOMETRY_QUERY_KERNEL = '1' }
    else { $env:GI_ISOLATE = 'all' }
    # R29/R33/R36b 诊断组开关（diag=1 TAA_OFF，diag=2 SHADOW_RENDER_OFF，
    # diag=3 BLACK_STAGE_FRAME=50；diag=4/5 PRIMAL_LIGHTING_DEBUG=albedo/
    # shadow（R48 deferred 采样链拆分）；canonical 组确保无残留）
    if ($g.diag -eq 1) { $env:TAA_OFF = '1' }
    elseif ($g.diag -eq 2) { $env:PRIMAL_SHADOW_RENDER_OFF = '1' }
    elseif ($g.diag -eq 3) { $env:PRIMAL_D3D12_BLACK_STAGE_FRAME = '4' }
    elseif ($g.diag -eq 4) { $env:PRIMAL_LIGHTING_DEBUG = 'albedo' }
    elseif ($g.diag -eq 5) { $env:PRIMAL_LIGHTING_DEBUG = 'shadow' }
    elseif ($g.diag -eq 6) { $env:PRIMAL_LIGHTING_DEBUG = 'albedo_load' }
    elseif ($g.diag -eq 7) { $env:PRIMAL_LIGHTING_DEBUG = 'shadow_load' }
    elseif ($g.diag -eq 8) { $env:PRIMAL_LIGHTING_DEBUG = 'const' }
    elseif ($g.diag -eq 9) { $env:PRIMAL_LIGHTING_DEBUG = 'gradient' }
    elseif ($g.diag -eq 10) {
        $env:PRIMAL_D3D12_GBV = '1'
        $env:SPONZA_MAX_FRAMES = '12'
        $env:SPONZA_CAPTURE_FRAME = '12'
    }
    else {
        Remove-Item Env:TAA_OFF -ErrorAction SilentlyContinue
        Remove-Item Env:PRIMAL_SHADOW_RENDER_OFF -ErrorAction SilentlyContinue
        Remove-Item Env:PRIMAL_D3D12_BLACK_STAGE_FRAME -ErrorAction SilentlyContinue
        Remove-Item Env:PRIMAL_LIGHTING_DEBUG -ErrorAction SilentlyContinue
        Remove-Item Env:PRIMAL_D3D12_GBV -ErrorAction SilentlyContinue
        $env:SPONZA_MAX_FRAMES = '400'
        $env:SPONZA_CAPTURE_FRAME = '350'
    }

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

    $entry = [pscustomobject]@{
        group = $g.name; api = $g.api; gi = $g.gi;
        exit_code = $rc; ppm = $g.ppm; ppm_exists = $ppmExists;
        ppm_sha256 = $sha; watchdog = $watchdogFired; anchors = $anchors;
        missing_assets = @()
    }
    if ($g.diag) { $diagResults += $entry } else { $results += $entry }
}

$json = ConvertTo-Json -InputObject $results -Depth 4
Set-Content -Path (Join-Path $OutDir 'f350_gates.json') -Value $json -Encoding UTF8
Write-Output '[f350] all groups done; gates written to f350_gates.json'
if ($diagResults.Count -gt 0) {
    # R29 诊断组独立 gates 文件——parity_report 只消费 f350_gates.json，
    # 诊断组不影响任何门禁裁决。
    $diagJson = ConvertTo-Json -InputObject $diagResults -Depth 4
    Set-Content -Path (Join-Path $OutDir 'diag_gates.json') -Value $diagJson -Encoding UTF8
    Write-Output '[f350] diagnostic groups done; gates written to diag_gates.json'
}
exit 0
