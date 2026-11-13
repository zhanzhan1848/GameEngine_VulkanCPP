#pragma once
#include "sal_generated.h"
// COM interface declaration macros (normally from combaseapi.h)
#ifndef STDMETHOD
#define STDMETHOD(name) virtual HRESULT name
#endif
#ifndef STDMETHOD_
#define STDMETHOD_(type, name) virtual type name
#endif
#ifndef PURE
#define PURE = 0
#endif
#ifndef THIS_
#define THIS_
#endif
#ifndef THIS
#define THIS void
#endif
#ifndef DECLARE_INTERFACE
#define DECLARE_INTERFACE(i) interface i
#endif
#ifndef DECLARE_INTERFACE_
#define DECLARE_INTERFACE_(i, b) interface i : public b
#endif
#ifndef CONST_VTBL
#define CONST_VTBL
#endif
#ifndef EXTERN_C
#define EXTERN_C extern "C"
#endif
#ifndef WINAPI
#define WINAPI
#endif
#ifndef CALLBACK
#define CALLBACK
#endif
#ifndef NATIVE_DECLSPEC_UUID
#define NATIVE_DECLSPEC_UUID(x)
#endif
#ifndef DEFINE_ENUM_FLAG_OPERATORS
#define DEFINE_ENUM_FLAG_OPERATORS(ENUMTYPE) \
    extern "C++" inline ENUMTYPE operator|(ENUMTYPE a, ENUMTYPE b) { return ENUMTYPE(((int)a)|((int)b)); } \
    extern "C++" inline ENUMTYPE &operator|=(ENUMTYPE &a, ENUMTYPE b) { return (ENUMTYPE &)(((int &)a) |= ((int)b)); } \
    extern "C++" inline ENUMTYPE operator&(ENUMTYPE a, ENUMTYPE b) { return ENUMTYPE(((int)a)&((int)b)); } \
    extern "C++" inline ENUMTYPE &operator&=(ENUMTYPE &a, ENUMTYPE b) { return (ENUMTYPE &)(((int &)a) &= ((int)b)); } \
    extern "C++" inline ENUMTYPE operator~(ENUMTYPE a) { return (ENUMTYPE)(~((int)a)); } \
    extern "C++" inline bool operator!(ENUMTYPE a) { return ((int)a) == 0; }
#endif
#ifndef REFGUID
#define REFGUID const GUID&
#endif
