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

# R66 env 对分组（PRIMAL_F350_MINENV_DIAG=1 时启用；写 diag_gates.json 不进门禁）。
# 依据：GPU 1.wpix（PIX 手动会话，R63b 最小 env——无 DDGI/SC/SPGI 附加旋钮）
# 嵌入 PNG 1280x720 逐像素非全黑（直射光/布幔/柱廊完整），而 CI canonical
# 全集 capture 逐字节全黑且 10 个消融组 SHA 相同。嫌疑收敛=canonical 附加
# 旋钮在 D3D12 上搞死中段资源（gbuffer 活、deferred/fusion/spgi 死，
# 与 R32-R38 "GPU 路径插入后写全灭/区段错位"全部证据相容）。
# 三分对分：minenv=1 全关（基线）；minenv=2 仅 DDGI 族开；minenv=3 仅
# SC+ALBEDO_MIPS 开（DDGI 关）。
# R66 判读（run 45137944823）：base=0482929d 非全黑（tonemap 0.0199 链活，
# 画面 84 色深灰微光）→ 附加旋钮假说成立；ddgi=847b7f79 逐字节等于
# canonical 全黑 → DDGI 族单独复现全黑（连 base 里活着的 composite/tonemap
# 也归零）；scmip=b3ace7b9 非全黑 → SC/MIPS 无辜。gi0 组黑且不设
# QUERY_KERNEL → 凶手在其余 5 旋钮。R67 续分（minenv=4/5）：
# A=DDGI_RAY_SAMPLE+SPGI_DDGI_VISIBILITY（采样/可见性查询侧）；
# B=DDGI_MEASURED_GEOMETRY+GEOMETRY_VISIBILITY+TRAVERSAL_AUDIT（几何侧）。
if ($env:PRIMAL_F350_DEFERRED_SAMPLES -eq '1') {
    foreach ($api in @('d3d12','vulkan')) {
        $diagGroups += @{ name = "$api-gion-samples"; api = $api; gi = 1;
                          ppm = "art2_win_${api}_gion_samples_f350.ppm"; diag = 13 }
    }
    $diagGroups += @{ name = 'vulkan-gion-mode0'; api = 'vulkan'; gi = 1;
                      ppm = 'art2_win_vulkan_mode0_f350.ppm'; diag = 12 }
    $diagGroups += @{ name = 'd3d12-gioff-samples'; api = 'd3d12'; gi = 0;
                      ppm = 'art2_win_d3d12_gioff_samples_f350.ppm'; diag = 11 }
    $diagGroups += @{ name = 'vulkan-gioff-samples'; api = 'vulkan'; gi = 0;
                      ppm = 'art2_win_vulkan_gioff_samples_f350.ppm'; diag = 11 }
}
$minEnvGroups = @()
if ($env:PRIMAL_F350_MINENV_DIAG -eq '1') {
    $minEnvGroups += @(
        @{ name = 'd3d12-minenv-base';  api = 'd3d12'; gi = 1; ppm = 'art2_win_d3d12_minenv_base_f350.ppm';  minenv = 1 },
        @{ name = 'd3d12-minenv-ddgiA'; api = 'd3d12'; gi = 1; ppm = 'art2_win_d3d12_minenv_ddgiA_f350.ppm'; minenv = 4 },
        @{ name = 'd3d12-minenv-ddgiA1'; api = 'd3d12'; gi = 1; ppm = 'art2_win_d3d12_minenv_ddgiA1_f350.ppm'; minenv = 6 },
        @{ name = 'd3d12-minenv-ddgiA2'; api = 'd3d12'; gi = 1; ppm = 'art2_win_d3d12_minenv_ddgiA2_f350.ppm'; minenv = 7 }
    )
}
# R70：A3（mode7+measured 完整组合，D3D12 首跑）与主批量隔离——run109 两连
# 死（attempt1/2 同在 step12 丢 runner、零 artifact、日志 BlobNotFound）锁定
# A3 触发机器级问题（GPU hang/驱动崩，R62 PIX 崩机同型风险）。改为独立开关
# 调用：4 帧短跑（GBV 组 diag=10 先例）+ workflow 侧插在 evidence upload 之后
# + continue-on-error，崩机也不丢其他组证据。
if ($env:PRIMAL_F350_A3_ONLY -eq '1') {
    $minEnvGroups += @{ name = 'd3d12-minenv-a3iso'; api = 'd3d12'; gi = 1;
                        ppm = 'art2_win_d3d12_minenv_a3_f350.ppm'; minenv = 8; short = 1 }
}
# R73：pixlike 判别组（PRIMAL_F350_PIXLIKE_DIAG=1；写 diag_gates.json 不进门禁）。
# R72 后唯一主矛盾=层2 deferred_out 直射零（CI mode0 微光态 composite 仅
# 0.0003，PSNR 6.94），而用户 PIX 会话（GPU 1.wpix PNG）直射光完整。CI base
# 组（无 DDGI 族）直射仍零 vs 用户会话直射正常——剩余差异清单极短：
# PRIMAL_TEST_MODEL/TANGENT_FRAME/API_TRACE/DUMP_MATRIX/帧数(12 vs 400)/
# PIX attach/交互 vs 无头会话。pixlike 逐项复制用户会话 env（R63b 最小四件
# +BLACK_STAGE_DUMP+DEFERRED_PROBE 保留=用户同款；摘模型/切线帧/诊断件/
# DDGI 全族/SC/MIPS；12 帧 CAPTURE_FRAME=10）。判读：pixlike 直射正常→
# 嫌疑=被摘的 env 之一或 12 帧时长，继续二分；pixlike 仍黑→差异只剩 PIX
# attach/交互会话/无头 swapchain 行为，转用户侧实验。
$pixlikeGroups = @()
if ($env:PRIMAL_F350_PIXLIKE_DIAG -eq '1') {
    $pixlikeGroups += @{ name = 'd3d12-pixlike'; api = 'd3d12'; gi = 1;
                         ppm = 'art2_win_d3d12_pixlike_f10.ppm'; pixlike = 1 }
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

foreach ($g in ($groups + $diagGroups + $minEnvGroups + $pixlikeGroups)) {
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
        if ($g.diag -or $g.minenv -or $g.pixlike) { $diagResults += $skipEntry } else { $results += $skipEntry }
        continue
    }

    # --- env matrix (verbatim from the r11 launchers) ---
    foreach ($key in @(
        'PRIMAL_F350_GI_BINDINGS', 'PRIMAL_VK_HANG_DIAG', 'PRIMAL_SPGI_SKIP', 'GI_ISOLATE',
        'PRIMAL_DDGI_GEOMETRY_QUERY_KERNEL', 'RHI_TEST_PLATFORM', 'PRIMAL_LIGHTING_DEBUG',
        'TAA_OFF', 'PRIMAL_SHADOW_RENDER_OFF', 'PRIMAL_D3D12_BLACK_STAGE_FRAME',
        'PRIMAL_D3D12_GBV', 'PRIMAL_D3D12_BLACK_STAGE_DUMP', 'PRIMAL_D3D12_API_TRACE',
        'PRIMAL_D3D12_DEFERRED_PROBE', 'PRIMAL_DUMP_MATRIX', 'PRIMAL_D3D12_RTVAUDIT',
        'PRIMAL_D3D12_DEFERRED_CLEAR_SENTINEL', 'PRIMAL_D3D12_DEFERRED_COPY_WRITE', 'PRIMAL_DEFERRED_SAMPLES'
    )) { Remove-Item "Env:$key" -ErrorAction SilentlyContinue }

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

    if ($g.name -in @('d3d12-gion','vulkan-gion')) { $env:PRIMAL_F350_GI_BINDINGS = '1' }
    $ppmPath = Join-Path $OutDir $g.ppm
    $env:SPONZA_CAPTURE = $ppmPath

    if ($g.api -eq 'd3d12') { $env:RHI_TEST_PLATFORM = 'd3d12' }
    if (($g.diag -and $g.diag -notin @(11,12,13)) -or $g.minenv -or $g.pixlike) {
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
    # R66 minenv 对分覆盖（在 canonical 设置与 gi 设置之后执行才能摘干净）。
    # minenv=1：摘掉全部附加旋钮（DDGI 族+SC+ALBEDO_MIPS——R63b 最小 env 同款）。
    # minenv=2：恢复 DDGI 族（QUERY_KERNEL 含内）。minenv=3：恢复 SC+ALBEDO_MIPS。
    if ($g.minenv -or $g.pixlike) {
        Remove-Item Env:PRIMAL_DDGI_GEOMETRY_VISIBILITY -ErrorAction SilentlyContinue
        Remove-Item Env:DDGI_RAY_SAMPLE -ErrorAction SilentlyContinue
        Remove-Item Env:DDGI_MEASURED_GEOMETRY -ErrorAction SilentlyContinue
        Remove-Item Env:SPGI_DDGI_VISIBILITY -ErrorAction SilentlyContinue
        Remove-Item Env:PRIMAL_DDGI_TRAVERSAL_AUDIT -ErrorAction SilentlyContinue
        Remove-Item Env:PRIMAL_DDGI_GEOMETRY_QUERY_KERNEL -ErrorAction SilentlyContinue
        Remove-Item Env:PRIMAL_SC_OWNER_CARD_DIRECTORY -ErrorAction SilentlyContinue
        Remove-Item Env:PRIMAL_VK_ALBEDO_MIPS -ErrorAction SilentlyContinue
        if ($g.minenv -eq 2) {
            $env:PRIMAL_DDGI_GEOMETRY_VISIBILITY = '1'
            $env:DDGI_RAY_SAMPLE = '7'
            $env:DDGI_MEASURED_GEOMETRY = '1'
            $env:SPGI_DDGI_VISIBILITY = '2'
            $env:PRIMAL_DDGI_TRAVERSAL_AUDIT = '0'
            $env:PRIMAL_DDGI_GEOMETRY_QUERY_KERNEL = '1'
        }
        if ($g.minenv -eq 3) {
            $env:PRIMAL_SC_OWNER_CARD_DIRECTORY = '1'
            $env:PRIMAL_VK_ALBEDO_MIPS = '1'
        }
        # R67：DDGI 族内部对分（gi0 组黑且不设 QUERY_KERNEL → 凶手在其余 5 个）。
        # minenv=4 = A 组：采样/可见性查询侧（DDGI_RAY_SAMPLE+SPGI_DDGI_VISIBILITY）。
        # minenv=5 = B 组：几何侧（DDGI_MEASURED_GEOMETRY+GEOMETRY_VISIBILITY+
        # TRAVERSAL_AUDIT——SWRT 审计注：MEASURED_GEOMETRY 实为 BVH 替换开关）。
        if ($g.minenv -eq 4) {
            $env:DDGI_RAY_SAMPLE = '7'
            $env:SPGI_DDGI_VISIBILITY = '2'
        }
        # R67 判读（run 45143876421）：ddgiA=847b7f79 逐字节=canonical 全黑
        # （采样侧锁死）；ddgiB=dcc52c94 非全黑（几何侧无辜——D3D12 上
        # measuredVisibility=(vk||metal)&&… 恒 false，B 组 vis mode 走 0 无
        # 变化，自洽）。R68 单旋钮拆分：
        # minenv=6 = A1：仅 DDGI_RAY_SAMPLE=7（LumenDDGI trace 方向 mode，
        #   measured oct strata——改 ray 生成 shader 分支）。
        # minenv=7 = A2：仅 SPGI_DDGI_VISIBILITY=2（SPGI gather 可见性 mode 2，
        #   shader 读 ddgi_depth_buffer_/relocated_origins_ 做可见性查询——
        #   D3D12 上若该数据链没填对（R35 D32 SRV 采样零家族）→ 极端值/NaN
        #   → UNORM 写 NaN=0 全黑传播）。
        if ($g.minenv -eq 6) {
            $env:DDGI_RAY_SAMPLE = '7'
        }
        if ($g.minenv -eq 7) {
            $env:SPGI_DDGI_VISIBILITY = '2'
        }
        # R69 A3：mode7+measured 完整组合（DDGI_RAY_SAMPLE=7 +
        # DDGI_MEASURED_GEOMETRY=1）——引擎回落门落地后判 D3D12 上
        # 该组合是否独立健康（黑=mode7+measured 独立断点；活=canonical
        # 黑因在其他旋钮×mode7 交互）。
        if ($g.minenv -eq 8) {
            $env:DDGI_RAY_SAMPLE = '7'
            $env:DDGI_MEASURED_GEOMETRY = '1'
        }
    }
    if ($g.pixlike) {
        foreach ($key in @('PRIMAL_TEST_MODEL', 'PRIMAL_VK_TANGENT_FRAME',
                           'PRIMAL_D3D12_API_TRACE', 'PRIMAL_DUMP_MATRIX')) {
            Remove-Item "Env:$key" -ErrorAction SilentlyContinue
        }
        $env:SPONZA_MAX_FRAMES = '12'
        $env:SPONZA_CAPTURE_FRAME = '10'
        $env:PRIMAL_D3D12_BLACK_STAGE_FRAME = '10'
    }
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
        # R61b：GBV 慢 10-100x——4 帧 + watchdog 放宽到 900s（第一帧 GBV
        # 校验本身可能 >300s，R62 实测 300s watchdog 连第一帧都没出）
        $env:PRIMAL_D3D12_GBV = '1'
        $env:SPONZA_MAX_FRAMES = '4'
        $env:SPONZA_CAPTURE_FRAME = '4'
        $env:PRIMAL_SPONZA_WATCHDOG_SECS = '900'
    }
    # R70：隔离短跑档（diag 开关之后覆盖才生效）——A3 崩机风险面缩到 4 帧
    # + 900s watchdog（GBV 组 diag=10 同款先例）。
    if ($g.short) {
        $env:SPONZA_MAX_FRAMES = '4'
        $env:SPONZA_CAPTURE_FRAME = '4'
        $env:PRIMAL_SPONZA_WATCHDOG_SECS = '900'
    }

    if ($g.diag -eq 12) { $env:DDGI_RAY_SAMPLE = '0' }
    if ($g.diag -in @(12,13)) {
        $env:PRIMAL_DEFERRED_SAMPLES = '1'
        $env:PRIMAL_F350_GI_BINDINGS = '1'
    }
    if ($g.diag -eq 8) {
        $env:PRIMAL_D3D12_DEFERRED_CLEAR_SENTINEL = '1'
        $env:PRIMAL_D3D12_RTVAUDIT = '1'
        $env:PRIMAL_D3D12_DEFERRED_COPY_WRITE = '1'
    }
    if ($g.diag -eq 11 -or $g.diag -eq 8) {
        $env:PRIMAL_DEFERRED_SAMPLES = '1'
    }

    $so = Join-Path $OutDir ($g.name + '.stdout.log')
    $se = Join-Path $OutDir ($g.name + '.stderr.log')
    if (Test-Path $ppmPath) { Remove-Item $ppmPath -Force }

    Write-Output "[f350] group=$($g.name) api=$($g.api) gi=$($g.gi) ppm=$ppmPath"
    $start = Get-Date
    $proc = Start-Process -FilePath $Exe -WorkingDirectory $Repo `
        -RedirectStandardOutput $so -RedirectStandardError $se -PassThru
    # Retain the native handle while the process is live (Windows PowerShell 5.1).
    $null = $proc.Handle
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

    $captureFrame = [int]$env:SPONZA_CAPTURE_FRAME
    $maxFrames = [int]$env:SPONZA_MAX_FRAMES
    $expectedModel = if ($env:PRIMAL_TEST_MODEL) { $env:PRIMAL_TEST_MODEL } else { 'Sponza.model' }
    $anchors = @(Select-String -LiteralPath $se,$so -Pattern @(
        '\[Part30.4\] model path:', '\[Part37\] Loading HDR:',
        "Frame $captureFrame captured", "SPONZA_MAX_FRAMES=$maxFrames reached"
    ) | ForEach-Object { $_.Line })
    $observed = ($anchors -join "`n")
    $contract = ($observed -match ("model=" + [regex]::Escape($expectedModel) + ",")) -and
                ($observed -match 'Loading HDR:.*sunset\.hdr') -and
                ($observed -match "Frame $captureFrame captured") -and
                ($observed -match "clean exit at frame $maxFrames\b") -and
                ($rc -eq 0) -and $ppmExists -and (-not $watchdogFired)
    if (-not $contract) { Write-Warning "[f350] $($g.name) launch/capture contract failed" }
    Write-Output "[f350] $($g.name) exit=$rc ppm_exists=$ppmExists watchdog=$watchdogFired sha256=$sha"

    $launchEnv = [ordered]@{}
    foreach ($key in @('PRIMAL_TEST_MODEL','PRIMAL_VK_TANGENT_FRAME','GI_ISOLATE','DDGI_RAY_SAMPLE',
        'DDGI_MEASURED_GEOMETRY','PRIMAL_DDGI_GEOMETRY_VISIBILITY','PRIMAL_DDGI_GEOMETRY_QUERY_KERNEL',
        'SPGI_DDGI_VISIBILITY','PRIMAL_SC_OWNER_CARD_DIRECTORY','PRIMAL_VK_ALBEDO_MIPS',
        'PRIMAL_DDGI_TRAVERSAL_AUDIT','TAA_OFF','PRIMAL_SHADOW_RENDER_OFF','PRIMAL_LIGHTING_DEBUG',
        'PRIMAL_D3D12_BLACK_STAGE_DUMP','PRIMAL_D3D12_BLACK_STAGE_FRAME','PRIMAL_DUMP_MATRIX',
        'PRIMAL_D3D12_API_TRACE','PRIMAL_D3D12_DEFERRED_PROBE','PRIMAL_D3D12_GBV',
        'PRIMAL_D3D12_DEFERRED_CLEAR_SENTINEL','PRIMAL_D3D12_RTVAUDIT','PRIMAL_D3D12_DEFERRED_COPY_WRITE',
        'PRIMAL_F350_GI_BINDINGS','PRIMAL_DEFERRED_SAMPLES','SPONZA_CAPTURE_FRAME','SPONZA_MAX_FRAMES','RHI_TEST_PLATFORM')) {
        $value = [Environment]::GetEnvironmentVariable($key)
        if ($null -ne $value) { $launchEnv[$key] = $value }
    }
    $entry = [pscustomobject]@{
        group = $g.name; api = $g.api; gi = $g.gi;
        exit_code = $rc; ppm = $g.ppm; ppm_exists = $ppmExists;
        ppm_sha256 = $sha; watchdog = $watchdogFired; anchors = $anchors;
        model = $expectedModel; max_frames = $maxFrames; capture_frame = $captureFrame
        contract_ok = $contract; launch_env = $launchEnv; missing_assets = @()
    }
    if ($g.diag -or $g.minenv -or $g.pixlike) { $diagResults += $entry } else { $results += $entry }
}

$json = ConvertTo-Json -InputObject $results -Depth 4
Set-Content -Path (Join-Path $OutDir 'f350_gates.json') -Value $json -Encoding UTF8
Write-Output '[f350] all groups done; gates written to f350_gates.json'
if ($diagResults.Count -gt 0) {
    # R29 诊断组独立 gates 文件——parity_report 只消费 f350_gates.json，
    # 诊断组不影响任何门禁裁决。R70：A3 隔离调用（PRIMAL_F350_A3_ONLY=1）
    # 写独立 a3_gates.json，防同 OutDir 复用时覆盖主调用的 diag_gates.json。
    $diagGatesName = if ($env:PRIMAL_F350_A3_ONLY -eq '1') { 'a3_gates.json' } else { 'diag_gates.json' }
    $diagJson = ConvertTo-Json -InputObject $diagResults -Depth 4
    Set-Content -Path (Join-Path $OutDir $diagGatesName) -Value $diagJson -Encoding UTF8
    Write-Output "[f350] diagnostic groups done; gates written to $diagGatesName"
}
exit 0
