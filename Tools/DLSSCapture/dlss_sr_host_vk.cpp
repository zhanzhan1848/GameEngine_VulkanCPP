/**
 * dlss_sr_host_vk.cpp — 无头 DLSS SR 触发宿主 v2（NGX Vulkan 组件模式，零 WSI）。
 *
 * 流程（与 NGXWrapper.cpp 官方用法对齐，但不建 surface/swapchain）：
 *   NGX 查询 instance/device 扩展需求 → 建 Vulkan 实例/设备 → VULKAN_Init
 *   → 参数对象 → DLSS feature 创建（权重上传 launch 在此，capture 目标）
 *   → 评估 N 次 → 退出。
 *
 * 编译（GPU 机器，需 libvulkan-dev）:
 *   g++ -O2 -std=c++17 -o dlss_sr_host_vk dlss_sr_host_vk.cpp -ldl -lvulkan
 */
#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

/* ---------------- NGX 类型（官方 defs/params 逐字对齐） ---------------- */
typedef int NVSDK_NGX_Result;
typedef void NVSDK_NGX_Handle;
typedef int NVSDK_NGX_Feature;
typedef void (*PFN_NVSDK_NGX_ProgressCallback)(void);
#define NGX_FAILED(x) (((x) & 0xBAD00000u) != 0)
#define NGX_SUCCESS_V 1

typedef struct ID3D11Resource ID3D11Resource;
typedef struct ID3D12Resource ID3D12Resource;

class NVSDK_NGX_Parameter {
public:
    virtual void Set(const char *, unsigned long long) = 0;
    virtual void Set(const char *, float) = 0;
    virtual void Set(const char *, double) = 0;
    virtual void Set(const char *, unsigned int) = 0;
    virtual void Set(const char *, int) = 0;
    virtual void Set(const char *, ID3D11Resource *) = 0;
    virtual void Set(const char *, ID3D12Resource *) = 0;
    virtual void Set(const char *, void *) = 0;
    virtual NVSDK_NGX_Result Get(const char *, unsigned long long *) const = 0;
    virtual NVSDK_NGX_Result Get(const char *, float *) const = 0;
    virtual NVSDK_NGX_Result Get(const char *, double *) const = 0;
    virtual NVSDK_NGX_Result Get(const char *, unsigned int *) const = 0;
    virtual NVSDK_NGX_Result Get(const char *, int *) const = 0;
    virtual NVSDK_NGX_Result Get(const char *, ID3D11Resource **) const = 0;
    virtual NVSDK_NGX_Result Get(const char *, ID3D12Resource **) const = 0;
    virtual NVSDK_NGX_Result Get(const char *, void **) const = 0;
    virtual void Reset() = 0;
    virtual ~NVSDK_NGX_Parameter() = default;
};

class MapParameter : public NVSDK_NGX_Parameter {
public:
    std::map<std::string, unsigned long long> u;
    std::map<std::string, double> f;
    std::map<std::string, void *> p;
    void Set(const char *n, unsigned long long v) override { u[n] = v; }
    void Set(const char *n, float v) override { f[n] = v; }
    void Set(const char *n, double v) override { f[n] = v; }
    void Set(const char *n, unsigned int v) override { u[n] = v; }
    void Set(const char *n, int v) override { u[n] = (unsigned long long)(long long)v; }
    void Set(const char *n, ID3D11Resource *v) override { p[n] = (void *)v; }
    void Set(const char *n, ID3D12Resource *v) override { p[n] = (void *)v; }
    void Set(const char *n, void *v) override { p[n] = v; }
    NVSDK_NGX_Result Get(const char *n, unsigned long long *v) const override {
        auto it = u.find(n); if (it == u.end()) return -1; *v = it->second; return 1;
    }
    NVSDK_NGX_Result Get(const char *n, float *v) const override {
        auto it = f.find(n); if (it == f.end()) return -1; *v = (float)it->second; return 1;
    }
    NVSDK_NGX_Result Get(const char *n, double *v) const override {
        auto it = f.find(n); if (it == f.end()) return -1; *v = it->second; return 1;
    }
    NVSDK_NGX_Result Get(const char *n, unsigned int *v) const override {
        auto it = u.find(n); if (it == u.end()) return -1; *v = (unsigned)it->second; return 1;
    }
    NVSDK_NGX_Result Get(const char *n, int *v) const override {
        auto it = u.find(n); if (it == u.end()) return -1; *v = (int)(long long)it->second; return 1;
    }
    NVSDK_NGX_Result Get(const char *n, ID3D11Resource **v) const override {
        auto it = p.find(n); if (it == p.end()) return -1; *v = (ID3D11Resource *)it->second; return 1;
    }
    NVSDK_NGX_Result Get(const char *n, ID3D12Resource **v) const override {
        auto it = p.find(n); if (it == p.end()) return -1; *v = (ID3D12Resource *)it->second; return 1;
    }
    NVSDK_NGX_Result Get(const char *n, void **v) const override {
        auto it = p.find(n); if (it == p.end()) return -1; *v = it->second; return 1;
    }
    void Reset() override { u.clear(); f.clear(); p.clear(); }
};

