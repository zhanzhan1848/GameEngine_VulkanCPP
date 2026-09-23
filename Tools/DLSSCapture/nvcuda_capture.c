/**
 * nvcuda_capture.c — DLSS SR 权重抓取工具（LD_PRELOAD）。
 *
 * 原理：权重在 libnvidia-ngx-dlss.so 里是运行时组装的（MLX-DLSS
 * package_dlss_sr.py 的结论），唯一入口是拦截 cuLaunchKernel：用
 * cuFuncGetParamInfo 把 kernel 参数块线性化（offset/size 与 package-sr
 * 期望的 L*_params.bin 布局一致），把参数块中落入已跟踪显存分配的指针
 * 所指的整个 allocation dump 成
 *   L<序号>_params.bin + L<序号>_buf<基址十六进制>.bin
 *
 * 用法：
 *   mkdir cap && NVCUDA_CAPTURE_DIR=$PWD/cap \
 *     LD_PRELOAD=$PWD/nvcuda_capture.so ./dlss_sr_host
 * 之后 python3 triage_capture.py cap/ out/ 按 WEIGHTS/FILTER SHA 自动分拣出
 * package-sr 需要的 L0001_* 与 L0012_*。
 *
 * env：
 *   NVCUDA_CAPTURE_DIR      输出目录（默认 cap）
 *   NVCUDA_CAPTURE_MAX      最多记录的 launch 数（默认 2000）
 *   NVCUDA_CAPTURE_QUIET=1  不打 stderr 日志
 *
 * 已知边界（v1）：分配跟踪覆盖 cuMemAlloc/cuMemAllocPitch/cudaMalloc/
 * cudaMallocPitch；不覆盖 dlsym 旁路（cudart-static 场景）与 VMM
 * （cuMemCreate/cuMemMap）。若 triage 报"权重未命中"，按提示扩展。
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <vulkan/vulkan.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int CUresult;
typedef struct CUfunc_st *CUfunction;
typedef unsigned long long CUdeviceptr;
typedef int cudaError_t;
struct CUlaunchConfig;   /* 仅指针使用 */

/* ---------------- 日志 ---------------- */
static void logf_(const char *fmt, ...) {
    static int quiet = -1;
    if (quiet < 0) {
        const char *e = getenv("NVCUDA_CAPTURE_QUIET");
        quiet = (e && e[0] == '1') ? 1 : 0;
    }
    if (quiet) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("[nvcuda_capture] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static const char *cap_dir(void) {
    static const char *dir;
    if (!dir) {
        const char *e = getenv("NVCUDA_CAPTURE_DIR");
        dir = (e && *e) ? e : "cap";
    }
    return dir;
}
static long cap_max(void) {
    static long v;
    if (!v) {
        const char *e = getenv("NVCUDA_CAPTURE_MAX");
        v = e ? atol(e) : 2000;
        if (v <= 0) v = 2000;
    }
    return v;
}

/* ---------------- 分配表 ---------------- */
typedef struct {
    CUdeviceptr base;
    size_t size;
} Alloc;
#define MAX_ALLOCS 8192
static Alloc g_allocs[MAX_ALLOCS];
static int g_alloc_count;
static long g_launch_index;
static int g_capturing = 1;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static void track(CUdeviceptr base, size_t size) {
    pthread_mutex_lock(&g_mu);
    if (g_alloc_count < MAX_ALLOCS) {
        g_allocs[g_alloc_count].base = base;
        g_allocs[g_alloc_count].size = size;
        ++g_alloc_count;
    }
    pthread_mutex_unlock(&g_mu);
}
static void untrack(CUdeviceptr base) {
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_alloc_count; ++i)
        if (g_allocs[i].base == base) {
            g_allocs[i] = g_allocs[g_alloc_count - 1];
            --g_alloc_count;
            break;
        }
    pthread_mutex_unlock(&g_mu);
}
static const Alloc *covering(CUdeviceptr addr) {
    const Alloc *best = NULL;
    for (int i = 0; i < g_alloc_count; ++i) {
        const Alloc *a = &g_allocs[i];
        if (addr >= a->base && addr < a->base + a->size)
            if (!best || a->size < best->size) best = a;
    }
    return best;
}

