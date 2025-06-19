#pragma once

#if defined(_MSC_VER)
#ifndef EDITOR_INTERFACE
#define EDITOR_INTERFACE extern "C" __declspec(dllexport)
#endif // !EDITOR_INTERFACE
#elif defined(__clang__)
#ifndef EDITOR_INTERFACE
#define EDITOR_INTERFACE extern "C" __attribute__((visibility("default")))
#endif // !EDITOR_INTERFACE
#endif