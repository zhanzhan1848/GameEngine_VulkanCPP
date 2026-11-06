// R21-B syntax-gate shim: minimal Windows surface for dxsdk d3d12.h on macOS clang.
#pragma once
#include <cstddef>
#include <cstdint>
typedef int INT, BOOL;
typedef unsigned char BYTE, UCHAR;
typedef unsigned short WORD;
typedef unsigned int UINT;
typedef unsigned long ULONG, DWORD, COLORREF;
typedef long LONG, HRESULT;
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG;
typedef void* HANDLE;
typedef void* LPVOID, *PVOID;
typedef const void* LPCVOID;
// P1b 修正：原 R21-B 版本把 LPCSTR 一并绑成 char*（丢 const），与真实
// Windows LPCSTR = const char* 不符——GetFileAttributesA("const char*") 等
// 调用点误报 no matching function。此处拆开定义。
typedef char CHAR, *LPSTR;
typedef const char *LPCSTR;
typedef wchar_t WCHAR, *LPWSTR, *LPCWSTR, OLECHAR, *LPOLESTR;
typedef const WCHAR *LPCWCH;
typedef DWORD* LPDWORD;
typedef ULONG *PULONG;
typedef LONG *PLONG;
typedef WORD *LPWORD;
typedef BYTE *LPBYTE;
typedef intptr_t INT_PTR, LONG_PTR;
typedef uintptr_t UINT_PTR, ULONG_PTR, DWORD_PTR, SIZE_T;
typedef intptr_t SSIZE_T, LONGLONG_PTR;
typedef uint64_t UINT64;
typedef int64_t INT64, LONGLONG_2;
typedef uint32_t UINT32; typedef int32_t INT32;
typedef uint16_t UINT16; typedef int16_t INT16;
typedef uint8_t UINT8; typedef int8_t INT8;
typedef float FLOAT;
typedef struct tagRECT { LONG left, top, right, bottom; } RECT, *LPRECT;
typedef struct tagPOINT { LONG x, y; } POINT, *LPPOINT;
typedef struct tagSIZE { LONG cx, cy; } SIZE, *LPSIZE;
typedef RECT* PRECT;
typedef double DOUBLE;
typedef struct HMODULE__* HMODULE;
typedef struct HWND__* HWND;
typedef struct HDC__* HDC;
typedef struct HMONITOR__* HMONITOR;
typedef struct HKEY__* HKEY;
typedef void (*FARPROC)(void);
#ifndef WINAPI
#define WINAPI
#endif
#define APIENTRY
#define CALLBACK
#define STDMETHODCALLTYPE __attribute__((thiscall)) // placeholder; clang x64 ignores
#define STDAPICALLTYPE
#define WINAPIV
#define PASCAL
#define cdecl
#define __cdecl
#define __stdcall
#define DECLSPEC_NOTHROW
#define DECLSPEC_NOVTABLE
#define DECLSPEC_SELECTANY
#define DECLSPEC_UUID(x)
#define DECLSPEC_CHURCH(x)
#define WINAPI_FAMILY_DEFINE
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#ifndef NULL
#define NULL nullptr
#endif
#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_FAIL ((HRESULT)0x80004005L)
#define E_NOINTERFACE ((HRESULT)0x80004002L)
#define E_NOTIMPL ((HRESULT)0x80004001L)
#define E_INVALIDARG ((HRESULT)0x80070057L)
#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)
#define E_POINTER ((HRESULT)0x80004003L)
#define DXGI_ERROR_NOT_FOUND ((HRESULT)0x887A0002L)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
typedef union _LARGE_INTEGER { struct { DWORD LowPart; LONG HighPart; } u; LONGLONG QuadPart; } LARGE_INTEGER, *PLARGE_INTEGER;
typedef struct _SECURITY_ATTRIBUTES { DWORD nLength; LPVOID lpSecurityDescriptor; BOOL bInheritHandle; } SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;
typedef struct _OVERLAPPED { ULONG Internal; ULONG InternalHigh; union { struct { DWORD Offset; DWORD OffsetHigh; } DUMMYSTRUCTNAME; PVOID Pointer; } DUMMYUNIONNAME; HANDLE hEvent; } OVERLAPPED, *LPOVERLAPPED;
#define EVENT_MODIFY_STATE 0x0002
#define SYNCHRONIZE 0x00100000L
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define INFINITE 0xFFFFFFFF
#define CREATE_EVENT_MANUAL_RESET 0x00000001
#define CREATE_EVENT_INITIAL_SET 0x00000002
extern "C" {
HANDLE CreateEventExW(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
HANDLE CreateEventExA(LPSECURITY_ATTRIBUTES, LPCSTR, DWORD, DWORD);
BOOL CloseHandle(HANDLE);
DWORD WaitForSingleObject(HANDLE, DWORD);
DWORD GetLastError(void);
HMODULE LoadLibraryA(LPCSTR);
FARPROC GetProcAddress(HMODULE, LPCSTR);
}
#define CreateEventEx CreateEventExW
#define OutputDebugStringA(x) ((void)0)
#define OutputDebugStringW(x) ((void)0)
#define DebugBreak() ((void)0)
#define InterlockedIncrement(p) (__atomic_add_fetch((volatile long*)(p), 1, __ATOMIC_SEQ_CST))
#define InterlockedDecrement(p) (__atomic_sub_fetch((volatile long*)(p), 1, __ATOMIC_SEQ_CST))
#define MAX_PATH 260
#ifndef DECLARE_HANDLE
#define DECLARE_HANDLE(name) struct name##__; typedef struct name##__* name
#endif
// libc++ __locale flips to MSVCRT ctype-mask mode under _WIN32; supply the
// MSVC CRT masks so <locale> compiles for syntax gating.
#ifndef _UPPER
#define _UPPER 0x01
#define _LOWER 0x02
#define _DIGIT 0x04
#define _SPACE 0x08
#define _PUNCT 0x10
#define _CONTROL 0x20
#define _BLANK 0x40
#define _HEX 0x80
#define _ALPHA (0x0100 | _UPPER | _LOWER)
#define _LEADBYTE 0x8000
#endif

// ---------------------------------------------------------------------------
// 以下为 DirectXMath 形状门禁（P1b，2026-11-06）在 R21-B 原版 windows.h 基础
// 上追加的最小面，服务 Engine/Core/MainWin32.cpp、Platform/PlatformWin32.cpp、
// Input/InputWin32.cpp 的 Win32 消息循环/窗口 API 调用点。原 R21-B 原版以上
// 方为准、未改动。
// ---------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>
typedef char* PSTR;
typedef const char* PCSTR;
typedef wchar_t* PWSTR;
typedef const wchar_t* PCWSTR;
typedef struct HINSTANCE__* HINSTANCE;
typedef struct HICON__* HICON;
typedef struct HCURSOR__* HCURSOR;
typedef struct HBRUSH__* HBRUSH;
typedef struct HMENU__* HMENU;
#ifndef LRESULT_DEFINED_SHIM
#define LRESULT_DEFINED_SHIM
typedef LONG_PTR LRESULT;
#endif
#ifndef WPARAM_DEFINED_SHIM
#define WPARAM_DEFINED_SHIM
typedef UINT_PTR WPARAM;
#endif
#ifndef LPARAM_DEFINED_SHIM
#define LPARAM_DEFINED_SHIM
typedef LONG_PTR LPARAM;
#endif
#ifndef ATOM_DEFINED_SHIM
#define ATOM_DEFINED_SHIM
typedef WORD ATOM;
#endif

// 消息循环（MainWin32.cpp WinMain）；tagPOINT 沿用 R21-B 原版定义（文件头）
typedef struct tagMSG {
    HWND   hwnd;
    UINT   message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD  time;
    struct tagPOINT pt;
} MSG, *LPMSG;

// 文件属性（ContentTools/ToolsCommon.h GetFileAttributesA）
#define INVALID_FILE_ATTRIBUTES  ((DWORD)-1)
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_READONLY  0x00000001
#define FILE_ATTRIBUTE_NORMAL    0x00000080
extern "C" {
DWORD GetFileAttributesA(LPCSTR);
DWORD GetFileAttributesW(LPCWSTR);
}
#define GetFileAttributes GetFileAttributesW

// COM 任务内存分配器（ContentTools/Geometry.cpp、ContentTools.cpp）
extern "C" {
void* CoTaskMemAlloc(std::size_t);
void  CoTaskMemFree(void*);
}

#define ZeroMemory(dst, len) __builtin_memset((dst), 0, (len))

// DLL 入口原因码（EngineDLL/DllMain.cpp）
#define DLL_PROCESS_ATTACH 1
#define DLL_THREAD_ATTACH  2
#define DLL_THREAD_DETACH  3
#define DLL_PROCESS_DETACH 0

// WinDef 基本量
#ifndef HIWORD
#define HIWORD(l) ((WORD)(((DWORD_PTR)(l) >> 16) & 0xffff))
#endif
#ifndef LOWORD
#define LOWORD(l) ((WORD)(((DWORD_PTR)(l)) & 0xffff))
#endif
#define PM_NOREMOVE 0x0000
#define PM_REMOVE   0x0001
#define PM_NOYIELD  0x0002
#define WM_QUIT     0x0012
#define CW_USEDEFAULT ((int)0x80000000)
#define SW_SHOW     5
#define SW_SHOWNORMAL 1
#define WS_OVERLAPPEDWINDOW 0x00CF0000L
#define CS_HREDRAW  0x0002
#define CS_VREDRAW  0x0001
#define COLOR_WINDOW 5
#define IDI_APPLICATION 32512
#define IDC_ARROW      32512

extern "C" {
BOOL PeekMessageW(LPMSG, HWND, UINT, UINT, UINT);
BOOL TranslateMessage(const MSG*);
LRESULT DispatchMessageW(const MSG*);
void PostQuitMessage(int);
HWND CreateWindowExW(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int,
                     HWND, HMENU, HINSTANCE, LPVOID);
BOOL ShowWindow(HWND, int);
BOOL UpdateWindow(HWND);
BOOL DestroyWindow(HWND);
ATOM RegisterClassExW(const void*);
LRESULT DefWindowProcW(HWND, UINT, WPARAM, LPARAM);
HMODULE GetModuleHandleW(LPCWSTR);
}
#define PeekMessage   PeekMessageW
#define DispatchMessage DispatchMessageW
#define CreateWindowEx CreateWindowExW
#define RegisterClassEx RegisterClassExW
#define DefWindowProc DefWindowProcW
#define GetModuleHandle GetModuleHandleW
