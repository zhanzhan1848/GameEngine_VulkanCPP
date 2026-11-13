# DirectXMath 形状门禁（Mac 侧）

## 用途

用户要求 Windows 上的引擎数学类型改用 DirectXMath（v2=XMFLOAT2、
v3=v3a=XMFLOAT3A、v4=v4a=XMFLOAT4A、m4x4=m4x4a=XMFLOAT4X4A、m3x3=
XMFLOAT3X3、u32v*/s32v* = XMUINT*/XMINT*，运算符由 MathDirectXOps.h 提供，
元素访问改走 MathAccess.h）。Mac 上无法编译真实 DirectXMath（用户明确
拒绝下载），本目录提供一个**自写的形状替身头 + Windows 视角包装**，对
Windows 会编译的全部共享编译单元跑 `clang++ -std=c++20 -fsyntax-only`，
提前抓出调用点残留的不兼容写法：

- `.columns`（simd 矩阵旧成员）
- 浮点向量 `v[i]` 下标（XMFLOAT* 无 operator[]）
- `v4{v3, w}` 组合构造、`v3{1.f}` 标量广播构造（无对应构造函数）
- `m4x4{v4,v4,v4,v4}` 列构造（XMFLOAT4X4A 只有 16 标量 / const float*）
- 找不到匹配运算符（`invalid operands to binary expression`）

## 原理

1. **替身头** `include/DirectXMath.h`：只声明类型形状与函数签名（无实现），
   成员/构造/签名按 Microsoft Learn 文档逐个核对（每个声明旁注 URL）。
   刻意**不给存储类型提供任何运算符、columns 成员或 operator[]**——
   这是门禁要抓的东西。
2. **Windows 视角** `prefix/win_view_prefix.h`（经 `clang++ -include` 注入）：
   先在 Apple 配置下把 TU 集用到的标准头全部预解析（Apple libc++ 头自身
   依赖 `__APPLE__`，直接 `-U__APPLE__` 不可行——沿 runbook R4-7 做法），
   然后 `#undef __APPLE__`/`#undef __MACH__`，`#define _WIN32/_WIN64/
   _MSC_VER(1936)`，再编译目标 TU。`_MSC_VER` 已定义，MSVC 专属分支
   （EntityAPI/ScriptAPI 的 XMFLOAT 分支、Math.h 的 intrin.h/CRC、DllMain）
   都受检；编译器仍是 Apple clang（`__clang__` 保持定义），配
   `-fms-extensions -fdeclspec` 覆盖 `__declspec` 族。
3. **Windows 头垫片** `include/`：windows.h（R21-B 轮验证过的最小面 +
   本轮追加的消息循环/文件属性/COM 分配器面）、intrin.h（_mm_crc32_u64/
   __rdtsc/_mm_pause/__cpuid）、crtdbg.h、direct.h、io.h、process.h、
   wrl.h、sal.h（转发仓内 xatlas vendored sal.h + dxsdk 用法生成的
   补充宏）。D3D12 头用仓内 vendored dxsdk
   （`third_party/xatlas/.../dxsdk/include`）。
4. **TU 清单** `tu_list.txt`（由 `gen_tu_list.py` 再生）：复刻
   Engine/CMakeLists.txt 在 WIN32+ENABLE_VULKAN+ENABLE_D3D12 下的源规则
   （排除 legacy Graphics/Direct3D12、Graphics/Vulkan、Metal、*.mm）+
   EngineDLL + ContentTools + UnitTests（CMake 门控解析，排除 APPLE/
   NOT WIN32/NOT MSVC/EMSCRIPTEN 块）+ IntegrationTests
   （TestSponzaVT、TestRHISponzaShared；WIN32 分支的 legacy TestRenderer
   已在 R13-C 移除）。

## 运行方式

```bash
# 全量（输出 /tmp/dxmath_gate_report/：per_tu/*.log + summary.txt）
./scripts/dxmath_shape_gate/run_gate.sh

# 单个 TU（第 2 阶段迭代用）
./scripts/dxmath_shape_gate/run_gate.sh --tu EngineDLL/EntityAPI.cpp

# 自测（正例必须过、四个负例必须报 MATH 诊断）
cd scripts/dxmath_shape_gate/selftest && bash run_selftest.sh

# 再生成 TU 清单
python3 scripts/dxmath_shape_gate/gen_tu_list.py > scripts/dxmath_shape_gate/tu_list.txt
```

诊断二分类（`classify_report.py`）：
- **MATH（数学相关）**：块内引用 DirectX::/XMFLOAT*/XMVECTOR/XMMATRIX/
  XM* 函数、MathTypes.h/MathAccess.h/MathDirectXOps.h/RHIMath.h/
  primal::math，或 `no member named 'columns'`，或 XMFLOAT* 相关的
  no matching constructor / subscript / invalid operands。
