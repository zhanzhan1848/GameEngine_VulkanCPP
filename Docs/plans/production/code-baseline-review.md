# 通用制作规划：现有代码基线评估

> 日期：2026-10-04；状态：📋 静态复核完成，运行/交互/视觉验收未执行。
> 上级：[总规划](../../2026-10-04-agent-production-and-3d-printing-roadmap.md)。本文是评估记录，不新增领域模块，也不替代专项实施/验收报告。

## 复核范围与证据等级

逐项检查 28 个子规划对应的自有源码：Engine、EngineDLL、ContentTools、PrimalEditor_Avalonia、Primal.CliFramework/PrimalCli、Tools、相关测试以及 wasm。结合声明、实现、生产宿主/编辑器调用与现有测试文件判断，不把 third_party 能力、枚举、注释用例或测试样例当生产能力。

“已有”在本文仅指看到实现；“未贯通”指数据/底层有但关键宿主或编辑路径未接；“未找到”限定本轮入口/关键词检索与源文件审阅，实施时仍需确认相关分支。没有运行构建、测试、软件连接或 UI 操作；既有测试文件的存在不表示当前测试通过。

## 最重要的跨模块调整

1. **Player 是共享断点**：RuntimeSceneExporter 的场景包和 Player 目前偏向显示/录制，未接完整脚本、物理/PFX tick、Sequence、UI、音频等运行链。必须逐模块补导出/加载/生命周期/输入，不能用编辑器预览替代成品验收；也不要求所有模块同时做完才能测一项。
2. **构建清单不等于发布包**：BuildPipeline 标记 PendingCook，依赖收集聚焦 MeshRenderer/MaterialOverride，运行场景含绝对源资产路径。先补组件依赖、产物状态、可搬移资源与部署，再验安装/更新。
3. **现有模块应复用**：PCG 编辑/执行/应用、PFX 程序与缓存、脚本生命周期/反射、物理控制、资产 GUID/索引、Prefab 文档/实例化、编辑器恢复、帧输出/FFmpeg、WASM Demo 已有；规划改为补缺口和交互深度。
4. **不能把相近能力认作完整能力**：GPU StreamingMesh 不是场景分区；SurfaceNets 不等于地形笔刷；meshlet 校验不是打印检查；曲线几何不是 CAD/BIM；raycast/Patrol 数据不是导航；字段枚举不是工程求解器。
5. **渲染仍延后**：只复核代码与接入边界，渲染/材质计划不启动。其他领域使用既有能力；新增蒙皮/场显示/HLOD/UI 等渲染依赖如确有必要，单列待现有专项协调，不擅自承诺完整画面已可用。

## 全模块评估与调整入口

