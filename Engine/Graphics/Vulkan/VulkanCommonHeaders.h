// Copyright (c) Contributors of Primal+
// Distributed under the MIT license. See the LICENSE file in the project root for more information.
#pragma once

#include "CommonHeaders.h"
#include "Graphics/Renderer.h"

#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#ifndef NOMINMAX
#define NOMINMAX
#endif // NOMINMAX
#ifndef WIN_32_LEAN_AND_MEAN
#define WIN_32_LEAN_AND_MEAN
#endif // WIN_32_LEAN_AND_MEAN
#include <Windows.h>
#include <wrl.h>
#elif __linux__
#define VK_USE_PLATFORM_XLIB_KHR
#include <iostream>
#include <X11/Xlib.h>
#include <vulkan/vulkan_xlib.h>
#endif // _WIN32

// NOTE: volk comes with Vulkan SDK.
#include <volk/volk.h>

namespace primal::graphics::vulkan {

constexpr u32 frame_buffer_count{ 3 };

struct vulkan_image
{
    VkImage			image;
    VkDeviceMemory	memory;
    VkImageView		view;
    u32				width;
    u32				height;
};

struct vulkan_texture : vulkan_image
{
    VkSampler       sampler;
    VkFormat        format;
};

struct vulkan_renderpass
{
    enum state : u32 {
        READY,
        RECORDING,
        IN_RENDER_PASS,
        RECORDING_ENDED,
        SUBMITTED,
        NOT_ALLOCATED
    };

    VkRenderPass	render_pass;
    math::u32v4		render_area;
    math::v4		clear_color;
    f32				depth;
    u32				stencil;
};

struct vulkan_cmd_buffer
{
    enum state : u32 {
        CMD_READY,
        CMD_RECORDING,
        CMD_IN_RENDER_PASS,
        CMD_RECORDING_ENDED,
        CMD_SUBMITTED,
        CMD_NOT_ALLOCATED
    };

    VkCommandBuffer cmd_buffer;
    state           cmd_state;
};

struct vulkan_framebuffer
{
    VkFramebuffer				framebuffer;
    u32							attach_count;
    utl::vector<VkImageView>	attachments;
    vulkan_renderpass*			renderpass;
};

struct vulkan_fence
{
    VkFence	fence;
    bool	signaled;
};

// Own param
struct baseBuffer
{
    VkBuffer    buffer;
    VkDeviceMemory memory;
};

struct uniformBuffer : baseBuffer
{
    void*   mapped;
};

struct Vertex
{
    math::v3 pos;
    math::v3 color;
    math::v3 texCoord;
    math::v3 normal;
    math::v3 tangent;

    bool operator==(Vertex& v)
    {
        const f32 epsilon = 1.192e-07f;
        return abs(this->pos.x - v.pos.x) < epsilon && abs(this->pos.y - v.pos.y) < epsilon && abs(this->pos.z - v.pos.z) < epsilon &&
            abs(this->color.x - v.color.x) < epsilon && abs(this->color.y - v.color.y) < epsilon && abs(this->color.z - v.color.z) < epsilon &&
            abs(this->texCoord.x - v.texCoord.x) < epsilon && abs(this->texCoord.y - v.texCoord.y) < epsilon && abs(this->texCoord.z - v.texCoord.z) < epsilon &&
            abs(this->normal.x - v.normal.x) < epsilon && abs(this->normal.y - v.normal.y) < epsilon && abs(this->normal.z - v.normal.z) < epsilon;
    }
};

struct geometry_config
{
    u32						vertex_size;
    u32						vertex_count;
    utl::vector<Vertex>		vertices;

    u32						index_size;
    u32						index_count;
    utl::vector<u32>		indices;

    math::v3				center;
    math::v3				min_extents;
    math::v3				max_extents;

    char					name[256];
    char					material_name[256];
    char                    amibent_map[256];
    char                    diffuse_map[256];
    char                    specular_map[256];
    char                    alpha_map[256];
    char                    normal_map[256];
};

struct InstanceData
{
    math::v3 transform;
    math::v3 rotate;
    math::v3 scale;
};

struct UniformBufferObject
{
    math::m4x4 model;
    math::m4x4 view;
    math::m4x4 projection;
};

struct UniformBufferObjectPlus : UniformBufferObject
{
    //math::m4x4  lightModel;
    //math::m4x4  lightView;
    //math::m4x4  lightProjection;
    math::v3    cameraDirection;
    math::v3    cameraPosition;
    f32         lightNear;
    f32         lightFar;
    f32         time;
    f32         pading;
};

struct render_type
{
    enum type : u32
    {
        forward,
        defer,
        both,
        custom,
        
        count
    };
};
}


#ifdef _DEBUG
#ifndef VkCall
#ifdef _WIN32
#define VkCall(x, msg)                              \
if(((VkResult)(x)) != VK_SUCCESS) {                 \
    char line_number[32];                           \
    sprintf_s(line_number, "%u", __LINE__);         \
    OutputDebugStringA("Error in: ");               \
    OutputDebugStringA(__FILE__);                   \
    OutputDebugStringA("\nLine: ");                 \
    OutputDebugStringA(line_number);                \
    OutputDebugStringA("\n");                       \
    OutputDebugStringA(msg);                        \
    OutputDebugStringA("\n");                       \
    __debugbreak();                                 \
}
#elif __linux__
#define VkCall(x, msg)                              \
if (((VkResult)(x)) != VK_SUCCESS) {                \
    std::cout << msg << std::endl;                  \
    __builtin_trap();                               \
}
#endif // _WIN32
#endif // !VkCall
#else
#ifndef VkCall
#define VkCall(x, msg) x
#endif // !VkCall
#endif // _DEBUG

#ifndef MESSAGE
#ifdef _WIN32
#define MESSAGE(x)      \
OutputDebugStringA(x);  \
OutputDebugStringA("\n")
#elif __linux__
#define MESSAGE(x) std::cout << x << std::endl
#endif // _WIN32
#endif // !MESSAGE

#ifndef ERROR_MSSG
#ifdef _WIN32
#define ERROR_MSSG(x)       \
OutputDebugStringA(x);      \
OutputDebugStringA("\n");   \
__debugbreak()
#elif __linux__
#define ERROR_MSSG(x)       \
std::cout << x << std::endl \
__builtin_trap()
#endif // _WIN32
#endif // !ERROR_MSSG

#ifndef GET_INSTANCE_PROC_ADDR
#define GET_INSTANCE_PROC_ADDR(inst, entry)											\
{																					\
    fp##entry = (PFN_vk##entry)vkGetInstanceProcAddr(inst, "vk"#entry);				\
    if (!fp##entry)																	\
        throw std::runtime_error("vkGetInstanceProcAddr failed to find vk"#entry);	\
}
#endif // !GET_INSTANCE_PROC_ADDR

#ifndef GET_DEVICE_PROC_ADDR
#define GET_DEVICE_PROC_ADDR(dev, entry)										    \
{																				    \
    fp##entry = (PFN_vk##entry)vkGetDeviceProcAddr(dev, "vk"#entry);			    \
    if (!fp##entry)																    \
        throw std::runtime_error("vkGetDeviceProcAddr failed to find vk"#entry);    \
}
#endif // !GET_DEVICE_PROC_ADDR