- **PLATFORM（平台/垫片相关）**：其余（缺第三方头、Windows API 面、
  垫片噪声）。

## 排除清单（结构性，垫片无法满足的第三方依赖）

| 编译单元 | 原因 |
| --- | --- |
| ContentTools/FbxImporter.cpp | FBX SDK（fbxsdk.h），Windows 真机验证 |
| ContentTools/TextureImporter.cpp | DirectXTex.h + d3d11.h + WIC（DirectXTex 未 vendored） |
| ContentTools/NormalMapIdentification.cpp | DirectXTex.h（DirectX::Image；同上） |
| EngineDLL/CrashHandler.cpp | dbghelp.h / Win32 SEH 深度面，交 Windows 真机 |

## 局限（必读）

1. **只证语法与类型层面**。真实 DirectXMath 的语义（NaN/精度边界、
   行列主序语义、SIMD 行为）与性能要靠 Windows 真机编译+运行验证；
   本门禁不能替代。
2. 函数**只声明不实现**：constexpr 语境下的 XM 调用会因缺定义报错
   （分类为 PLATFORM 噪声，不是数学缺陷）。
3. 真实 DirectXMath >= 3.14（新 Windows SDK）为 XMFLOAT2/3/4 等存储类型
   提供了 `operator==`/`operator<=>`；本替身按门禁设计**省略**。纯
   `==` 比较被报 MATH 时属已知假阳性，人工分诊（其余算术/下标错误在
   真实头下同样编译不过，非假阳性）。
4. `XMVECTOR` 是不透明 16 字节替身（真实为 __m128/联合体）；XMVECTOR/
   XMMATRIX 的运算符按真实头存在性声明（供调用点匹配）。
5. macOS 默认文件系统大小写不敏感：`<directxmath.h>`（小写，
   PrimitiveMesh.cpp 用）直接解析到 `DirectXMath.h`（实测带
   -Wnonportable-include-path 警告，即同一文件）。大小写敏感卷上需要
   `cp include/DirectXMath.h include/directxmath.h` 兜底（本机无法同时
   创建两个大小写不同名的文件）。
6. sal.h 采用"转发仓内 xatlas vendored sal.h（CC0）+ sal_extra.h 补充"
   的分层：xatlas 的 salieri.h 覆盖通用 SAL，dxsdk d3d12.h 用到的
   _COM_Outptr_/STDMETHOD 等由 R21-B 轮生成的补充宏提供（全部带
   #ifndef 护栏，无重定义冲突）。
7. TU 门控解析对 `if(TARGET …)/if(EXISTS …)` 等不可静态判定条件默认
   "包含"并在 tu_list.txt 组标签里标注 `undecidable-gate`——宁可多查，
   多出的失败也会被分类为 PLATFORM 而非 MATH。
8. `-fsyntax-only` 不做代码生成；模板实例化深度、链接符号等不在覆盖面。

## 文件清单

```
scripts/dxmath_shape_gate/
├── include/
│   ├── DirectXMath.h      # 替身头（类型形状 + 函数声明，注 Learn URL）
│   ├── sal.h              # <sal.h> 转发（xatlas vendored + sal_extra）
│   ├── sal_generated.h / sal_extra.h / guiddef.h / rpc.h / rpcndr.h
│   │                      # R21-B 轮垫片原样拷贝（与 /tmp 原件字节一致）
│   ├── windows.h          # R21-B 原版 + P1b 追加（LPCSTR const 修正、
│   │                      #   消息循环/文件属性/CoTaskMem/DLL_* 面）
│   ├── oaidl.h / ocidl.h / ole2.h / wrl.h / wrl/client.h
│   ├── intrin.h           # _mm_crc32_u64/__rdtsc/_mm_pause/__cpuid
│   └── crtdbg.h / direct.h / io.h / process.h
├── prefix/win_view_prefix.h  # Windows 视角前缀（含 MSVC CRT 补面）
├── gen_tu_list.py         # TU 清单生成器（可再生成）
├── tu_list.txt            # 生成的清单（path \t 组标签）
├── run_gate.sh            # 运行器（--tu 单跑 / 全量并行 + 汇总）
├── run_one_tu.sh          # 单 TU worker（独立脚本：绕开 export -f 在
│                          #   /bin/bash 3.2 与 PATH 里 bash 5 之间的
│                          #   函数编码不兼容）
├── classify_report.py     # 诊断分类与汇总器（含按 file:line 去重视图）
├── selftest/
│   ├── mini_contract.h    # P1 契约最小面（别名+MathAccess+运算符）
│   ├── positive.cpp       # 正例：合法写法必须零错误
│   ├── negative_columns.cpp / negative_subscript.cpp /
│   │   negative_v4_from_v3.cpp / negative_broadcast.cpp   # 四负例
│   └── run_selftest.sh
└── README.md
```
