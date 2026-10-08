# Froxel 体积雾升级与集成门禁 — 完工记录

> 日期:2026-09-30
> 状态:✅ **完工**(Phase 0-3 全部落地,Vulkan + Metal 双端)
> 计划:`Docs/2026-09-30-froxel-volume-fog-upgrade-and-gate-plan.md`(本文为其完工记录)
> 范围:`Engine/Graphics/Volume/` 三 pass → 四 pass 管线 + `TestVulkanFroxelRenderGraph` 门禁 + MSL 全量移植

---

## 1. 交付总览

| Phase | 内容 | DoD 状态 |
|---|---|---|
| 0 | TestVulkanFroxelRenderGraph 门禁(G1-G4) | ✅ RESULT PASS,G1-G4 全绿,阈值回填 §4 |
| 1 | WP1 light march 体积自阴影 | ✅ G5 双相位 PASS(march0 vs march4),FROXEL_MARCH env |
| 2 | WP2 SPGI 环境项 | ✅ A/B 入档(场均值 6×/雾色跟随 GI),fallback 确定性验证,帧时间零增量 |
| 3 | WP3 体积时间重投影 | ✅ 门禁 temporal 回归 5/5,切镜 +1/+2/+4 帧无鬼影(截图入档) |
| 4 | WP4 抖动累积(可选) | ⏸ 未排期(按计划:仅当 WP1 观感不足时启动;WP1 诚实版达标) |
| Metal | MSL 全量移植(mode2/march/SPGI/reproject)+ TestModularPipeline 集成 | ✅ 功能验证(见 §6) |

**最终门禁**:`Darwin/Debug/TestVulkanFroxelRenderGraph; echo $?` → `RESULT PASS (5 pass, 0 fail)`,exit 0。

## 2. 门禁设计(TestVulkanFroxelRenderGraph)

- 照 DLSS 门禁模式:Sponza 脚手架 + `Initialize` 内 setenv 自带配置(`FROXEL=1`、
  `SPONZA_REFERENCE=1`、`FROXEL_UNIFORM=0.35`)+ 相位机 + gate frame 进程内断言 +
  `_Exit(0/1)`。CMake 注册照计划 §6.2 模板,依赖 VulkanShaderFreshnessGate。
- 场景策略采用计划 §6.3 **退路**(Sponza 场景):断言全部做成资产无关形式
  (全局统计 / 世界 Y 分层 / 双相位自比较),程序化最小场景留待后续。
- 相位时间线(F=FROXEL_GATE_FRAME,默认 300):
  A(F,uniform+shadow)→ B(F+45,no-shadow)→ C(F+90,shadow+march0)→
  D(F+135,march4 断言 G5)→ noise 重建(F+137)→ E(F+180,noise 读回 G4)→ RESULT。

## 3. 引擎改动

### 3.1 契约扩展(FroxelTypes.h,656B → 800B 三方镜像)

```
VolumeParams4        656: x=march_steps, y=march_distance, z=ambient_source, w=temporal_blend
ViewProj[4]          672: 当前帧 view-projection
PrevInvViewProj[4]   736: 上一帧 inverse view-projection(WP3)
static_assert(sizeof(FroxelParams) == 800)
```
GLSL(FroxelCommon.glsl)/MSL(FroxelCommon.metal)/C++ 逐字节一致;MSL 同步补
`froxel_world_to_uvw` helper。

### 3.2 WP1 light march(FroxelLightIntegrate.comp)

- `march_steps>0`:朝光方向(传播方向取负——计划伪码 `+LightDir` 未区分约定,
  audit V7 契约下朝光为负)N 步,`vis *= exp(-σt·stepLen)`(常量介质免 3D 采样,
  噪声模式三线性采 `froxel_density` 3D)× 每步 `froxelShadowQuery`。
- 半步起步(中点采样);`FROXEL_MARCH=<N>` env(Sponza/Metal 测试);默认 0
  = legacy 单查询(回滚位),G5 过关后视需要翻默认。

### 3.3 WP2 SPGI 环境项(FroxelLightIntegrate.comp + StandardRenderPipeline)

