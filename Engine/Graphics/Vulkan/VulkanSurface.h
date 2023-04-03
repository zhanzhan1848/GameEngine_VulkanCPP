// Copyright (c) Contributors of Primal+
// Distributed under the MIT license. See the LICENSE file in the project root for more information.
#pragma once
#include "VulkanCommonHeaders.h"

namespace primal::graphics::vulkan
{
struct swapchain_details
{
    VkSurfaceCapabilitiesKHR		surface_capabilities;	// Surface properties, e.g. image size/extent
    utl::vector<VkSurfaceFormatKHR> formats;				// Surface image formats supported, e.g. RGBA, size of each color
    utl::vector<VkPresentModeKHR>	presentation_modes;		// How images should be presented to screen
};

struct swapchain_image
{
    VkImage		image;
    VkImageView image_view;
};

struct vulkan_swapchain
{
    swapchain_details				details;
    VkSwapchainKHR					swapchain;
    VkFormat						image_format;
    VkExtent2D						extent;
    utl::vector<swapchain_image>	images;
    vulkan_image					depth_attachment;
};

class vulkan_surface
{
public:
    explicit vulkan_surface(platform::window window) : _window{ window }
    {
        assert(window.handle());
    }
    DISABLE_COPY_AND_MOVE(vulkan_surface);
    ~vulkan_surface() { release(); }

    void create(VkInstance instance);
    void present(VkSemaphore image_available, VkSemaphore render_finished, VkFence fence, VkQueue presentation_queue);
    void resize();
    bool recreate_swapchain();
    bool next_image_index(VkSemaphore image_available, VkFence fence, u64 timeout);
    constexpr void set_renderpass_render_area(math::u32v4 render_area) { _renderpass.render_area = render_area; }
    constexpr void set_renderpass_clear_color(math::v4 clear_color) { _renderpass.clear_color = clear_color; }

    [[nodiscard]] constexpr VkFramebuffer& current_framebuffer() { return _framebuffers[_image_index].framebuffer; }
    [[nodiscard]] constexpr vulkan_renderpass& renderpass() { return _renderpass; }
    u32 width() const { return _window.width(); }
    u32 height() const { return _window.height(); }
    constexpr u32 current_frame() const { return _frame_index; }
    constexpr bool is_recreating() const { return _is_recreating; }
    constexpr bool is_resized() const { return _framebuffer_resized; }
    /*constexpr const VkViewport& viewport() const {}
    constexpr const VkRect2D& scissor_rect() const {}*/

    // Own function
    [[nodiscard]] constexpr VkPipeline& pipeline() { return _pipeline; }
    [[nodiscard]] constexpr VkPipelineLayout& pipelineLayout() { return _pipelineLayout; }
    [[nodiscard]] constexpr VkDescriptorSetLayout& descriptorSetLayout() { return _descriptorSetLayout; }
    [[nodiscard]] constexpr utl::vector<VkDescriptorSet>& DescriptorSets() { return _descriptorSets; }
    [[nodiscard]] constexpr baseBuffer& VertexBuffer() { return _vertexBuffer; }
    [[nodiscard]] constexpr baseBuffer& IndexBuffer() { return _indicesBuffer; }
    [[nodiscard]] constexpr std::vector<u16>& Indices() { return _indices; }

private:
    void create_surface(VkInstance instance);
    void create_render_pass();
    bool create_swapchain();
    bool recreate_framebuffers();
    void clean_swapchain();
    void release();

    VkSurfaceKHR					_surface{};
    vulkan_swapchain				_swapchain{};
    vulkan_renderpass				_renderpass{};
    utl::vector<vulkan_framebuffer>	_framebuffers{};
    platform::window				_window{};
    bool							_framebuffer_resized{ false };
    bool							_is_recreating{ false };
    u32								_image_index{ 0 };
    u32								_frame_index{ 0 };

    // Own param
    VkPipelineLayout                _pipelineLayout;
    VkPipeline                      _pipeline;
    VkDescriptorSetLayout           _descriptorSetLayout;
    std::vector<Vertex>             _vertices{ { math::v3{ -0.5f, -0.5f, 0.0f }, math::v3{ 1.0f, 0.0f, 0.0f }, math::v3{1.0f, 0.0f, 0.0f} },
                                                { math::v3{ 0.5f, -0.5f, 0.0f }, math::v3{ 0.0f, 1.0f, 0.0f }, math::v3{0.0f, 0.0f, 0.0f} },
                                                { math::v3{ 0.5f, 0.5f, 0.0f }, math::v3{ 0.0f, 0.0f, 1.0f }, math::v3{0.0f, 1.0f, 0.0f} },
                                                { math::v3{ -0.5f, 0.5f, 0.0f }, math::v3{ 1.0f, 1.0f, 1.0f }, math::v3{1.0f, 1.0f, 0.0f} },
                        
                                                { math::v3{ -0.5f, -0.5f, -0.5f }, math::v3{ 1.0f, 0.0f, 0.0f }, math::v3{1.0f, 0.0f, 0.0f} },
                                                { math::v3{ 0.5f, -0.5f, -0.5f }, math::v3{ 0.0f, 1.0f, 0.0f }, math::v3{0.0f, 0.0f, 0.0f} },
                                                { math::v3{ 0.5f, 0.5f, -0.5f }, math::v3{ 0.0f, 0.0f, 1.0f }, math::v3{0.0f, 1.0f, 0.0f} },
                                                { math::v3{ -0.5f, 0.5f, -0.5f }, math::v3{ 1.0f, 1.0f, 1.0f }, math::v3{1.0f, 1.0f, 0.0f} },
    };
    std::vector<u16>                _indices{ 0, 1, 2, 2, 3, 0,
                                                4, 5, 6, 6, 7, 4};
    std::string                     modelPath{ "C:/Users/27042/Desktop/DX_Test/PrimalMerge/EngineTest/assets/models/viking_room.obj" };
    UniformBufferObject             _ubo;
    baseBuffer                      _vertexBuffer;
    baseBuffer                      _indicesBuffer;
    uniformBuffer                   _uniformBuffer;
    VkDescriptorPool                _descriptorPool;
    utl::vector<VkDescriptorSet>    _descriptorSets;
    vulkan_texture                  _texture;
    // Own function
    void createVertexBuffer(VkDevice device, const std::vector<Vertex>& vertex, baseBuffer& buffer);
    void createIndexBuffer(VkDevice device, const std::vector<u16>& indices, baseBuffer& buffer);
    void createUniformBuffers(VkDevice device, UniformBufferObject& ubo, const u32 width, const u32 height, uniformBuffer& buffer);
    void loadModel(std::string path);


    // Function Pointers
    PFN_vkCreateSwapchainKHR		fpCreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR		fpDestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR		fpGetSwapchainImagesKHR;
    PFN_vkAcquireNextImageKHR		fpAcquireNextImageKHR;
    PFN_vkQueuePresentKHR			fpQueuePresentKHR;
};

swapchain_details get_swapchain_details(VkPhysicalDevice device, VkSurfaceKHR surface);
}