/* ---------------- 原始函数（惰性绑定） ---------------- */
static CUresult (*real_cuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned,
                                       unsigned, unsigned, unsigned, unsigned,
                                       void *, void **, void **);
static CUresult (*real_cuLaunchKernelEx)(const struct CUlaunchConfig *,
                                         CUfunction, void **, void **);
static CUresult (*real_cuMemAlloc)(CUdeviceptr *, size_t);
static CUresult (*real_cuMemAllocPitch)(CUdeviceptr *, size_t *, size_t, size_t,
                                        unsigned);
static CUresult (*real_cuMemFree)(CUdeviceptr);
static cudaError_t (*real_cudaMalloc)(void **, size_t);
static cudaError_t (*real_cudaFree)(void *);
static CUresult (*real_cuMemcpyDtoH)(void *, CUdeviceptr, size_t);

#define BIND(field, name)                                                      \
    do {                                                                       \
        if (!field) field = (void *)dlsym(RTLD_NEXT, name);                    \
    } while (0)

static int (*gpinfo)(CUfunction, size_t, size_t *, size_t *);
static void bind_gpinfo(void) {
    static int tried;
    if (tried) return;
    tried = 1;
    void *h = dlopen("libcuda.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen("libcuda.so", RTLD_NOW | RTLD_GLOBAL);
    if (h) gpinfo = (int (*)(CUfunction, size_t, size_t *, size_t *))dlsym(
               h, "cuFuncGetParamInfo");
    logf_("cuFuncGetParamInfo=%s", gpinfo ? "ok" : "MISSING（退化为 8 字节步长）");
}

/* ---------------- launch 处理 ---------------- */
static void handle_launch(CUfunction func, void *const *kernelParams) {
    pthread_mutex_lock(&g_mu);
    if (!g_capturing || g_launch_index >= cap_max()) {
        pthread_mutex_unlock(&g_mu);
        return;
    }
    long idx = ++g_launch_index;
    pthread_mutex_unlock(&g_mu);
    bind_gpinfo();

    unsigned char block[4096];
    size_t block_len = 0;
    int used_gpinfo = 0;

    if (kernelParams && gpinfo) {
        used_gpinfo = 1;
        for (size_t i = 0;; ++i) {
            size_t off = 0, sz = 0;
            if (gpinfo(func, i, &off, &sz) != 0) break;   /* 无更多参数 */
            if (sz == 0 || off + sz > sizeof(block)) {
                used_gpinfo = 0;
                break;
            }
            if (off + sz > block_len) block_len = off + sz;
            memcpy(block + off, kernelParams[i], sz);
        }
        if (block_len == 0) used_gpinfo = 0;
    }
    if (!used_gpinfo && kernelParams) {
        /* 退化路径：按 8 字节步长线性化（triage 以 SHA 兜底验证） */
        for (size_t i = 0; i < 512; ++i) {
            if (!kernelParams[i]) break;
            if ((i + 1) * 8 > sizeof(block)) break;
            memcpy(block + i * 8, kernelParams[i], 8);
            if ((i + 1) * 8 > block_len) block_len = (i + 1) * 8;
        }
    }
    if (block_len == 0) return;

    char path[512];
    snprintf(path, sizeof(path), "%s/L%04ld_params.bin", cap_dir(), idx);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(block, 1, block_len, f);
    fclose(f);

    /* 参数块内的 8 字节对齐值 → 命中已跟踪分配则整块 dump */
    BIND(real_cuMemcpyDtoH, "cuMemcpyDtoH");
    if (!real_cuMemcpyDtoH) return;
    for (size_t off = 0; off + 8 <= block_len; off += 8) {
        uint64_t v;
        memcpy(&v, block + off, 8);
        if (v < 0x10000) continue;
        const Alloc *a = covering((CUdeviceptr)v);
        if (!a) continue;
        snprintf(path, sizeof(path), "%s/L%04ld_buf%llx.bin", cap_dir(), idx,
                 (unsigned long long)a->base);
        FILE *probe = fopen(path, "rb");
        if (probe) { fclose(probe); continue; }   /* 同 launch 同分配只 dump 一次 */
        unsigned char *host = malloc(a->size);
        if (!host) continue;
        if (real_cuMemcpyDtoH(host, a->base, a->size) == 0) {
            FILE *bf = fopen(path, "wb");
            if (bf) {
                fwrite(host, 1, a->size, bf);
                fclose(bf);
                logf_("L%04ld buf 0x%llx size=%zu", idx,
                      (unsigned long long)a->base, a->size);
            }
        }
        free(host);
    }
    if (idx % 50 == 1)
        logf_("launch %ld params=%zuB gpinfo=%d", idx, block_len, used_gpinfo);
}

/* ---------------- 导出钩子 ---------------- */
CUresult cuLaunchKernel(CUfunction f, unsigned gx, unsigned gy, unsigned gz,
                        unsigned bx, unsigned by, unsigned bz, unsigned smem,
                        void *hStream, void **kernelParams, void **extra) {
    BIND(real_cuLaunchKernel, "cuLaunchKernel");
    handle_launch(f, kernelParams);
    return real_cuLaunchKernel(f, gx, gy, gz, bx, by, bz, smem, hStream,
                               kernelParams, extra);
}

CUresult cuLaunchKernelEx(const struct CUlaunchConfig *config, CUfunction f,
                          void **kernelParams, void **extra) {
    BIND(real_cuLaunchKernelEx, "cuLaunchKernelEx");
    handle_launch(f, kernelParams);
    return real_cuLaunchKernelEx(config, f, kernelParams, extra);
}

CUresult cuMemAlloc(CUdeviceptr *dptr, size_t size) {
    BIND(real_cuMemAlloc, "cuMemAlloc");
    CUresult rc = real_cuMemAlloc(dptr, size);
    if (rc == 0 && size >= 4096) {
        track(*dptr, size);
        logf_("alloc 0x%llx size=%zu", (unsigned long long)*dptr, size);
    }
    return rc;
}

CUresult cuMemAllocPitch(CUdeviceptr *dptr, size_t *pitch, size_t w, size_t h,
                         unsigned elem) {
    BIND(real_cuMemAllocPitch, "cuMemAllocPitch");
    CUresult rc = real_cuMemAllocPitch(dptr, pitch, w, h, elem);
    if (rc == 0 && *pitch * h >= 4096) track(*dptr, *pitch * h);
    return rc;
}

CUresult cuMemFree(CUdeviceptr p) {
    BIND(real_cuMemFree, "cuMemFree");
    untrack(p);
    return real_cuMemFree(p);
}

cudaError_t cudaMalloc(void **p, size_t size) {
    BIND(real_cudaMalloc, "cudaMalloc");
    cudaError_t rc = real_cudaMalloc(p, size);
    if (rc == 0 && size >= 4096) track((CUdeviceptr)*p, size);
    return rc;
}

cudaError_t cudaFree(void *p) {
    BIND(real_cudaFree, "cudaFree");
    untrack((CUdeviceptr)p);
    return real_cudaFree(p);
}

/* ================= WSI 纯合成 + 假 swapchain（C 版，本地最小 VK ABI） ========
 * 无头 NVIDIA X 上 ICD 的 WSI 是坏的：present 查询 UnknownError、swapchain
 * 创建段错误。全部 surface 查询纯合成应答；swapchain 发放自建真实 VkImage
 * （GPU 侧可渲染），present 空转。NGX 权重上传在 feature 创建阶段，
 * 与 swapchain 无关。 queue 从 vkGetDeviceQueue 捕获，acquire 用它信号量。 */
static void *g_vkinstance;
static void *g_queue;
static unsigned g_acq;

struct FakeSwapchain {
    VkDevice device;
    unsigned count;
    VkImage images[4];
    VkDeviceMemory mems[4];
    unsigned w, h, fmt;
};
static struct FakeSwapchain *g_sc;

static PFN_vkCreateImage real_vkCreateImage_;
static PFN_vkGetImageMemoryRequirements real_vkGetImageMemReq_;
static PFN_vkAllocateMemory real_vkAllocMem_;
static PFN_vkBindImageMemory real_vkBindImageMem_;
static PFN_vkQueueSubmit real_vkQueueSubmit_;
static PFN_vkGetSwapchainImagesKHR real_vkGetSwapchainImagesKHR_;

static unsigned fake_create_swapchain(VkDevice device, const void *ciVoid, void **out) {
    const VkSwapchainCreateInfoKHR *ci = (const VkSwapchainCreateInfoKHR *)ciVoid;
    unsigned fmt = (unsigned)ci->imageFormat, w = ci->imageExtent.width,
             h = ci->imageExtent.height, minc = ci->minImageCount;
    if (w == 0 || h == 0) { w = 1280; h = 720; }
    if (minc < 2) minc = 2;
    if (minc > 4) minc = 4;

    struct FakeSwapchain *sc = (struct FakeSwapchain *)calloc(1, sizeof(*sc));
    sc->device = device; sc->w = w; sc->h = h; sc->fmt = fmt; sc->count = minc;

    for (unsigned i = 0; i < sc->count; ++i) {
        VkImageCreateInfo ii;
        memset(&ii, 0, sizeof(ii));
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = ci->imageFormat;
        ii.extent.width = w; ii.extent.height = h; ii.extent.depth = 1;
        ii.mipLevels = 1; ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (real_vkCreateImage_(device, &ii, NULL, &sc->images[i]) != VK_SUCCESS) {
            sc->count = i; break;
        }
        VkMemoryRequirements mr;
        real_vkGetImageMemReq_(device, sc->images[i], &mr);
        VkMemoryAllocateInfo ai;
        memset(&ai, 0, sizeof(ai));
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = 0;
        for (unsigned t = 0; t < 32; ++t)
            if (mr.memoryTypeBits & (1u << t)) { ai.memoryTypeIndex = t; break; }
        if (real_vkAllocMem_(device, &ai, NULL, &sc->mems[i]) != VK_SUCCESS) {
            sc->count = i; break;
        }
        real_vkBindImageMem_(device, sc->images[i], sc->mems[i], 0);
    }
    *out = sc;
    logf_("fake swapchain %ux%u images=%u", w, h, sc->count);
    return 0;
}

static int pd_is_nvidia(VkPhysicalDevice pd) {
    static PFN_vkGetPhysicalDeviceProperties getprops = NULL;
    if (!getprops)
        getprops = (PFN_vkGetPhysicalDeviceProperties)dlsym(RTLD_NEXT, "vkGetPhysicalDeviceProperties");
    if (!getprops) return 0;
    VkPhysicalDeviceProperties props;
    memset(&props, 0, sizeof(props));
    getprops(pd, &props);
    return (props.vendorID == 0x10DE && props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? 1 : 0;
}

VkResult vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice pd, VkSurfaceKHR s,
                                                   uint32_t *count, VkPresentModeKHR *modes) {
    if (!count) return VK_SUCCESS;
    if (modes) modes[0] = VK_PRESENT_MODE_FIFO_KHR;
    *count = 1;
    return VK_SUCCESS;
}
VkResult vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice pd, VkSurfaceKHR s,
                                              uint32_t *count, VkSurfaceFormatKHR *fmts) {
    if (!count) return VK_SUCCESS;
    if (fmts) {
        fmts[0].format = VK_FORMAT_B8G8R8A8_UNORM;
        fmts[0].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    }
    *count = 1;
    return VK_SUCCESS;
}
VkResult vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice pd, VkSurfaceKHR s,
                                                   VkSurfaceCapabilitiesKHR *caps) {
    memset(caps, 0, sizeof(*caps));
    caps->minImageCount = 2; caps->maxImageCount = 4;
    caps->currentExtent.width = 1280; caps->currentExtent.height = 720;
    caps->minImageExtent.width = 64; caps->minImageExtent.height = 64;
    caps->maxImageExtent.width = 8192; caps->maxImageExtent.height = 8192;
    caps->maxImageArrayLayers = 1;
    caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    caps->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT;
    return VK_SUCCESS;
}
VkResult vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice pd, unsigned qf,
                                              VkSurfaceKHR s, VkBool32 *supported) {
    if (supported) *supported = pd_is_nvidia(pd) ? VK_TRUE : VK_FALSE;
    (void)qf; (void)s;
    return VK_SUCCESS;
}