- 数据源:上一帧 SPGI 过滤输出(`GetFilteredOutputTexture()`,雾在 Step 4.5
  先于本帧 SPGI 重写执行 → 读到最近完成帧;计划 §4 WP2 认可的退路)。
- 采样坐标:**froxel 列自身的屏幕瓦片 UV**(`(gid+0.5)/dims`)——初版用
  ViewProj 重投影实测把 UV 打到纹理零内容区(排查记录见 §5),froxel 网格与
  屏幕对齐故直接用列 UV,更省一次矩阵乘。
- 单位:SPGI 纹理是辐照度 E(FusionIndirect 按 albedo×E×gain/π 消费);
  雾各向同性入射源 = σs·E/π,乘观感校准 GAIN 30(E 场内 ~0.001-0.05,物理
  量级下雾不可见)。`FROXEL_SPGI_AMBIENT=0`/句柄无效 → 硬编码天空色 fallback
  (G1-G5 全部在 fallback 基准下标定并通过)。

### 3.4 WP3 时间重投影(新 FroxelReproject.comp/.metal + 第 4 管线)

- 每 froxel 一线程(4³ 线程组):世界坐标 → PrevInvViewProj 投影 → 三线性
  采上一帧混合结果 → 3×3×3 邻域 min/max 钳制(27 读,独立 pass)→
  `mix(current, history, blend)`(blend=0.35)写独立 hist 纹理;resolve 采
  hist,下一帧 hist 作为 prev(triple 轮转)。
- 首帧/切镜 passthrough;`FROXEL_TEMPORAL=0` 或 reproject 编译失败 → 整段
  旁路(行为与历史一致)。`GetScatter3DTexture` 在 temporal 开启时返回 hist
  (与 resolve 同源)。

### 3.5 管线接线

- `StandardRenderPipeline::RenderWithCommandBuffer` Step 4.5:`froxelIn.
  spgi_ambient_tex`(裸句柄直读,SPGI 自管理纹理,与 froxel 模块自有纹理同模式)。
- 运行时旋钮透传:`SetFroxelMarchSteps/SetFroxelTemporalEnabled/
  SetFroxelAmbientSPGI`(免 pass 重建,门禁双相位用);shadow_mode=2 的
  Vulkan-only 门已撤(MSL 移植后双端同权)。

## 4. 门禁阈值标定(回填计划 §6.4 表)

| # | 断言 | 判据(标定后) | 实测 |
|---|---|---|---|
| G1 | 积分健康 | NaN=0;T 单调违例 ≤0.01%;首片 meanT>0.9;远半段散射 >1e-4 占比 ≥0.5% | 全过(nan=0, tviol=0%, T₀=0.992, far=100%) |
| G2 | uniform 覆盖 | 总覆盖 ≥25% 且首 slice 覆盖 ≥90%(min-slice 判据无意义:墙后 slice 近全清零是预期,Phase 0 首跑改判) | 32.1% / 100% |
| G3 | 体积阴影对比 | 强阴影列占比(r<0.7)≥3% 且均值比 ≤0.97(shadow off↔on 双相位) | 100% / 0.405 |
| G4 | SDF carve | 地面下参考层(y<-0.4)均值 <0.01 且空气带(0.5-4m)>0.004 且覆盖 ∈(2%,95%) | 0 / 0.107 / 31.5% |
| G5 | march 自阴影 | r<0.8 列 ≥5% 且场均值比 ≤0.97 且 darker>4×brighter(march0↔march4 双相位) | 6.85% / 0.9411 / 1730:36 |
| G6 | 时域稳定性 | 打印不断言(门禁相机锁定+噪声密度无时间项,静态差分不敏感;时域以切镜视觉验收代替) | — |

注:G5 的 A/B 放在 **uniform 相位**(Beer 衰减有解析预测 vis≈exp(-σt·L);
噪声模式 march 贡献被低密度+陡太阳压到 <1%,run12 实测);噪声模式的 march
效果以 §6 截图与 meanA/meanB(0.0345→0.0324,-5.9%)入档。

## 5. 排障记录(高价值教训)

1. **描述符 count bug(WP2 主坑)**:integrate 描述符集加 binding 8 后
   `UpdateDescriptorSet(..., count)` 未同步 8→9 → SPGI 采样恒 0 且对增益
   不敏感,表现为"ambient 静默失效"。任何加绑定的 pass 都要检查 count 参数。