/* NVSDK_NGX_Resource_VK（defs_vk.h 布局） */
typedef enum { NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW = 0,
               NVSDK_NGX_RESOURCE_VK_TYPE_VK_BUFFER = 1 } NVSDK_NGX_Resource_VK_Type_;
typedef struct { VkBuffer Buffer; unsigned int SizeInBytes; } NGX_BufferInfo;
typedef struct {
    union { NGX_BufferInfo BufferInfo; } Resource;
    NVSDK_NGX_Resource_VK_Type_ Type;
    bool ReadWrite;
} NGX_Resource_VK;

/* NVSDK_NGX_FeatureDiscoveryInfo（defs.h 布局） */
typedef void NVSDK_NGX_FeatureCommonInfo_Internal;
typedef struct { const wchar_t *const *Path; unsigned int Length; } NGX_PathListInfo;
typedef void NVSDK_NGX_AppLogCallback(const char *message, int loggingLevel, int sourceComponent);
typedef struct { int MinimumLoggingLevel; } NGX_LoggingInfo;
typedef struct {
    NGX_PathListInfo PathListInfo;
    NVSDK_NGX_FeatureCommonInfo_Internal *InternalData;
    NGX_LoggingInfo LoggingInfo;   /* SDK 0x14 起 */
} NGX_FeatureCommonInfo;
typedef struct {
    const char *ProjectId;
    int EngineType;
    const char *EngineVersion;
} NGX_ProjectIdDescription;
typedef struct {
    int IdentifierType;              /* 0=AppId 1=ProjectId */
    union { NGX_ProjectIdDescription ProjectDesc; unsigned long long ApplicationId; } v;
} NGX_AppIdentifier;
typedef struct {
    unsigned long long SDKVersion;
    int FeatureID;
    NGX_AppIdentifier Identifier;
    const wchar_t *ApplicationDataPath;
    const NGX_FeatureCommonInfo *FeatureInfo;
} NGX_FeatureDiscoveryInfo;

/* ---------------- NGX 运行时导出 ---------------- */
static void *ngx = NULL;
#define NGX_SYM(var, name, req)                                                \
    do {                                                                       \
        *(void **)&var = dlsym(ngx, name);                                     \
        printf("[host] %-56s %p\n", name, (void *)var);                        \
        if (!(var) && req) return 2;                                           \
    } while (0)

static NVSDK_NGX_Result (*ngx_vk_get_feature_instance_ext_req)(
    const NGX_FeatureDiscoveryInfo *, unsigned int *, VkExtensionProperties **);
static NVSDK_NGX_Result (*ngx_vk_get_feature_device_ext_req)(
    VkInstance, VkPhysicalDevice, const NGX_FeatureDiscoveryInfo *,
    unsigned int *, VkExtensionProperties **);
static NVSDK_NGX_Result (*ngx_vk_init)(unsigned long long, const wchar_t *,
                                       VkInstance, VkPhysicalDevice, VkDevice,
                                       unsigned long long);
static NVSDK_NGX_Result (*ngx_vk_get_parameters)(NVSDK_NGX_Parameter **);
static NVSDK_NGX_Result (*ngx_vk_allocate_parameters)(NVSDK_NGX_Parameter **);
static NVSDK_NGX_Result (*ngx_vk_populate_impl)(NVSDK_NGX_Parameter *);
static NVSDK_NGX_Result (*ngx_vk_create_feature)(VkCommandBuffer, NVSDK_NGX_Feature,
                                                 const NVSDK_NGX_Parameter *,
                                                 NVSDK_NGX_Handle **);
