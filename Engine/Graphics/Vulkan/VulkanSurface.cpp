// Copyright (c) Contributors of Primal+
// Distributed under the MIT license. See the LICENSE file in the project root for more information.
#include "VulkanSurface.h"
#include "VulkanCore.h"
#include "VulkanResources.h"
#include "VulkanRenderPass.h"
#include "VulkanHelpers.h"

// Own header file
#include "VulkanTexture.h"
#include "VulkanContent.h"
#include "Components/Transform.h"
#include "Components/Entity.h"
#include "Shaders/ShaderTypes.h"

#include "VulkanData.h"
#include "VulkanLight.h"
#include "VulkanCompute.h"
#include <fstream>
#include <filesystem>
#include <exception>

namespace primal::graphics::vulkan
{
namespace
{
    bool load_kms_file(const char* file, utl::vector<geometry_config>& out_geometry_array)
    {
        //std::unique_ptr<std::ifstream, std::function<void(std::ifstream*)>> infile(new std::ifstream{ file, std::ios::binary }, [](std::ifstream *fptr) {fptr->close(); delete fptr; });

        std::ifstream infile(file, std::ios::in | std::ios::binary);
        // infile.open(file, std::ios::in | std::ios::binary);

        if (!infile.is_open()) 
        {
            //OutputDebugStringA(std::strerror(errno));
            //OutputDebugStringA("\n");
            return false;
        }

        u64 address = 0;

        infile.seekg(address, std::ios::beg);
        u32 geometry_count;
        infile.read(reinterpret_cast<char*>(&geometry_count), sizeof(geometry_count)); address += sizeof(u32);
        infile.seekg(address, std::ios::beg);

        // Each geometry
        for (u32 i{ 0 }; i < geometry_count; ++i)
        {
            geometry_config g{};

            // Vertices (size/count/array)
            infile.read(reinterpret_cast<char*>(&g.vertex_size), sizeof(u32)); address += sizeof(u32);
            infile.seekg(address, std::ios::beg);
            infile.read(reinterpret_cast<char*>(&g.vertex_count), sizeof(u32)); address += sizeof(u32);
            infile.seekg(address, std::ios::beg);
            //g.vertices.resize(g.vertex_count);
            for (u32 x{ 0 }; x < g.vertex_count; ++x)
            {
                Vertex vertex;
                infile.read(reinterpret_cast<char*>(&vertex), sizeof(Vertex)); address += sizeof(Vertex);
                infile.seekg(address, std::ios::beg);
                g.vertices.emplace_back(vertex);
            }

            // Indices (size/count/array)
            infile.read(reinterpret_cast<char*>(&g.index_size), sizeof(u32)); address += sizeof(u32);
            infile.seekg(address, std::ios::beg);
            infile.read(reinterpret_cast<char*>(&g.index_count), sizeof(u32)); address += sizeof(u32);
            infile.seekg(address, std::ios::beg);
            for (u32 y{ 0 }; y < g.index_count; ++y)
            {
                u32 index{ 0 };
                infile.read(reinterpret_cast<char*>(&index), sizeof(u32)); address += sizeof(u32);
                infile.seekg(address, std::ios::beg);
                g.indices.emplace_back(index);
            }

            // Name
            u32 g_name_lenght = 0;
            infile.read(reinterpret_cast<char*>(&g_name_lenght), sizeof(u32)); address += sizeof(u32);
            infile.seekg(address, std::ios::beg);
            infile.read(reinterpret_cast<char*>(&g.name), g_name_lenght * sizeof(char)); address += g_name_lenght * sizeof(char);
            infile.seekg(address, std::ios::beg);

            // Material Name
            u32 m_name_lenght = 0;
            infile.read(reinterpret_cast<char*>(&m_name_lenght), sizeof(u32)); address += sizeof(u32);
            infile.seekg(address, std::ios::beg);
            infile.read(reinterpret_cast<char*>(&g.material_name), m_name_lenght * sizeof(char)); address += m_name_lenght * sizeof(char);
            infile.seekg(address, std::ios::beg);
            
            // Ambient Map Name
            u32 ambient_map_lenght = 0;
            infile.read(reinterpret_cast<char*>(&ambient_map_lenght), sizeof(u32)); address += sizeof(u32);
            infile.seekg(address, std::ios::beg);
            infile.read(reinterpret_cast<char*>(&g.amibent_map), ambient_map_lenght * sizeof(char)); address += ambient_map_lenght * sizeof(char);
            infile.seekg(address, std::ios::beg);

            // Diffuse Map Name
            u32 diffuse_map_lenght = 0;
            infile.read(reinterpret_cast<char*>(&diffuse_map_lenght), sizeof(u32)); address += sizeof(u32);
            infile.seekg(address, std::ios::beg);
            infile.read(reinterpret_cast<char*>(&g.diffuse_map), diffuse_map_lenght * sizeof(char)); address += diffuse_map_lenght * sizeof(char);
            infile.seekg(address, std::ios::beg);

            // Specular Map Name
            u32 specular_map_lenght = 0;
            infile.read(reinterpret_cast<char*>(&specular_map_lenght), sizeof(u32)); address += sizeof(u32);
            infile.seekg(address, std::ios::beg);
            infile.read(reinterpret_cast<char*>(&g.specular_map), specular_map_lenght * sizeof(char)); address += specular_map_lenght * sizeof(char);
            infile.seekg(address, std::ios::beg);

            // Alpha Map Name
            u32 alpha_map_lenght = 0;
            infile.read(reinterpret_cast<char*>(&alpha_map_lenght), sizeof(u32)); address += sizeof(u32);
            infile.seekg(address, std::ios::beg);
            infile.read(reinterpret_cast<char*>(&g.alpha_map), alpha_map_lenght * sizeof(char)); address += alpha_map_lenght * sizeof(char);
            infile.seekg(address, std::ios::beg);

            // Normal Map Name
            u32 normal_map_lenght = 0;
            infile.read(reinterpret_cast<char*>(&normal_map_lenght), sizeof(u32)); address += sizeof(u32);
            infile.seekg(address, std::ios::beg);
            infile.read(reinterpret_cast<char*>(&g.normal_map), normal_map_lenght * sizeof(char)); address += normal_map_lenght * sizeof(char);
            infile.seekg(address, std::ios::beg);

            // Center
            infile.read(reinterpret_cast<char*>(&g.center), sizeof(math::v3)); address += sizeof(math::v3);
            infile.seekg(address, std::ios::beg);

            // Extents (min/max)
            infile.read(reinterpret_cast<char*>(&g.min_extents), sizeof(math::v3)); address += sizeof(math::v3);
            infile.seekg(address, std::ios::beg);
            infile.read(reinterpret_cast<char*>(&g.max_extents), sizeof(math::v3)); address += sizeof(math::v3);
            infile.seekg(address, std::ios::beg);

            out_geometry_array.emplace_back(g);
        }

        infile.clear();
        infile.close();

        return true;
    }

VkSurfaceFormatKHR
choose_best_surface_format(const utl::vector<VkSurfaceFormatKHR>& formats)
{
    // VK_FORMAT_UNDEFINED means all formats are available
    if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED)
        return { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };

