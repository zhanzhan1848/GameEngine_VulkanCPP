// ============================================================================
// 仅供 Mac 语法门禁使用的最小 crtdbg.h 垫片。
// 覆盖仓内用法：EngineDLL/DllMain.cpp、Engine/Core/MainWin32.cpp 的
// _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF)。
// ============================================================================
#pragma once

#define _CRTDBG_ALLOC_MEM_DF   0x01
#define _CRTDBG_LEAK_CHECK_DF  0x20

inline void _CrtSetDbgFlag(int /*f*/)
{
}
