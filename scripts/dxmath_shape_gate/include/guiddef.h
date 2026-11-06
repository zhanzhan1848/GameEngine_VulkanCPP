#pragma once
#include "ole2.h"
#ifndef EXTERN_C
#define EXTERN_C extern "C"
#endif
#define FAR
#define NEAR
#define CONST const
#ifndef DEFINE_GUID
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) EXTERN_C const GUID FAR name
#endif
#ifndef DEFINE_GUIDEX
#define DEFINE_GUIDEX(name, str) EXTERN_C const GUID FAR name
#endif
#define __IID_DEFINED__
typedef void* RPC_IF_HANDLE;
typedef void* RPC_BINDING_HANDLE;
struct IRpcStubBuffer; struct IRpcChannelBuffer; struct IRpcStubBuffer; struct IPSFactoryBuffer; struct NDR_SCONTEXT; struct MIDL_STUB_MESSAGE; struct MIDL_STUB_DESC; struct SYNTAX_IDENTIFIER; struct IRpcProxyBuffer; struct ITypeLib; struct ITypeInfo; struct tagRPCOLEMESSAGE; struct IExternalConnection; struct tagRemSNB; struct tagSTGMEDIUM; struct IEnumSTATSTG; struct IEnumMoniker; struct IMarshal; struct IMarshal2; struct IMoniker; struct IStream; struct IStorage; struct IPersistFile; struct IRecordInfo; struct ITypeComp; struct IEnumFORMATETC; struct IEnumSTATDATA; struct IDropTarget; struct IDropSource; struct IAdviseSink; struct IDataObject; struct IRunningObjectTable; struct IBindCtx; struct IClassFactory; struct IServiceProvider; struct ISequentialStream; struct IClientSecurity; struct IServerSecurity; struct IStdMarshalInfo; struct IExternalConnection_;
#define MIDL_SIZEOF_SHORT 2
#define MIDL_SIZEOF_LONG 4
#define MIDL_SIZEOF_INT 4
#define MIDL_SIZEOF_HYPER 8
#define MIDL_ALIGN8 8
#define __midl
typedef struct _MIDL_STUB_MESSAGE* PMIDL_STUB_MESSAGE;
typedef struct _FULL_PTR_XLAT_TABLES* PFULL_PTR_XLAT_TABLES;
typedef struct _NDR_PROC_CONTEXT* PNDR_PROC_CONTEXT;
typedef struct _MIDL_SYNTAX_INFO* PMIDL_SYNTAX_INFO;
struct _NDR_PROC_DESCR;
typedef struct _NDR_PROC_DESCR* PNDR_PROC_DESCR;
struct MIDL_TYPE_PICKLING_INFO;
typedef unsigned long error_status_t;
typedef unsigned short wchar_t_shim_unused_;
typedef GUID UUID;
#define __RPC_STUB
#define __RPC_USER
#define __RPC_API
#define RPC_ENTRY
#include "sal_extra.h"
#define MIDL_INTERFACE_FWD(x) struct
