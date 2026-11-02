# Vulkan Sponza RC 集成实验

该实验在单独的 `TestVulkanSponzaRC` 可执行文件中运行，使用现有 Sponza 场景、相机、GBuffer、场景 BVH 与 Surface Cache（SC）。实验用于检查世界空间辐射区间的级联合并、视锥驱动更新，以及与当前漫反射 GI 的图像、GPU 时间和资源成本差异。

当前实现与运行结果必须分开看待：程序成功退出只证明配置的八次采样完成且输出有限，不代表画质、性能、动态场景或完整 RC GI 已验收。真实结果以每次独立输出目录中的日志、SHA、`samples.jsonl` 和 `summary.json` 为准。

## 隔离范围

- CMake 的 `PRIMAL_BUILD_RC_INTEGRATION_TEST` 默认 `OFF`；即使显式构建，CTest 条目也设置为 `DISABLED TRUE`。正常 Sponza 可执行文件不编译 `PRIMAL_RC_INTEGRATION_TEST` 宏内的 RC hook。
- RC 使用 `EngineTest/shaders/Vulkan/RC/Cascade.comp` 与构建树内的私有 SPIR-V，不替换生产 Vulkan、Metal 或 Dawn shader。RC 探针区间、合并结果、活动标记、输出、常量与读回缓冲独立分配。
- 测试在当前生产帧 fence 完成后，通过同一 graphics queue 运行 RC 并等待完成。它复用 SC 的现有几何上传入口和共享输入，读取当前 GBuffer、卡片表、深度与辐射 atlas；RC 输出不会反馈到生产 GI 或覆盖生产图像。
- 开关开启时，CMake 将该构建树的整个 `Engine` archive 放到 `${CMAKE_BINARY_DIR}/RC/lib`，避开仓库共用的 Metal/Vulkan archive。应使用专用 Vulkan 构建树；该属性也影响同构建树内链接 `Engine` 的其他目标。

## 算法与空间更新

三个级联覆盖以相机附近为中心的有限世界空间域。当前基础网格为 `16×8×16`、间距 1 m；后续级联空间分辨率依次减半，方向边长依次加倍。方向以等面积的 azimuth/z 分桶采样。追踪阶段记录各距离区间的辐射及开放状态，从远级联向近级联合并，再在 GBuffer 接收表面上积分漫反射 irradiance。

活动探针由世界位置投影到视锥判定，并包含随级联增大的支持 halo。视锥只控制更新优先范围，射线仍查询完整可用场景 BVH。未更新探针可保留已有结果；相机跨过当前 2 m 对齐边界时会整体重置域。这个实现是有限域更新原型，尚非滚动持久世界缓存，也没有证明动态灯光/几何的完整失效策略。

RC 区间区分已知遮挡、已知开放与未知。缺少合格 SC 光照来源时保持未知，不能替换成天空；合法黑色不能当作缺失来源。输出 alpha 表示接收点方向采样的已知覆盖率。图像中的黑色既可能是真实黑，也可能是未知贡献被保守排除，必须结合 coverage 图判断。

## 对照能说明什么

两份图像都是线性漫反射 irradiance，在同一相机、场景、分辨率和当前帧输入下捕获。报告使用共同的固定曝光和显示曲线，不对每份图像单独归一化。与当前 GI 的差异不等于相对物理参考的误差；`rmse_known_only` 只统计 RC 覆盖充分的像素，也不能代替全图画质验收。

当前 RC 的命中光照来自现有 SC radiance，其中可能已经包含现有 GI 的贡献。天空采用 DDGI 的常量 sky 设置；几何查询按不透明三角形处理，没有 alpha-test、透明和完整材质传输。因此该实验比较的是共享供源下的传输/重建原型，尚不能宣称独立求解多反弹 GI，也不能凭此宣布生产算法替换成功。SC 来源资格与缓存失效的具体边界见 `ENGINE_RC_LESSONS.md`。

`rc_gpu_ms` 是 RC GPU dispatch 范围的时间戳，排除生产帧、CPU 等待、资源上传和读回。`baseline_selected_pass_ms` 是现有计时表中选定名称的 pass 求和，可能来自延迟查询，须检查 freshness 与 sequence；它不等价于完整 GI 成本。RC 当前在现有 GI 运行后额外执行，不能将两者时间直接解释为替换后的 FPS 收益。默认八帧采样也不足以认证移动相机、动态灯光或稳定帧时间分布。

显存报告分别记录 RC 请求的资源字节数、共用几何大小，以及 VMA 总 allocation/block bytes。allocation 表示分配占用，block 表示 allocator 保留容量；统一内存设备上二者都不等于独立显存物理驻留量。VMA 前后差值还需排除同期生产资源变化，不能全部自动归因于 RC。

## 构建与运行

从仓库根目录运行，使用明确的独立 Vulkan 构建树：

```bash
cmake -S . -B Darwin/DebugVulkan \
  -DCMAKE_BUILD_TYPE=Debug -DENABLE_VULKAN=ON \
  -DPRIMAL_BUILD_RC_INTEGRATION_TEST=ON
cmake --build Darwin/DebugVulkan --target TestVulkanSponzaRC -j8
./scripts/rc/run.sh /tmp/sponza-rc-run-unique
```

运行需要可访问的 macOS 图形会话。脚本记录输入 SHA、代码版本、host patch、相关实验配置和退出码，使用新目录保存日志与输出。当前可执行文件在第 120 帧开始 RC，完成八次采样后退出；运行时必须显式设置 `RC_TEST_ENABLE=1`，脚本会设置该变量。

也可直接运行：

```bash
RC_TEST_ENABLE=1 RC_OUTPUT=/tmp/sponza-rc-run-unique \
  PRIMAL_FRAME_TIMING=1 SPONZA_TIMING_ONLY=1 \
  Darwin/Debug/TestVulkanSponzaRC
python3 scripts/rc/report.py /tmp/sponza-rc-run-unique
python3 -m unittest discover -s scripts/rc -p 'test_*.py'
```

主要输出为 `samples.jsonl`、配对 irradiance raw、`summary.json` 和可直接打开的 `comparison.html`。覆盖不足、计时来源无效或采样不完整时保留失败证据，不能以“程序运行成功”覆盖这些结果。未归档输入和 raw 不按年龄盲删；先按仓库归档纪律核验 SHA，再清理冗余物理副本。

## engine_* 经验积累

`engine_rc_review` 负责算法与来源边界，`engine_test_validation` 负责报告和验证，`engine_integration_review` 负责隔离契约。可复用经验保存在 `ENGINE_RC_LESSONS.md`：记录失败条件与证据，修复后重新验证，只有验证成立的规则才作为后续任务约束。候选、失败和未验证范围应持续保留。该循环改进项目知识与验证方式，不代表模型权重训练或自动修改代理能力。
