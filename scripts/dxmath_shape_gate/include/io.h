// ============================================================================
// 仅供 Mac 语法门禁使用的最小 io.h 垫片。
// 覆盖仓内用法：EngineDLL/DiagnosticsAPI.cpp 的 _open/_write/_close 与
// _O_WRONLY/_O_CREAT/_O_TRUNC/_O_BINARY（fcntl.h 由 macOS SDK 提供，只定义
// 无下划线版本，本垫片负责补下划线映射）；ObjImporter.cpp 的 _access 等。
// ============================================================================
#pragma once

#include <fcntl.h> // O_WRONLY/O_CREAT/O_TRUNC（macOS SDK）

#ifndef _O_WRONLY
#define _O_WRONLY O_WRONLY
#endif
#ifndef _O_CREAT
#define _O_CREAT  O_CREAT
#endif
#ifndef _O_TRUNC
#define _O_TRUNC  O_TRUNC
#endif
#ifndef _O_BINARY
#define _O_BINARY 0
#endif
#ifndef _O_RDONLY
#define _O_RDONLY O_RDONLY
#endif
#ifndef _O_RDWR
#define _O_RDWR   O_RDWR
#endif
#ifndef _O_APPEND
#define _O_APPEND O_APPEND
#endif

int   _open(const char*, int, ...);
int   _write(int, const void*, unsigned int);
int   _close(int);
int   _read(int, void*, unsigned int);
long  _lseek(int, long, int);
int   _unlink(const char*);
int   _access(const char*, int);
#define _access_s(p, m) _access(p, m)