2. **相位机状态覆盖**:门禁相位 B 尾部误设 `UniformShadowM0` 跳过 C 分支 →
   D 读空向量段错误。相位机每分支只应设置"下一分支期待的值"。
3. **G3 相位后忘恢复阴影**:noise/march 相位在 shadowMode=0 下 march 分支
   被整体跳过(恒 lit),G5 恒等于 baseline——切配置后必须复位所有相关开关。
4. **ViewProj 投影采样落到零内容区**:世界坐标重投影的 UV 数值上成立但采样
   恒 0;froxel 列=屏幕瓦片,直接用列 UV(见 §3.3)。
5. **SPGI 重建窗口瞬态(已知问题,未修)**:GI content-ready 同帧 SPGI 重建,
   `output_texture_filtered_` 句柄值复用而 view 短暂 0x0,binding 8 报 ~18 条
   启动期 validation 警告后自愈。句柄值不变无法在 froxel 侧探测,任何裸绑定
   消费者都会暴露;不崩溃、结果正确,留待 SPGI 重建时序治理。
6. **环境文件搅局(会话级异常,非仓库问题)**:会话期间 `build_spv.sh` 的
   LUMEN_GLSL 数组与源树 spv 多次被外部回退/改写(症状:spv 入口点错配成
   `eye_adaptation`/`screen_probe_light_var`、数组 53/52 错位、并行数组多出
   `"main"`)。已修复并经 53/53/53 对齐 + 0 mismatch 校验;如复发,优先怀疑
   并行会话/编辑器对 `Engine/Graphics/Vulkan/shaders/` 的写操作。

## 6. 截图档案(/tmp/froxel_shots/,PPM 原档 + PNG)

| 文件 | 内容 |
|---|---|
| `froxel_gate_A_shadow_300.png` | Phase 0 uniform+阴影(god rays) |
| `froxel_gate_C_noise_395.png` / `E_noise_480.png` | noise 相位(SDF carve,天空净空) |
| `march0.png` / `march4.png` | WP1 A/B(参考机位,340 帧) |
| `beauty_march4_spgi.png` / `beauty_spgi_on.png` | 氛围档(march4+SPGI+EXT0.9+相位0.6) |
| `final_vulkan.png` / `final_vulkan_dense.png` | 最终成品(校准增益;EXT0.9/1.5 两档) |
| `comparison_vs_reference.png` | **与参考图并排对比**(交付物) |
| `metal_froxel.png` / `metal_froxel_ref.png` | Metal 进程内捕获(基线过曝为测试既有观感) |
| `cut_362/364.png` | WP3 切镜收敛(无鬼影) |
| `gi_isolate_spgi.png` | SPGI 源隔离视图(WP2 数据源验证) |

与参考图的对照结论:参考为离线路溯的局部云团(Density scale 0.04 面片云),
本引擎为相机对齐 froxel 网格上的噪声雾 + 高度雾 + SDF carve——同为
emission-absorption 前向积分 + Beer 自阴影 + 环境项照亮的实时 froxel 管线;
雾体自阴影(god rays 内投影层次)、GI 着色(暖色跟随)、时域稳定均已达成,
密度形体差异属场景/密度源差异(噪声雾 vs 面片云),非管线能力差异。

## 7. 性能(§5 预算表回填,Debug 档 2560×1440 渲染,整管线墙钟)

| 项 | 实测 |
|---|---|
| 基线(uniform+shadow+SPGI off,frames 100-299) | 27.03-27.24 ms/帧 |
| + WP1 march=4 | 27.20 ms(噪声模式;uniform 模式下 march 分支更重但同量级) |
| + WP2 SPGI ambient | 26.97-27.23 ms(±噪声级,预算 +0.05-0.1ms 内) |
| + WP3 temporal | 26.94 ms(±噪声级,预算 +0.2-0.4ms 内——92万线程 27 读较轻) |
| 结论 | 全功能开启无可测增量,远低于 <2.5ms 预算线(Debug 档噪声 ±0.3ms) |

## 8. Metal 端(计划 §3.5 的"Metal 不跟"由用户目标改为"Vulkan 后进行 Metal")