static NVSDK_NGX_Result (*ngx_vk_evaluate_feature)(VkCommandBuffer,
                                                   const NVSDK_NGX_Handle *,
                                                   const NVSDK_NGX_Parameter *,
                                                   PFN_NVSDK_NGX_ProgressCallback);
static NVSDK_NGX_Result (*ngx_vk_release_feature)(NVSDK_NGX_Handle *);
static NVSDK_NGX_Result (*ngx_vk_shutdown)(void);
static NVSDK_NGX_Handle *ngx_handle = NULL;
static VkQueue queue = VK_NULL_HANDLE;
static unsigned int (*ngx_param_setui)(NVSDK_NGX_Parameter *, const char *, unsigned int);
static void (*ngx_param_seti)(NVSDK_NGX_Parameter *, const char *, int);
static void (*ngx_param_setull)(NVSDK_NGX_Parameter *, const char *, unsigned long long);
static void (*ngx_param_setf)(NVSDK_NGX_Parameter *, const char *, float);
static void (*ngx_param_setp)(NVSDK_NGX_Parameter *, const char *, void *);

static unsigned long long env_ull(const char *k, unsigned long long d) {
    const char *e = getenv(k);
    return e ? strtoull(e, NULL, 0) : d;
}
static const char *env_str(const char *k, const char *d) {
    const char *e = getenv(k);
    return (e && *e) ? e : d;
}

/* buffer 资源包装（helpers_vk.h 的 Create_Buffer_Resource_VK 等价） */
static NGX_Resource_VK wrap_buffer(VkBuffer buf, unsigned size, bool rw) {
    NGX_Resource_VK r{};
    r.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_BUFFER;
    r.Resource.BufferInfo.Buffer = buf;
    r.Resource.BufferInfo.SizeInBytes = size;
    r.ReadWrite = rw;
    return r;
}

