#pragma once
#include "windows.h"
// Minimal COM surface: IUnknown + GUID + IID + uuidof plumbing.
#ifndef _SHIM_GUID_DEFINED
#define _SHIM_GUID_DEFINED
struct _GUID {
    UINT32 Data1; UINT16 Data2; UINT16 Data3; UINT8 Data4[8];
    bool operator==(const _GUID& o) const {
        return Data1==o.Data1 && Data2==o.Data2 && Data3==o.Data3 &&
               (Data4[0]==o.Data4[0] && Data4[1]==o.Data4[1] && Data4[2]==o.Data4[2] &&
                Data4[3]==o.Data4[3] && Data4[4]==o.Data4[4] && Data4[5]==o.Data4[5] &&
                Data4[6]==o.Data4[6] && Data4[7]==o.Data4[7]);
    }
    bool operator!=(const _GUID& o) const { return !(*this == o); }
};
typedef _GUID GUID, IID, CLSID;
typedef const IID& REFIID;
typedef const CLSID& REFCLSID;
#endif
#ifndef _SHIM_IUNKNOWN_DEFINED
#define _SHIM_IUNKNOWN_DEFINED
struct IUnknown {
    virtual HRESULT QueryInterface(const IID&, void**) = 0;
    virtual ULONG AddRef() = 0;
    virtual ULONG Release() = 0;
    virtual ~IUnknown() {}
};
#ifndef interface
#define interface struct
#endif
#ifndef __declspec_uuid_defined_shim
#include "guiddef.h"
#define MIDL_INTERFACE(x) struct __declspec(uuid(x)) __declspec(novtable)
#endif
#define COM_DECLSPEC_NOTHROW
#define __RPC__deref_out
#define __RPC__out
#define __RPC__in
#define __RPC__in_opt
#define __RPC__in_ecount(x)
#define __RPC__out_ecount(x)
#define LPUNKNOWN IUnknown*
template <typename T> inline void** IID_PPV_ARGS_Helper(T** pp) { return reinterpret_cast<void**>(pp); }
// dxsdk declspec uuid + clang __uuidof work via -fms-extensions; provide fallback macro
// for wrl usage below.

#endif // _SHIM_IUNKNOWN_DEFINED