- MSL 全量移植:`FroxelLightIntegrate.metal`(双模式阴影查询 + march +
  SPGI ambient)、`FroxelReproject.metal`(新)、`FroxelCommon.metal`
  (800B 布局 + world_to_uvw)。
- `TestModularPipeline`:`FROXEL=1` env 开启(默认关——T8.13 契约:fusion
  加性合成下 Metal 雾差读作全屏白洗),标定与 Vulkan 同款(ext 0.35/height
  fog 8/fade 30/FROXEL_UNIFORM/MARCH/SPGI_AMBIENT/TEMPORAL)。
- 验证:三 pass 每帧调度(TEXBIND enc=froxel_*),75-100s 多轮无 MSL 错误/
  无 fault;A/B 方向正确(雾 on:均值 220.6→212.8、stdev 48.4→53.9,即变暗
  + 增对比);进程内捕获 `metal_froxel*.png` 入档。
- 已知边界:TestModularPipeline 基线过曝(GI on)与 GI_ALL_OFF 黑屏均为该
  测试既有观感问题(与雾无关,本文不处理);显示器休眠时窗口截图不可用
  (CG 全黑)——统一走进程内 PPM 捕获。

## 8a. 追加:云原语(WP5,2026-09-30 用户验收反馈后追加)

> **最终路线(用户建议对齐参考实现):云体 = CPU 预烘焙 3D 密度纹理**
> (96³ R32_Float,初始化时一次性生成上传),注入 pass 按云 AABB(center
> ± radius)采样 `max()` 叠加于基础霾。inject 描述符 7→8 bindings
> (binding 7 = cloud_volume,云关时绑噪声纹理占位)。
> 三尺度形状:低频 Worley 大团块 plateau(团间深谷)+ FBM 中尺度 +
> 高频 Worley 蚀边,系数经 v1-v13 迭代标定。
> **格式坑:R8_UNorm 在该 CopyBufferToTexture 路径上采样恒 0**
> (体素数据已验证健康仍不可见),换 R32_Float(与 128³ 噪声纹理同
> 路径)即通。
> **位置坑:云必须放在天光带**(庭院开孔阴影区/近遮挡屏幕落点会被
> 注入的深度清零吃掉,或纯遮挡无散射成黑体);v10-v13 排查记录。
> **观感**:环境项下限(max(SPGI, 天空×0.55))保证云底灰调;成品档
> D=12-14(三尺度连乘后平均体密度 ~1)+ EXT 0.1-0.25 + AMBIENT 0.9-1.8。
> 成品截图 `texcloud_v9.png`(庭院上空积云,完整轮廓),对比拼图已更新。
>
> **第二轮重设计(puff-cluster,用户"和参考图差得远"反馈)**:三尺度噪声调制的
> 包络读作"雾团"而非积云——根因:①噪声调制把内部掏空(参考图的云剪影是
> **实心**的,团块结构靠 march 阴影明暗表达,不靠透明度);②包络是平滑椭圆,
> 没有多团块轮廓。重写为**显式 puff 球堆叠**(13 球,平底/中部大团/顶部小团,
> 二次衰减求和阈值出凹凸包络),内部实心(D=14-16,τ~3),三尺度噪声只做
> 谷/调制/蚀边(**不清零**);观感关键=**低消光**(EXT 0.05,τ~2-3,光穿透
> 云体填亮,参考图面板 density scale 0.04 同调法)——EXT 0.25 时 τ≈19 纯黑体。
> 成品 `puff_v4.png`(R6/D16/EXT0.05/AMBIENT1.6/PHASE_G0.3)。
>
> **第三轮(用户"体高度有点高"反馈)**:y 压扁 1.05→1.30(CPU 烘焙单点修改),
> 定稿 `puff_final.png`(POS 7.5,14.8,3 / R5.5 / D14 / EXT0.1 / AMBIENT0.9)。
> 排查记录:云在庭院中部/远拱开口区会被注入的深度清零完全吃掉(透过屋顶
> 竖井看到的是井壁,距离近于云——无条件密度 30 实心立方探针验证),该区
> 不可放云。
>
> **第四轮(用户"为什么要设置这么高"反馈)= 最终形态**:云心放低到
> **y≈3.5-3.8(地面之上数米,与地面参考机位视线齐平)**——无需任何高机位,
> 平视即参考图构图。配套两处注入 pass 重构:
> 1. **云体先于深度清零采样**:4 象限 min-depth 清零是霾的保守策略,云的
>    屏幕足迹沾到近处槽沿像素时整格被清(puff_v12 排查:小块云必然全灭);
>    云是显式放置的体积实体,墙体相交由 SDF 固体门(smoothstep(-1,-0.2,sdf))
>    剔除,霾保留原清零(GLSL/MSL 同步)。
> 2. **垂直密度梯度**:底浓顶淡(×1.15→0.85),march 阴影下自然形成参考图
>    的"暗底亮顶"。
> 定稿 `cloud_ground_v2.png`(POS 7,3.8,2 / R4 / D22 / FREQ0.42 / EXT0.1 /
> AMBIENT1.2),对比拼图已更新。

