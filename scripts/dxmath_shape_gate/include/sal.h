// ============================================================================
// 仅供 Mac 语法门禁使用的 <sal.h> 转发头。
//
// 按任务约定优先复用仓内 vendored sal.h（xatlas bx 的 mingw compat，CC0；
// 内部 include 同目录 salieri.h），用相对路径转发（本文件位于
// scripts/dxmath_shape_gate/include/，仓库根在四级之上）；随后叠加
// sal_extra.h —— 它带 #ifndef 护栏，只补 salieri.h 没覆盖、而 vendored
// dxsdk d3d12.h 用到的 SAL/COM 宏（_COM_Outptr_、STDMETHOD 等，R21-B 轮
// 按 dxsdk 用法扫描生成）。
// ============================================================================
#pragma once

// 仓库根 = 本文件向上四级（scripts/dxmath_shape_gate/include → … → 仓库根）
#include "../../../../third_party/xatlas/source/thirdparty/bx/include/compat/mingw/sal.h"

#include "sal_extra.h"
