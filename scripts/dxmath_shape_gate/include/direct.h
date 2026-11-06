// ============================================================================
// 仅供 Mac 语法门禁使用的最小 direct.h 垫片。
// 覆盖仓内用法：ContentTools/ObjImporter.cpp、GltfImporter.cpp 的
// _mkdir/_rmdir/_chdir 族（形状声明）。
// ============================================================================
#pragma once

int _mkdir(const char*);   // MSVC 形状（POSIX mkdir 只有一个参数）
int _rmdir(const char*);
int _chdir(const char*);
char* _getcwd(char*, int);