用户对比参考图反馈"有大气雾效果,但没有云的效果",并定调形态:**全场景雾霾 +
中间一小块独立云体**。追加实现:

- 契约:FroxelParams 800→**832B**(`CloudA`=enable/radius/density/noise_freq、
  `CloudB`=center/wind),三方镜像 + static_assert 832。
- 密度源(注入 pass,GLSL/MSL 同式):球形包络(略压扁 ×1.18,云底 0.5R 下淡出
  成平底盘)× FBM 基底 × Worley 细节侵蚀,`max()` 叠加在基础霾之上;绕过
  SDF/高度门(云悬空于开放空域);`CloudB.w·time` 漂移。云模式不影响基础霾
  (用户明确要"全场景雾霾 + 独立云块")。
- env:`FROXEL_CLOUD=1 FROXEL_CLOUD_POS=x,y,z FROXEL_CLOUD_R=4.3
  FROXEL_CLOUD_D=2.6 FROXEL_CLOUD_FREQ=0.24 FROXEL_CLOUD_WIND`(Vulkan/Metal
  同款);成品档 extinction 0.55 + phase_g 0.5 + SPGI ambient。
- 验证:门禁回归 5/5(默认云关,832B 布局无回归);Metal 同步编译运行无错
  (进程内捕获 `metal_cloud.png`,基线过曝为测试既有观感)。
- 截图:`cloud_v1-v4.png` 迭代过程,`cloud_v4.png` 成品,`comparison_vs_
  reference.png` 已更新为参考图 vs 云块成品对比。

## 9. 回归

- `TestVulkanFroxelRenderGraph` 默认与 `FROXEL_TEMPORAL=1`/`FROXEL_SPGI_AMBIENT=1`
  变体:5/5 PASS,exit 0。
- `TestVulkanSponzaRenderGraph` 全功能(FROXEL+MARCH4+SPGI+TEMPORAL)120s+
  长跑:无 fault,仅 §5.5 已知启动期 validation 瞬态。
- `ctest -R Unit`:5/5 通过。
- CLI schema:无命令面改动,无需重生成。

## 10. 运行手册

```bash
# 门禁(自动断言 + 退出码)
Darwin/Debug/TestVulkanFroxelRenderGraph; echo $?
# 变体:FROXEL_TEMPORAL=1 / FROXEL_SPGI_AMBIENT=1 / FROXEL_GATE=0(观景)
#      FROXEL_GATE_FRAME=<F> / FROXEL_GATE_AB=0(跳过 G5)

# Sponza 视觉(Vulkan)
Darwin/Debug/TestVulkanSponzaRenderGraph   # + FROXEL=1 FROXEL_MARCH=4
#   FROXEL_UNIFORM / FROXEL_PHASE_G / FROXEL_EXT / FROXEL_AMBIENT /
#   FROXEL_SPGI_AMBIENT=1 / FROXEL_TEMPORAL=1 / SPONZA_CAPTURE(截图)

# Metal
Darwin/Debug/TestModularPipeline           # + FROXEL=1 FROXEL_MARCH=4 ...
#   SPONZA_CAPTURE=/tmp/x.ppm SPONZA_CAPTURE_FRAME=340(进程内截图)
```