int main(void) {
    const char *lib = env_str("NVCUDA_HOST_LIB", "./libnvidia-ngx-dlss.so.310.7.0");
    const char *data_path = env_str("NVCUDA_HOST_DATA", "/tmp/dlss_sr_host_data");
    const unsigned W = (unsigned)env_ull("NVCUDA_HOST_W", 640);
    const unsigned H = (unsigned)env_ull("NVCUDA_HOST_H", 360);
    const unsigned OW = (unsigned)env_ull("NVCUDA_HOST_OUTW", 1280);
    const unsigned OH = (unsigned)env_ull("NVCUDA_HOST_OUTH", 720);
    const long preset = (long)env_ull("NVCUDA_HOST_PRESET", (unsigned long long)-1LL);
    const unsigned perfq = (unsigned)env_ull("NVCUDA_HOST_PERFQ", 3);
    const long loops = (long)env_ull("NVCUDA_HOST_LOOPS", 3);

    /* ---- 1. NGX 库与符号 ---- */
    ngx = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
    if (!ngx) { fprintf(stderr, "[host] dlopen(%s): %s\n", lib, dlerror()); return 1; }
    NGX_SYM(ngx_vk_get_feature_instance_ext_req, "NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements", 0);
    NGX_SYM(ngx_vk_get_feature_device_ext_req, "NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements", 0);
    NGX_SYM(ngx_vk_init, "NVSDK_NGX_VULKAN_Init", 1);
    NGX_SYM(ngx_vk_get_parameters, "NVSDK_NGX_VULKAN_GetParameters", 0);
    NGX_SYM(ngx_vk_allocate_parameters, "NVSDK_NGX_VULKAN_AllocateParameters", 0);
    NGX_SYM(ngx_vk_populate_impl, "NVSDK_NGX_VULKAN_PopulateParameters_Impl", 0);
    NGX_SYM(ngx_vk_create_feature, "NVSDK_NGX_VULKAN_CreateFeature", 1);
    NGX_SYM(ngx_vk_evaluate_feature, "NVSDK_NGX_VULKAN_EvaluateFeature", 1);
    NGX_SYM(ngx_vk_release_feature, "NVSDK_NGX_VULKAN_ReleaseFeature", 0);
    NGX_SYM(ngx_vk_shutdown, "NVSDK_NGX_VULKAN_Shutdown", 0);
    
    
    
    
    

    /* ---- 1b. 驱动 NGX 支持探测（fork 隔离，崩溃不影响主流程） ---- */
    {
        void *snip = dlsym(ngx, "NGX_SNIPPETS_GetRequiredDriverSupport");
        void *gdrv = dlsym(ngx, "NVSDK_NGX_GetRequiredDriverSupport");
        printf("[probe] snippets_gds=%p gds=%p\n", snip, gdrv);
        if (snip || gdrv) {
            pid_t pid = fork();
            if (pid == 0) {
                unsigned int support = 0xDEAD;
                int (*fn)(unsigned int *) = (int (*)(unsigned int *))(snip ? snip : gdrv);
                int rc = fn(&support);
                fprintf(stderr, "[probe] driver support rc=%d value=0x%x\n", rc, support);
                _exit(0);
            }
            int st = 0; waitpid(pid, &st, 0);
            printf("[probe] status=0x%x %s\n", st,
                   (st & 0x7f) ? "(子进程异常——签名不匹配)" : "");
        }
    }

    /* ---- 2. FeatureDiscoveryInfo ---- */
    std::wstring wpath(data_path, data_path + strlen(data_path));
    NGX_FeatureCommonInfo info{};
    std::wstring wlibdir = L"/tmp";   /* lib 所在目录搜索路径 */
    const wchar_t *paths[] = {wlibdir.c_str()};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.MinimumLoggingLevel = 2;   /* VERBOSE：让 NGX 自己说出拒绝原因 */
    NGX_FeatureDiscoveryInfo dis{};
    dis.SDKVersion = 0x15;
    dis.FeatureID = 1;                /* SuperSampling */
    dis.Identifier.IdentifierType = 1; /* Project_Id */
    dis.Identifier.v.ProjectDesc.ProjectId = "nvcuda-capture";
    dis.Identifier.v.ProjectDesc.EngineType = 0;  /* CUSTOM */
    dis.Identifier.v.ProjectDesc.EngineVersion = "1.0";
    dis.ApplicationDataPath = wpath.c_str();
    dis.FeatureInfo = &info;

    /* ---- 3. instance 扩展需求 → 建 instance ---- */
    unsigned nIExt = 0;
    VkExtensionProperties *iExts = NULL;
    NVSDK_NGX_Result rc = 0;
    if (ngx_vk_get_feature_instance_ext_req)
        rc = ngx_vk_get_feature_instance_ext_req(&dis, &nIExt, &iExts);
    printf("[host] instance ext req rc=0x%x n=%u\n", (unsigned)rc, nIExt);
    std::vector<const char *> iext_names;
    for (unsigned i = 0; i < nIExt; ++i) iext_names.push_back(iExts[i].extensionName);
    /* NGX Vulkan 必需的 instance 扩展（NGX 内部建设备/查询依赖它们） */
    iext_names.push_back("VK_KHR_get_physical_device_properties2");
    iext_names.push_back("VK_KHR_external_memory_capabilities");
    iext_names.push_back("VK_KHR_external_semaphore_capabilities");

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "nvcuda_capture";
    appInfo.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &appInfo;
    ici.enabledExtensionCount = (unsigned)iext_names.size();
    ici.ppEnabledExtensionNames = iext_names.data();
    VkInstance instance;
    if (vkCreateInstance(&ici, NULL, &instance) != VK_SUCCESS) {
        fprintf(stderr, "[host] vkCreateInstance failed\n"); return 1;
    }

    /* ---- 4. 物理/逻辑设备 ---- */
    unsigned ndev = 0;
    vkEnumeratePhysicalDevices(instance, &ndev, NULL);
    std::vector<VkPhysicalDevice> pds(ndev);
    vkEnumeratePhysicalDevices(instance, &ndev, pds.data());
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    for (auto p : pds) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(p, &props);
        printf("[host] PD: %s (type=%d vendor=0x%x)\n", props.deviceName,
               (int)props.deviceType, props.vendorID);
        if (props.vendorID == 0x10DE && props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            pd = p;
    }
    if (pd == VK_NULL_HANDLE && ndev) pd = pds[0];
    if (pd == VK_NULL_HANDLE) { fprintf(stderr, "[host] no physical device\n"); return 1; }

    unsigned nDExt = 0;
    VkExtensionProperties *dExts = NULL;
    if (ngx_vk_get_feature_device_ext_req)
        rc = ngx_vk_get_feature_device_ext_req(instance, pd, &dis, &nDExt, &dExts);
    printf("[host] device ext req rc=0x%x n=%u\n", (unsigned)rc, nDExt);
    std::vector<const char *> dext_names;
    for (unsigned i = 0; i < nDExt; ++i) {
        dext_names.push_back(dExts[i].extensionName);
        printf("[host]   + %s\n", dExts[i].extensionName);
    }
    /* NGX Vulkan 必需的 device 扩展（CUDA interop 依赖） */
    dext_names.push_back("VK_KHR_external_memory_fd");
    dext_names.push_back("VK_KHR_external_semaphore_fd");
    dext_names.push_back("VK_KHR_dedicated_allocation");

    unsigned qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, NULL);
    std::vector<VkQueueFamilyProperties> qps(qcount);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, qps.data());
    unsigned qf = 0;
    for (unsigned i = 0; i < qcount; ++i)
        if (qps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qf = i; break; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = qf;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (unsigned)dext_names.size();
    dci.ppEnabledExtensionNames = dext_names.data();
    VkDevice device;
    if (vkCreateDevice(pd, &dci, NULL, &device) != VK_SUCCESS) {
        fprintf(stderr, "[host] vkCreateDevice failed\n"); return 1;
    }
    printf("[host] device ready\n");

    /* ---- 5. NGX VULKAN Init ---- */
    for (unsigned long long v : {0x15ULL, 0x1EULL, 0x20ULL, 0x28ULL, 0x30ULL}) {
        rc = ngx_vk_init(231313132ULL /*官方 demo APP_ID*/, wpath.c_str(), instance,
                         pd, device, v);
        printf("[host] VULKAN_Init(sdkver=0x%llx) rc=0x%x %s\n", v, (unsigned)rc,
               NGX_FAILED(rc) ? "" : "<- OK");
        if (!NGX_FAILED(rc)) break;
    }
    if (NGX_FAILED(rc)) { fprintf(stderr, "[host] NGX init failed\n"); return 1; }

    /* ---- 6. 参数对象 ---- */
    NVSDK_NGX_Parameter *params = NULL;
    if (ngx_vk_allocate_parameters && ngx_vk_allocate_parameters(&params) == NGX_SUCCESS_V)
        printf("[host] params = AllocateParameters\n");
    else if (ngx_vk_get_parameters && ngx_vk_get_parameters(&params) == NGX_SUCCESS_V)
        printf("[host] params = GetParameters\n");
    else {
        params = (NVSDK_NGX_Parameter *)(new MapParameter());
        printf("[host] params = MapParameter（应用侧）\n");
    }
    if (ngx_vk_populate_impl) {
        pid_t pid = fork();
        if (pid == 0) { ngx_vk_populate_impl(params); _exit(0); }
        int st = 0; waitpid(pid, &st, 0);
        printf("[host] populate(child) st=0x%x\n", st);
    }

    /* ---- 7. 命令池/缓冲 + 资源 buffer ---- */
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = qf;
    VkCommandPool pool;
    if (vkCreateCommandPool(device, &pci, NULL, &pool) != VK_SUCCESS) return 1;
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    if (vkAllocateCommandBuffers(device, &cai, &cmd) != VK_SUCCESS) return 1;

    auto makeBuffer = [&](VkDeviceSize size, VkBuffer *buf, VkDeviceMemory *mem) {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = size;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkExternalMemoryBufferCreateInfo ext{};
        ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        bi.pNext = &ext;
        if (vkCreateBuffer(device, &bi, NULL, buf) != VK_SUCCESS) return false;
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(device, *buf, &mr);
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        unsigned mt = 0xFFFFFFFFu;
        for (unsigned t = 0; t < mp.memoryTypeCount; ++t)
            if ((mr.memoryTypeBits & (1u << t)) &&
                (mp.memoryTypes[t].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                mt = t; break;
            }
        if (mt == 0xFFFFFFFFu) return false;
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = mt;
        if (vkAllocateMemory(device, &ai, NULL, mem) != VK_SUCCESS) return false;
        if (vkBindBufferMemory(device, *buf, *mem, 0) != VK_SUCCESS) return false;
        void *mapped = NULL;
        if (vkMapMemory(device, *mem, 0, size, 0, &mapped) == VK_SUCCESS) {
            memset(mapped, 0, size);
            vkUnmapMemory(device, *mem);
        }
        return true;
    };
    VkBuffer bColor, bMV, bOut;
    VkDeviceMemory mColor, mMV, mOut;
    VkDeviceSize szColor = (VkDeviceSize)W * H * 8, szMV = (VkDeviceSize)W * H * 8,
                 szOut = (VkDeviceSize)OW * OH * 8;
    if (!makeBuffer(szColor, &bColor, &mColor) || !makeBuffer(szMV, &bMV, &mMV) ||
        !makeBuffer(szOut, &bOut, &mOut)) {
        fprintf(stderr, "[host] buffer 失败\n"); return 1;
    }
    {
        void *mapped = NULL;
        if (vkMapMemory(device, mColor, 0, szColor, 0, &mapped) == VK_SUCCESS) {
            unsigned short *px = (unsigned short *)mapped;   /* RGBA16F 渐变 */
            for (unsigned y = 0; y < H; ++y)
                for (unsigned x = 0; x < W; ++x) {
                    unsigned short *p16 = px + ((size_t)y * W + x) * 4;
                    p16[0] = 0x3800; p16[1] = 0x3800; p16[2] = 0x3C00; p16[3] = 0x3C00;
                }
            vkUnmapMemory(device, mColor);
        }
    }
    printf("[host] buffers ready %ux%u -> %ux%u\n", W, H, OW, OH);

    /* ---- 8. DLSS feature 创建（官方 NGX_VULKAN_CREATE_DLSS_EXT1 参数打包） ---- */
    params->Set("CreationNodeMask", 1);
    params->Set("VisibilityNodeMask", 1);
    params->Set("Width", W);
    params->Set("Height", H);
    params->Set("OutWidth", OW);
    params->Set("OutHeight", OH);
    params->Set("PerfQualityValue", (int)perfq);
    params->Set("DLSS.Feature.Create.Flags", 1 | 16);  /* MVLowRes | AutoExposure */
    params->Set("DLSS.Enable.Output.Subrects", 0);
    if (preset >= 0) {
        const char *keys[] = {"DLSS.Hint.Render.Preset.DLAA",
                              "DLSS.Hint.Render.Preset.Quality",
                              "DLSS.Hint.Render.Preset.Balanced",
                              "DLSS.Hint.Render.Preset.Performance",
                              "DLSS.Hint.Render.Preset.UltraPerformance"};
        for (const char *k : keys) params->Set(k, (unsigned long long)preset);
        printf("[host] preset hint = %ld\n", preset);
    }

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;

    vkBeginCommandBuffer(cmd, &bi);
    rc = ngx_vk_create_feature(cmd, (NVSDK_NGX_Feature)1, params, &ngx_handle);
    vkEndCommandBuffer(cmd);
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    printf("[host] CreateFeature rc=0x%x handle=%p\n", (unsigned)rc, (void *)ngx_handle);
    if (NGX_FAILED(rc)) return 1;

    /* ---- 9. 评估 ---- */
    NGX_Resource_VK rColor = wrap_buffer(bColor, (unsigned)szColor, true);
    NGX_Resource_VK rMV = wrap_buffer(bMV, (unsigned)szMV, true);
    NGX_Resource_VK rOut = wrap_buffer(bOut, (unsigned)szOut, true);
    for (long it = 0; it < loops; ++it) {
        params->Set("Reset", it == 0 ? 1 : 0);
        params->Set("Color", &rColor);
        params->Set("Output", &rOut);
        params->Set("MotionVectors", &rMV);
        vkBeginCommandBuffer(cmd, &bi);
        rc = ngx_vk_evaluate_feature(cmd, ngx_handle, params, NULL);
        vkEndCommandBuffer(cmd);
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);
        printf("[host] evaluate %ld rc=0x%x\n", it, (unsigned)rc);
        if (NGX_FAILED(rc)) return 1;
    }
    printf("[host] OK——cap/ 应含权重 launch，跑 triage\n");
    return 0;
}
