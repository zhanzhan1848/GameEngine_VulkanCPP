// WasmStdIncludeShim.h — WASM 构建的全 TU 强制包含垫片（不经由常规 include 链，
// 由 Engine/CMakeLists.txt 与根 CMakeLists 的 -include 编译选项按绝对路径注入，
// 仅 EMSCRIPTEN 生效）。
//
// 背景：新 libc++（emcc 7.x，CI setup-emsdk "latest"）大幅收敛传递包含，而本仓库
// 数十个 TU 直接使用 std::vector/std::string 却无显式 include——CommonHeaders.h
// 刻意不收 vector（见其头注释），native 平台全靠 libstdc++/旧 libc++ 的传递包含
// 存活。此垫片在 TU 级恢复旧可见性，避免数十文件的手工大 diff。
// 新代码仍应显式 #include <vector> / <string>。
#pragma once

#include <vector>
#include <string>