VkResult vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *ci,
                              const VkAllocationCallbacks *alloc, VkSwapchainKHR *out) {
    return fake_create_swapchain(device, ci, out);
}
void vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR handle,
                           const VkAllocationCallbacks *alloc) {
    struct FakeSwapchain *sc = (struct FakeSwapchain *)handle;
    if (!sc) return;
    PFN_vkFreeMemory freeMem = NULL;
    PFN_vkDestroyImage destroyImage = NULL;
    for (unsigned i = 0; i < sc->count; ++i) {
        if (sc->mems[i]) freeMem(device, sc->mems[i], NULL);
        if (sc->images[i]) destroyImage(device, sc->images[i], NULL);
    }
    free(sc);
}
VkResult vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR handle,
                                 uint32_t *count, VkImage *images) {
    struct FakeSwapchain *sc = (struct FakeSwapchain *)handle;
    if (!count) return VK_SUCCESS;
    if (!images) { *count = sc ? sc->count : 0; return VK_SUCCESS; }
    unsigned n = sc ? sc->count : 0;
    unsigned i = 0;
    for (; i < n && i < *count; ++i) images[i] = sc->images[i];
    *count = i;
    return VK_SUCCESS;
}
VkResult vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR handle,
                               uint64_t timeout, VkSemaphore semaphore,
                               VkFence fence, uint32_t *index) {
    static unsigned rr = 0;
    if (index) *index = (rr++) % 3;
    if (g_queue) {
        VkSubmitInfo si;
        memset(&si, 0, sizeof(si));
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.signalSemaphoreCount = semaphore ? 1 : 0;
        si.pSignalSemaphores = semaphore ? &semaphore : NULL;
        if (real_vkQueueSubmit_)
            real_vkQueueSubmit_(g_queue, si.signalSemaphoreCount ? 1 : 0,
                                si.signalSemaphoreCount ? &si : NULL, fence);
    }
    return VK_SUCCESS;
}
VkResult vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info) {
    return VK_SUCCESS;
}