    // If not all formats are available, choose the best available one
    for (const auto& format : formats)
    {
        // check for RGBA (first) or BGRA (second)
        if ((format.format == VK_FORMAT_R8G8B8A8_UNORM || format.format == VK_FORMAT_B8G8R8A8_UNORM) && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return format;
    }

    // If optimal format is not found, just return the first available one
    return formats[0];
}

VkPresentModeKHR
choose_best_presentation_mode(const utl::vector<VkPresentModeKHR> modes)
{
    // NOTE: VK_PRESENT_MODE_MAILBOX_KHR is preferred, as it always uses the next newest image available, and gets rid of any that are older.
    //		 This results in optimal response time, with no tearing. VK_PRESENT_MODE_FIFO_KHR is the backup, as Vulkan spec requires it to be
    //		 available. It also does not produce tearing, but my be somewhat less responsive as each image is displayed no matter how old it is.
    for (const auto& mode : modes)
    {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
            return mode;
    }

    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D
choose_swap_extent(const VkSurfaceCapabilitiesKHR& capabilities, u32 width, u32 height)
{
    // NOTE: If the currentExtent is at it's numeric limits, extent can vary in size from the window.
    //		 Otherwise, it is the size of the window.
    if (capabilities.currentExtent.width != std::numeric_limits<u32>::max())
    {
        return capabilities.currentExtent;
    }
    else // if currentExtent can vary, set it manually
    {
        VkExtent2D extent{};
        extent.width = (u32)width;
        extent.height = (u32)height;

        // Clamp extent to be within bounderies of surface min and max
        extent.width = std::max(capabilities.minImageExtent.width, std::min(capabilities.maxImageExtent.width, extent.width));
        extent.height = std::max(capabilities.minImageExtent.height, std::min(capabilities.maxImageExtent.height, extent.height));

        return extent;
    }
}

VkImageView
create_image_view(VkImage image, VkFormat format, VkImageAspectFlags flags)
{
    VkImageViewCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    // NOTE: Subresources allow the view to view only a part of the image
    info.subresourceRange.aspectMask = flags;				// which part of image ot view
    info.subresourceRange.baseMipLevel = 0;					// start mipmap level to view from
    info.subresourceRange.levelCount = 1;					// number of mipmap levels to view
    info.subresourceRange.baseArrayLayer = 0;				// start array level to view from
    info.subresourceRange.layerCount = 1;					// number of array levels to view

    VkImageView image_view{};
    VkResult result{ VK_SUCCESS };
    VkCall(result = vkCreateImageView(core::logical_device(), &info, nullptr, &image_view), "Failed to create Image View...");

    return image_view;
}
} // anonymous namespace

void
vulkan_surface::create(VkInstance instance)
{
    create_surface(instance);
    core::create_device(_surface);

    GET_DEVICE_PROC_ADDR(core::logical_device(), CreateSwapchainKHR);
    GET_DEVICE_PROC_ADDR(core::logical_device(), DestroySwapchainKHR);
    GET_DEVICE_PROC_ADDR(core::logical_device(), GetSwapchainImagesKHR);
    GET_DEVICE_PROC_ADDR(core::logical_device(), AcquireNextImageKHR);
    GET_DEVICE_PROC_ADDR(core::logical_device(), QueuePresentKHR);

    create_swapchain();
    create_render_pass();
    recreate_framebuffers();

    data::initialize();

    _geometry.setSize(this->width(), this->height());
    _geometry.setupPoolAndLayout();
    _geometry.setupRenderpassAndFramebuffer();
    _geometry.create_semaphore();
    _final.setSize(this->width(), this->height());

    core::create_graphics_command((u32)_swapchain.images.size());

    light::initialize();

    const std::string base_dir{ SOLUTION_DIR };

    std::string sponza_package{ base_dir };
    sponza_package.append("EngineTest\\assets\\kms\\sponza\\");
    for (const auto& file : std::filesystem::directory_iterator(sponza_package))
    {
        //load_kms_file("C:\\Users\\zy\\Desktop\\PrimalMerge\\PrimalEngine\\EngineTest\\assets\\kms\\sponza\\sponza_247_sponza_247_column_b.kms", model_2);
        if (file.path().has_extension())
        {
            utl::vector<geometry_config> model_2;
            load_kms_file(file.path().string().c_str(), model_2);
            void* model_2_data = &model_2;
            auto sponza_sub_id = submesh::add(model_2_data);
            u32 diffuse_map_id{ id::invalid_id };
            u32 specular_map_id{ id::invalid_id };
            u32 normal_map_id{ id::invalid_id };
            if (model_2[0].diffuse_map != nullptr && model_2[0].diffuse_map[0] != '\0')
            {
                diffuse_map_id = textures::add(base_dir + std::string{"EngineTest\\assets\\"} + std::string{ model_2[0].diffuse_map });
            }
            if (model_2[0].specular_map != nullptr && model_2[0].specular_map[0] != '\0')
            {
                specular_map_id = textures::add(base_dir + std::string{"EngineTest\\assets\\"} + std::string{ model_2[0].specular_map });
            }
            if (model_2[0].normal_map != nullptr && model_2[0].normal_map[0] != '\0')
            {
                normal_map_id = textures::add(base_dir + std::string{"EngineTest\\assets\\"} + std::string{ model_2[0].normal_map });
            }
            auto sponza_vs_id = shaders::add(base_dir + std::string({ "Engine\\Graphics\\Vulkan\\Shaders\\spv\\" }) + std::string({ model_2[0].material_name }) + std::string({ ".vert.spv" }), shader_type::vertex);
            auto sponza_frag_id = shaders::add(base_dir + std::string({ "Engine\\Graphics\\Vulkan\\Shaders\\spv\\" }) + std::string({ model_2[0].material_name }) + std::string({ ".frag.spv" }), shader_type::pixel);
            auto sponza_material_id = materials::add({ material_type::opauqe, 0, {sponza_vs_id, id::invalid_id, id::invalid_id, id::invalid_id, sponza_frag_id, id::invalid_id, id::invalid_id, id::invalid_id}, nullptr });
            if (id::is_valid(diffuse_map_id))    materials::get_material(sponza_material_id).add_texture(diffuse_map_id);
            if (id::is_valid(specular_map_id))    materials::get_material(sponza_material_id).add_texture(specular_map_id);
            if (id::is_valid(normal_map_id))    materials::get_material(sponza_material_id).add_texture(normal_map_id);

            transform::init_info sponza_transform_info{};
            math::v3 sponza_rotation{ 0.f, 0.f, 0.f };
            math::v3 sponza_position{ 0.f, 0.f, 0.f };
            math::v3 sponza_scale{ 0.01f, 0.01f, 0.01f };
            DirectX::XMVECTOR sponza_quat{ DirectX::XMQuaternionRotationRollPitchYawFromVector(DirectX::XMLoadFloat3(&sponza_rotation)) };
            math::v4a sponza_rot_quat;
            DirectX::XMStoreFloat4A(&sponza_rot_quat, sponza_quat);
            memcpy(&sponza_transform_info.scale[0], &sponza_scale.x, sizeof(sponza_transform_info.scale));
            memcpy(&sponza_transform_info.rotation[0], &sponza_rot_quat.x, sizeof(sponza_transform_info.rotation));
            memcpy(&sponza_transform_info.position[0], &sponza_position.x, sizeof(sponza_transform_info.position));
            game_entity::entity_info sponza_entity_info{};
            sponza_entity_info.transform = &sponza_transform_info;
            game_entity::entity sponza_ntt{ game_entity::create(sponza_entity_info) };

            auto sponza_sub_instance_id = _scene.add_model_instance(sponza_ntt, sponza_sub_id);
            _scene.add_material(sponza_sub_instance_id, sponza_material_id, false);
            model_2.clear();
        }
        else
        {
            continue;
        }
    }

    std::string sphere_package{ base_dir };
    sphere_package.append("EngineTest\\assets\\kms\\sphere\\");
    for (const auto& file : std::filesystem::directory_iterator(sphere_package))
    {
        //load_kms_file("C:\\Users\\zy\\Desktop\\PrimalMerge\\PrimalEngine\\EngineTest\\assets\\kms\\sponza\\sponza_247_sponza_247_column_b.kms", model_2);
        if (file.path().has_extension())
        {
            utl::vector<geometry_config> model_2;
            load_kms_file(file.path().string().c_str(), model_2);
            void* model_2_data = &model_2;
            auto sphere_sub_id = submesh::add(model_2_data);
            auto sphere_vs_id = shaders::add(base_dir + std::string({ "Engine\\Graphics\\Vulkan\\Shaders\\spv\\sphere.vert.spv" }), shader_type::vertex);
            auto sphere_frag_id = shaders::add(base_dir + std::string({ "Engine\\Graphics\\Vulkan\\Shaders\\spv\\sphere.frag.spv" }), shader_type::pixel);
            auto sphere_material_id = materials::add({ material_type::opauqe, 0, {sphere_vs_id, id::invalid_id, id::invalid_id, id::invalid_id, sphere_frag_id, id::invalid_id, id::invalid_id, id::invalid_id}, nullptr });

            transform::init_info sphere_transform_info{};
            math::v3 sphere_rotation{ 0.f, 0.f, 0.f };
            math::v3 sphere_position{ 0.f, 0.5f, 0.f };
            math::v3 sphere_scale{ 0.3f, 0.3f, 0.3f };
            DirectX::XMVECTOR sphere_quat{ DirectX::XMQuaternionRotationRollPitchYawFromVector(DirectX::XMLoadFloat3(&sphere_rotation)) };
            math::v4a sphere_rot_quat;
            DirectX::XMStoreFloat4A(&sphere_rot_quat, sphere_quat);
            memcpy(&sphere_transform_info.scale[0], &sphere_scale.x, sizeof(sphere_transform_info.scale));
            memcpy(&sphere_transform_info.rotation[0], &sphere_rot_quat.x, sizeof(sphere_transform_info.rotation));
            memcpy(&sphere_transform_info.position[0], &sphere_position.x, sizeof(sphere_transform_info.position));
            game_entity::entity_info sphere_entity_info{};
            sphere_entity_info.transform = &sphere_transform_info;
            game_entity::entity sphere_ntt{ game_entity::create(sphere_entity_info) };

            auto sphere_sub_instance_id = _scene.add_model_instance(sphere_ntt, sphere_sub_id);
            _scene.add_material(sphere_sub_instance_id, sphere_material_id, true);
            model_2.clear();
        }
        else
        {
            continue;
        }
    }

    _scene.createUniformBuffer();
    compute::initialize();


    _scene.createPipeline(data::get_data<VkRenderPass>(_geometry.getRenderpass()), data::get_data<VkPipelineLayout>(_geometry.getPipelineLayout()));
    
    
    _scene.createDescriptorSets(data::get_data<VkDescriptorPool>(_geometry.getDescriptorPool()), data::get_data<VkDescriptorSetLayout>(_geometry.getDescriptorSetLayout()));
    _final.setupDescriptorSets(_geometry.getTexture(), _scene.getUboID());
    _final.setupPipeline(_renderpass);
}

void
vulkan_surface::present(VkSemaphore image_available, VkSemaphore render_finished, VkFence fence, VkQueue presentation_queue)
{
    // Present image
    VkPresentInfoKHR info{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &render_finished;
    info.swapchainCount = 1;
    info.pSwapchains = &_swapchain.swapchain;
    info.pImageIndices = &_image_index;
    VkResult result{ VK_SUCCESS };
    result = vkQueuePresentKHR(presentation_queue, &info);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR /*|| _framebuffer_resized*/)
    {
        _framebuffer_resized = false;
        recreate_swapchain();
    }
    else if (result != VK_SUCCESS)
    {
        ERROR_MSSG("Failed to present swapchain...");
    }

    _frame_index = (_frame_index + 1) % _swapchain.images.size();
}

void
vulkan_surface::resize()
{
    _framebuffer_resized = true;
}

void
vulkan_surface::release()
{

    vkDeviceWaitIdle(core::logical_device());

    compute::shutdown();

    for (u32 i{ 0 }; i < _swapchain.images.size(); ++i)
    {
        destroy_framebuffer(core::logical_device(), _framebuffers[i]);
    }
    renderpass::destroy_renderpass(core::logical_device(), _renderpass);
    clean_swapchain();

    /// <summary>
    /// Own function destroy
    /// </summary>
    _scene.~vulkan_scene();

    vkDestroySurfaceKHR(core::get_instance(), _surface, nullptr);

    data::shutdown();
}

void
vulkan_surface::create_surface(VkInstance instance)
{
#ifdef _WIN32
    VkWin32SurfaceCreateInfoKHR create_info{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    create_info.hinstance = GetModuleHandle(NULL);
    create_info.hwnd = (HWND)_window.handle();

    VkCall(vkCreateWin32SurfaceKHR(instance, &create_info, nullptr, &_surface), "Failed to create a surface...");
#elif __linux__
    VkXlibSurfaceCreateInfoKHR create_info{ VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR };
    create_info.dpy = (Display*)_window.display();
    create_info.window = *(Window*)_window.handle();

    VkCall(vkCreateXlibSurfaceKHR(instance, &create_info, nullptr, &_surface), "Failed to create a surface...");
#endif // _WIN32
}

bool
vulkan_surface::create_swapchain()
{
    // We pick the best settings for the swapchain based on the swapchain details from the physical device
    _swapchain.details = get_swapchain_details(core::physical_device(), _surface);

    // Find optimal surface values for the swapchain
    VkSurfaceFormatKHR format{ choose_best_surface_format(_swapchain.details.formats) };
    VkPresentModeKHR mode{ choose_best_presentation_mode(_swapchain.details.presentation_modes) };
    VkExtent2D extent{ choose_swap_extent(_swapchain.details.surface_capabilities, _window.width(), _window.height()) };

    u32 images_in_flight = _swapchain.details.surface_capabilities.minImageCount + 1;
    // NOTE: At this point, in a typical situation, images_in_flight will be 2, allowing us to use a triple buffer.
    //		 However, there will be a rare occasion that triple buffering is not supported. If that is the case, we
    //		 will need to set images_in_flight to the max image count supported. If maxImageCount == 0, there is no max limit.
    if (_swapchain.details.surface_capabilities.maxImageCount > 0 && _swapchain.details.surface_capabilities.maxImageCount < images_in_flight)
        images_in_flight = _swapchain.details.surface_capabilities.maxImageCount;

    VkSwapchainCreateInfoKHR info{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    info.surface = _surface;
    info.imageFormat = format.format;
    info.imageColorSpace = format.colorSpace;
    info.presentMode = mode;
    info.imageExtent = extent;
    info.minImageCount = images_in_flight;
    info.imageArrayLayers = 1;														// number of layers for each image in swapchain
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;							// what attachment each image will be used as
    info.preTransform = _swapchain.details.surface_capabilities.currentTransform;	// transform to perform on swapchain images
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;						// how to handle blending images with external graphics (such as other windows, OS UI, etc...)
    info.clipped = VK_TRUE;															// Clip parts of images not in view? (i.e. off screen, behind another window, etc...)

    // If graphics and presentation queue family indices are different, the swapchain must let images be shared between families
    if (core::graphics_family_queue_index() != core::presentation_family_queue_index())
    {
        u32 queue_family_indices[]{ (u32)core::graphics_family_queue_index(),
                                    (u32)core::presentation_family_queue_index() };

        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;							// number of queues to share images between
        info.pQueueFamilyIndices = queue_family_indices;			// array pf queues tp share between
    }
    else
    {
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.queueFamilyIndexCount = 0;
        info.pQueueFamilyIndices = nullptr;
    }

    // NOTE: If old shwapchain has been destroyed and this one is replacing it, you can use this to
    //		 link the old one to quickly hand over responsibilities. Not using this feature for now.
    info.oldSwapchain = VK_NULL_HANDLE;

    // Check to make sure given surface is supported by device for each VkQueue
    // TODO: extend when using compute && transfer queues
    VkBool32 supported{ VK_FALSE };
    vkGetPhysicalDeviceSurfaceSupportKHR(core::physical_device(), core::graphics_family_queue_index(), _surface, &supported);
    if (supported == VK_FALSE)
    {
        ERROR_MSSG("Physical device doesn't support graphics family queue for this surface...");
        return false;
    }
    vkGetPhysicalDeviceSurfaceSupportKHR(core::physical_device(), core::presentation_family_queue_index(), _surface, &supported);
    if (supported == VK_FALSE)
    {
        ERROR_MSSG("Physical device doesn't support presentation family queue for this surface...");
        return false;
    }
    vkGetPhysicalDeviceSurfaceSupportKHR(core::physical_device(), core::compute_family_queue_index(), _surface, &supported);
    if (supported == VK_FALSE)
    {
        ERROR_MSSG("Physical device doesn't support compute family queue for this surface...");
        return false;
    }
    vkGetPhysicalDeviceSurfaceSupportKHR(core::physical_device(), core::transfer_family_queue_index(), _surface, &supported);
    if (supported == VK_FALSE)
    {
        ERROR_MSSG("Physical device doesn't support transfer family queue for this surface...");
        return false;
    }

    VkResult result{ VK_SUCCESS };
    VkCall(result = vkCreateSwapchainKHR(core::logical_device(), &info, nullptr, &_swapchain.swapchain), "Failed to create Swapchain...");
    if (result != VK_SUCCESS) return false;

    _swapchain.image_format = format.format;
    _swapchain.extent = extent;

    // Get swap chain images and image views
    u32 image_count{ 0 };
    vkGetSwapchainImagesKHR(core::logical_device(), _swapchain.swapchain, &image_count, nullptr);
    utl::vector<VkImage> images(image_count);
    vkGetSwapchainImagesKHR(core::logical_device(), _swapchain.swapchain, &image_count, images.data());

    for (const auto& image : images)
    {
        swapchain_image temp{};
        temp.image = image;
        temp.image_view = create_image_view(image, _swapchain.image_format, VK_IMAGE_ASPECT_COLOR_BIT);
        _swapchain.images.push_back(temp);
    }

    if (!core::detect_depth_format(core::physical_device()))
    {
        ERROR_MSSG("Failed to find a supported depth format...");
        return false;
    }

    if (!core::detect_push_descriptor(core::physical_device()))
    {
        ERROR_MSSG("Failed to use push descriptor set feature...");
        return false;
    }

    // Create depthbuffer image and view
    image_init_info image_info;
    image_info.image_type = VK_IMAGE_TYPE_2D;
    image_info.width = _swapchain.extent.width;
    image_info.height = _swapchain.extent.height;
    image_info.format = core::depth_format();
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage_flags = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    image_info.memory_flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    image_info.create_view = true;
    image_info.view_aspect_flags = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (!create_image(core::logical_device(), &image_info, _swapchain.depth_attachment))
        return false;


    MESSAGE("Swapchain created successfully");

    return true;
}

void
vulkan_surface::create_render_pass()
{
    _renderpass = renderpass::create_renderpass(core::logical_device(), _swapchain.image_format, core::depth_format(),
        { 0, 0, _window.width(), _window.height() }, { 0.0f, 0.0f, 0.0f, 0.0f }, 1.0f, 0);

}

bool
vulkan_surface::recreate_swapchain()
{
    vkDeviceWaitIdle(core::logical_device());

    _is_recreating = true;

    clean_swapchain();
    _swapchain.images.clear();
    if (!create_swapchain()) return false;
    if (!recreate_framebuffers()) return false;

    _is_recreating = false;
    _framebuffer_resized = false;

    return true;
}

bool
vulkan_surface::recreate_framebuffers()
{
    _framebuffers.resize(_swapchain.images.size());

    for (u32 i{ 0 }; i < _swapchain.images.size(); ++i)
    {
        destroy_framebuffer(core::logical_device(), _framebuffers[i]);
    }

    for (u32 i{ 0 }; i < _swapchain.images.size(); ++i)
    {
        u32 attach_count = 2;
        utl::vector<VkImageView> attachments{};
        attachments.resize(attach_count);
        attachments[0] = _swapchain.images[i].image_view;
        attachments[1] = _swapchain.depth_attachment.view;

        if (!create_framebuffer(core::logical_device(), _renderpass, _window.width(), _window.height(), attach_count, attachments, _framebuffers[i])) return false;
    }

    return true;
}

void
vulkan_surface::clean_swapchain()
{
    destroy_image(core::logical_device(), &_swapchain.depth_attachment);

    // NOTE: Swapchain provides images for us, but not the views. Therefore, we are responsible
    //		 for destroying only the views... the swapchain destroys images for us.
    for (u32 i{ 0 }; i < _swapchain.images.size(); ++i)
    {
        vkDestroyImageView(core::logical_device(), _swapchain.images[i].image_view, nullptr);
    }

    vkDestroySwapchainKHR(core::logical_device(), _swapchain.swapchain, nullptr);
}

bool
vulkan_surface::next_image_index(VkSemaphore image_available, VkFence fence, u64 timeout)
{
    // Get next image
    VkResult result{ VK_SUCCESS };
    result = vkAcquireNextImageKHR(core::logical_device(), _swapchain.swapchain, timeout, image_available, fence, &_image_index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        recreate_swapchain();
        return false;
    }
    else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        ERROR_MSSG("Failed to aquire swapchain image...");
    }

    return true;
}

swapchain_details
get_swapchain_details(VkPhysicalDevice device, VkSurfaceKHR surface)
{
    swapchain_details details{};

    // -- Capabilities --
    // Get the surface capabilities for the given surface on the given physical device
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.surface_capabilities);

    // -- Formats --
    u32 format_count{ 0 };
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &format_count, nullptr);
    // If formats returned, get list of formats
    if (format_count != 0)
    {
        details.formats.resize(format_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &format_count, details.formats.data());
    }

    // -- Presentation Modes --
    u32 presentation_count{ 0 };
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentation_count, nullptr);
    // If presentation modes returned, get list of presentation modes
    if (presentation_count != 0)
    {
        details.presentation_modes.resize(presentation_count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentation_count, details.presentation_modes.data());
    }

    return details;
}

}