| 子模块 | 代码基线状态 | 调整重点 |
|---|---|---|
| [rendering-quality-and-parity](rendering-quality-and-parity.md) | 延后；已有实现与专项 | 维持全部建设阶段延后；其他模块只接现有能力，新增渲染依赖单列，不因本文复核启动新的 RHI/GI 工作。 |
| [materials](materials.md) | 延后；已有制作链 | 基础阶段改为复核现有制作/同步闭环；子图/层与新表面属于扩展。全部仍随渲染延后。 |
| [software-integrations-and-skills](software-integrations-and-skills.md) | 局部探测已有；统一中心待建 | 不能把所有软件探测写成从零：先把 FFmpeg 探测迁为共享适配/兼容入口；未知来源与静态路径存在不等于 probe 通过。新增中心独立于 MCP 桥。 |
| [agent-workflows-and-artifacts](agent-workflows-and-artifacts.md) | 命令工作流与输出已有；长任务未贯通 | 复用命令、schema、帧捕获与编码；长任务状态/断点/跨软件产物层在其上扩展，不重写第二套工作流或 FFmpeg 实现。 |
| [character-animation](character-animation.md) | 静态导入已有；骨骼生产链未找到 | S1 先审计并锁定含 skins/animations 文件的实际拒绝/忽略语义，再增加角色资产/蒙皮通路；已有静态材质/纹理处理复用。新增蒙皮渲染明确待现有渲染专项协调，不提前判定角色视觉通过。 |
| [scripting-properties-and-events](scripting-properties-and-events.md) | 运行内核已有；编辑器与宿主接通不足 | 不重做脚本内核：先把现有 Lua 适配提升生产目标，统一宿主 tick 顺序并接编辑/序列化/Player；属性元数据补稳定身份、单位/权限，而非另建不兼容反射。 |
| [runtime-ui](runtime-ui.md) | 编辑器控件已有；Player UI 生产链未找到 | S1 先完成后端/打包契约小样与 Player 闭环，再拓展控件；现有字体/主题可评估复用但不能假定可直接跑。需要新增 UI 渲染时单列待协调。 |
| [ai-navigation](ai-navigation.md) | 物理查询/示例数据已有；导航生产链未找到 | 复用物理查询和实体身份，NavMesh/路径跟随仍需新增；Patrol 必须先接脚本生产运行，不能列为已有可用 AI。 |
| [sequence](sequence.md) | 文档/求值/操作/输出已有；通用轨与 Player 待接 | 扩展既有序列文档、求值、Undo 和输出，不重建时间线或编码器；通用轨/事件及 Player 播放按新契约接入，不能把编辑器 sequence.render 等同 Player 已支持。 |
| [3d-printing](3d-printing.md) | 修复/处理已有；制造检查与导出待建 | 复用 Repair 和处理 IR，补制造专用诊断、单位/尺寸、差异与 STL/3MF 输出。不能把 ValidationErrors=0 认定打印合格；默认对副本执行，报告尺寸/拓扑变化。 |
| [physics](physics.md) | 刚体/查询/编辑控制已有；复杂能力待补 | S1 改为复用/验证并补 Player 与形状编辑一致性；不能把凸包/网格底层入口写成未实现，也不能写成 Inspector 已可配置。关节/角色专项另建，不依据 Jolt 自带能力宣称引擎已有。 |
| [fluid](fluid.md) | 表面 Pass 与缓存交换已有；求解器待建 | S1 改为接现有缓存/报告与预览；求解/浮力分离设计。新水面/表面/着色建设延后，现有 Pass 支持须实测；点缓存不自动覆盖任意变拓扑流体网格。 |
| [pfx](pfx.md) | 模块/编辑器/缓存/物理联动已有 | 先查现有预览→Save→Resync 和 Player 断点；复用模块/schema/缓存。GPU 模块、新着色和 GI/雾联动延后，非渲染制作改进可单独推进。 |
| [pcg](pcg.md) | 图/编辑/执行/应用场景已有 | S1 改为验证既有链/模板，不重写编辑器；增量求值、稳定节点/生成实体 ID 和人工覆盖为剩余核心。节点索引不能直接充当长期稳定资产身份。 |
| [terrain-vegetation](terrain-vegetation.md) | SDF/PCG 地形基础；完整编辑系统未找到 | 撤回笼统的完整地形流送基础描述。S1 先定高度场或体素路线及资产/Undo，复用适配 SDF/PCG；不可混用两种数据模型。新地形/植被渲染优化仍延后。 |
| [prefab](prefab.md) | 序列化/实例化/覆盖容器已有；传播编辑未贯通 | 不重做文档和实例化；优先稳定源对象→实例映射、覆盖生成/Apply/Revert/传播与多选。字典存在不等于覆盖管理已可用；嵌套/变体放后续。 |
| [input-camera](input-camera.md) | 输入绑定/编辑视口已有；成品动作闭环待补 | 复用内核与视口交互，新增动作资产、上下文与 Player 事件桥；编辑相机和项目相机明确隔离，不把 orbit UI 当成可用运行相机框架。 |
| [audio](audio.md) | 识别/元信息已有；播放生产链未找到 | S1 先接解码/播放/资源打包，不重复分类与浏览器；waveform/空间/混音分阶段做，音频枚举/图标不能写成已可预听。 |
| [asset-pipeline](asset-pipeline.md) | 身份/索引/依赖/导入/烘焙已有；端到端需补 | 不重建 GUID/索引/插件注册。先补组件依赖覆盖、实际 cooked 产物状态与重导入修改保留；增量缓存/迁移复用现有契约，不能把离线重建清单当完整烘焙包。 |
| [large-world](large-world.md) | 资源流送/跨场景引用数据已有；实体分区未贯通 | 不重复资源页流送或引用身份；新增分区调度、实体生命周期/引用解析/状态保留，先用简单网格验收。新 HLOD 渲染/大坐标渲染改动延后协调。 |
| [editor-collaboration](editor-collaboration.md) | 编辑/恢复/迁移已有；协作深度待补 | S1 以真实操作查找事务/响应/布局断点；复用恢复和迁移。语义差异/冲突合并与跨资产事务才是新增，不另造 autosave 系统。 |
| [release-maintenance](release-maintenance.md) | 离线清单/场景 Player/录制已有；完整发布待补 | S1 优先可搬移资源包、完整组件导出/加载与宿主 tick；不能直接承诺干净机器成品。安装更新/回退是后续。编辑器 runtime 预览与独立 Player 必须分别验。 |
| [industrial-models](industrial-models.md) | 曲线几何/通用网格已有；行业语义待建 | 复用曲线/测量基础与 meta 身份，新增行业装配/属性/源部件映射；不要重做简单线曲线工具，也不能把这些 API 宣称完整 CAD/BIM。 |
| [device-data](device-data.md) | 可复用事件/属性/身份；设备连接器未找到 | S1 用模拟源建立时间/质量/单位记录，绑定走现有属性/事件适配；真实协议后接，不能把 CLI 或 EngineEventsBridge 当设备连接器。 |
| [twin-tools](twin-tools.md) | 编辑查询/曲线测量基础；成品孪生待建 | 复用对象查找/身份/基础测量；新增运行查询 UI、设备语义、规则与历史模式。曲线距离不等于任意工业网格精确测量/剖切。 |
| [gis](gis.md) | 本地坐标/空间数据基础；GIS 接入未找到 | S1 必须先定义独立源坐标精度与本地渲染锚点，不简单把经纬度塞 Transform；坐标数据/本地参考算例先做，大坐标渲染建设待协调。 |
| [engineering-simulation](engineering-simulation.md) | 几何/场/缓存交换可复用；工程求解接入未找到 | 先做独立结果数据/单位/参考算例，可适配已有场/缓存；不要另造无需求缓存格式，也不能强行把任意单元/场塞到粒子格式。专业求解器适配另验；新增场着色延后。 |
| [web-cloud-delivery](web-cloud-delivery.md) | WASM 演示/构建已有；产品交付待补 | S1 从 Demo 与资源清单改为项目包入口、版本/加载/交互验收，复用页面与构建；新增 Dawn 画质/后端扩展延后。云会话仍为独立可选路线，不能因 Demo 通过算两种交付已完成。 |

每个链接文档均已加入独立复核表，包含可点击源码、实现证据、限制和阶段调整。运行支持确认优先追踪“组件/资产文档→编辑保存→原生同步→导出→Player 加载→tick/输入→关闭”，不能只查看某个 API 头文件。

## 后续立项门禁

28 份子规划现已深化为独立实施合同：各含数据/接口/保存/错误恢复、模块与阶段不做项、S1–S3 进入/退出条件、每阶段四项任务及八类用例。共同门禁见 [严格阶段合同](stage-acceptance-contract.md)，证据填写 [验收模板](acceptance-record-template.md)。这属于规划深化，没有新增代码审计的运行结果，未将任何模块标为已验收；渲染/材质仍延后。

- 对已有能力，先用现有样例/测试和真实 UI 验证复用范围，不再次从零实现。
- 对关键契约缺口，先固定 schema、单位/身份、组件依赖、宿主执行顺序与失效/错误语义。
- 对源码注释与实现不一致（如 glTF skins/animations），先添加明确输入的审计用例，再选择拒绝或支持路线；本轮仅登记风险，没有修改代码。
- 各模块仍满足总规划四门禁；如因渲染待支持或没有非实现者交互验证，写明待验收，不能宣称无 bug 或已易用。