static int wsi_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("NVCUDA_CAPTURE_WSI_FIX");
        v = (!e || e[0] != '0') ? 1 : 0;
    }
    return v;
}

PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    static void *(*real)(void *, const char *);
    if (!real) real = (void *(*)(void *, const char *))dlsym(RTLD_NEXT, "vkGetInstanceProcAddr");
    if (name && wsi_enabled()) {
        if (!strcmp(name, "vkGetPhysicalDeviceSurfacePresentModesKHR"))
            return (void *)vkGetPhysicalDeviceSurfacePresentModesKHR;
        if (!strcmp(name, "vkGetPhysicalDeviceSurfaceFormatsKHR"))
            return (void *)vkGetPhysicalDeviceSurfaceFormatsKHR;
        if (!strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
            return (void *)vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
        if (!strcmp(name, "vkGetPhysicalDeviceSurfaceSupportKHR"))
            return (void *)vkGetPhysicalDeviceSurfaceSupportKHR;
        if (!strcmp(name, "vkCreateSwapchainKHR"))
            return (void *)vkCreateSwapchainKHR;
        if (!strcmp(name, "vkGetSwapchainImagesKHR"))
            return (void *)vkGetSwapchainImagesKHR;
        if (!strcmp(name, "vkAcquireNextImageKHR"))
            return (void *)vkAcquireNextImageKHR;
        if (!strcmp(name, "vkQueuePresentKHR"))
            return (void *)vkQueuePresentKHR;
    }
    return (PFN_vkVoidFunction)(real ? real(instance, name) : NULL);
}

static void (*real_vkGetDeviceQueue_)(VkDevice, unsigned, unsigned, void *);
void vkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *queue) {
    BIND_NEXT(real_vkGetDeviceQueue_, "vkGetDeviceQueue");
    real_vkGetDeviceQueue_(device, family, index, queue);
    if (queue && !g_queue) { g_queue = (void *)*queue; logf_("queue 捕获 %p", g_queue); }
}
__attribute__((constructor)) static void init_(void) {
    logf_("loaded dir=%s max=%ld", cap_dir(), cap_max());
    bind_gpinfo();
}
