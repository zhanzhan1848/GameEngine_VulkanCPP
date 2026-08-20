/**
 * @file TestMultiView.cpp
 * @brief Integration test for Multi-View Rendering.
 * 
 * This test case demonstrates:
 * 1. Multi-View Rendering (rendering to a CubeMap).
 * 2. Mirror Box integration using Reflection Plane.
 * 3. Screen Space Reflection (SSR) integration.
 * 4. Composite Pass blending Main View, SSR, and Debug Overlay.
 */
#include "TestMultiView.h"
#include "Engine/Graphics/RHI/Core/RHICommand.h"
#include "Engine/Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Engine/Graphics/RenderGraph/RenderGraphDefinitions.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RenderMesh.h"
#include "Engine/Graphics/RenderScene.h"
#include "Engine/Graphics/RHI/Core/RHIMath.h"
#include "Engine/Platform/Platform.h" // For create_window
#include "Engine/Utilities/SphericalHarmonics.h"
#include "Engine/Input/Input.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"  // third_party/stb submodule

#include <cmath>
#include <vector>
#include <set>
#include <fstream>
#include <sstream>

using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace primal::graphics::rendergraph;
using namespace primal::graphics::rhi::math;

void MultiViewTestCase::ProjectCubemapToSH(unsigned char* pixels[6], int width, int height) {
    // Initialize coefficients
    for(int i=0; i<9; ++i) computedSH[i] = {0,0,0,0};
    
    float invWidth = 1.0f / width;
    float invHeight = 1.0f / height;
    
    // Normalization factor for SH basis
    // We will sum (Radiance * Basis * dOmega)
    // Then normalization is done by 1 (if dOmega sums to 4Pi) or 4Pi/TotalWeight
    
    float totalWeight = 0.0f;
    
    for (int face = 0; face < 6; ++face) {
        for (int y = 0; y < height; ++y) {
            // Map y to [-1, 1]
            // Note: Standard Cubemap UV:
            // +X (Right): u=z, v=y (flipped?) -> Check stbi load order
            // Assuming standard OpenGL/Vulkan cubemap faces:
            // +X, -X, +Y, -Y, +Z, -Z
            // Center of pixel
            float v = (y + 0.5f) * invHeight * 2.0f - 1.0f; 
            
            for (int x = 0; x < width; ++x) {
                float u = (x + 0.5f) * invWidth * 2.0f - 1.0f;
                
                float xDir, yDir, zDir;
                // Calculate direction based on face
                // +Y is Up, -Y is Down. +Z is Front?
                // Vulkan Cubemap:
                // +X (Right), -X (Left), +Y (Up), -Y (Down), +Z (Front), -Z (Back)
                // Wait, Vulkan +Y is down in screen space, but for Cubemaps?
                // Usually:
                // +X: Z points left?
                // Let's stick to standard vector construction
                switch(face) {
                    case 0: // +X
                        xDir = 1.0f; yDir = -v; zDir = -u; break;
                    case 1: // -X
                        xDir = -1.0f; yDir = -v; zDir = u; break;
                    case 2: // +Y
                        xDir = u; yDir = 1.0f; zDir = v; break;
                    case 3: // -Y
                        xDir = u; yDir = -1.0f; zDir = -v; break;
                    case 4: // +Z
                        xDir = u; yDir = -v; zDir = 1.0f; break;
                    case 5: // -Z
                        xDir = -u; yDir = -v; zDir = -1.0f; break;
                }
                
                float lenSq = xDir*xDir + yDir*yDir + zDir*zDir;
                float len = std::sqrt(lenSq);
                
                float dirX = xDir / len;
                float dirY = yDir / len;
                float dirZ = zDir / len;
                
                // Solid Angle approximation
                // dOmega = 4 / ((1 + u^2 + v^2)^1.5)
                // Note: u, v here are essentially tangent plane coords [-1, 1]
                float dOmega = 4.0f / std::pow(1.0f + u*u + v*v, 1.5f);
                
                // Sample Color (Linearize)
                int idx = (y * width + x) * 4;
                float r = std::pow(pixels[face][idx] / 255.0f, 2.2f);
                float g = std::pow(pixels[face][idx+1] / 255.0f, 2.2f);
                float b = std::pow(pixels[face][idx+2] / 255.0f, 2.2f);
                
                // SH Basis Functions (Real SH)
                // l=0
                float Y00 = 0.282095f;
                // l=1
                float Y1_1 = 0.488603f * dirY;
                float Y10  = 0.488603f * dirZ;
                float Y11  = 0.488603f * dirX;
                // l=2
                float Y2_2 = 1.092548f * dirX * dirY;
                float Y2_1 = 1.092548f * dirY * dirZ;
                float Y20  = 0.315392f * (3.0f * dirZ * dirZ - 1.0f);
                float Y21  = 1.092548f * dirX * dirZ;
                float Y22  = 0.546274f * (dirX * dirX - dirY * dirY);
                
                float bases[9] = {Y00, Y1_1, Y10, Y11, Y2_2, Y2_1, Y20, Y21, Y22};
                
                for(int i=0; i<9; ++i) {
                    computedSH[i].x += r * bases[i] * dOmega;
                    computedSH[i].y += g * bases[i] * dOmega;
                    computedSH[i].z += b * bases[i] * dOmega;
                }
                
                totalWeight += dOmega;
            }
        }
    }
    
    // Normalize
    // The integral over sphere is 4Pi.
    // Our weighted sum approximates Integral(L * Y * dOmega)
    // The sum of dOmega should be 4Pi.
    // So we just need to ensure scale is correct.
    // If sum(dOmega) is not exactly 4Pi due to discretization, we can scale by (4Pi / totalWeight).
    float normFactor = (4.0f * 3.14159265359f) / totalWeight;
    
    // Scale down by 1/PI to convert from Irradiance to Diffuse Color (assuming Shader multiplies by Albedo)
    // E = Pi * L. If we want the final pixel color to match the texture color (approx),
    // we should divide by Pi because the shader calculates Irradiance E.
    // Standard Diffuse: Color = (E * Albedo) / Pi.
    // Our Shader: Color = E * Albedo.
    // So we pre-multiply coefficients by 1/Pi.
    // 
    // Additionally, we apply an Exposure Control factor.
    // The Skybox is likely very bright (L ~ 1.0).
    // Plus we have a Point Light in the scene.
    // Without Tone Mapping, (Ambient + Diffuse) > 1.0 leads to clipping (pure white).
    // We scale SH down to allow lighting variation to be visible.
    float exposure = 0.3f; 
    float userScale = (1.0f / 3.14159265359f) * exposure;
    
    std::cout << "ProjectCubemapToSH: TotalWeight=" << totalWeight << " NormFactor=" << normFactor << " UserScale=" << userScale << std::endl;

    for(int i=0; i<9; ++i) {
        computedSH[i].x *= (normFactor * userScale);
        computedSH[i].y *= (normFactor * userScale);
        computedSH[i].z *= (normFactor * userScale);
        computedSH[i].w = 1.0f;
    }
    
    std::cout << "Computed SH Coefficients from Cubemap:" << std::endl;
    for(int i=0; i<9; ++i) {
        std::cout << "SH[" << i << "]: " << computedSH[i].x << ", " << computedSH[i].y << ", " << computedSH[i].z << std::endl;
    }
}

primal::graphics::rhi::ResourceHandle MultiViewTestCase::LoadCubemap(const std::vector<std::string>& filenames) {
    if (filenames.size() != 6) return primal::graphics::rhi::handles::INVALID_RESOURCE;

    int width, height, channels;
    stbi_uc* pixels[6];
    
    for (int i = 0; i < 6; ++i) {
        pixels[i] = stbi_load(filenames[i].c_str(), &width, &height, &channels, 4); // Force RGBA
        if (!pixels[i]) {
            std::cout << "Failed to load cubemap face: " << filenames[i] << std::endl;
            // Cleanup previous
            for(int j=0; j<i; ++j) stbi_image_free(pixels[j]);
            return primal::graphics::rhi::handles::INVALID_RESOURCE;
        }
    }
    
    // Compute SH Coefficients before upload/free
    ProjectCubemapToSH(pixels, width, height);

    // Create Texture
    primal::graphics::rhi::TextureDesc desc{};
    desc.type = primal::graphics::rhi::TextureType::TextureCube;
    desc.size = {(uint32_t)width, (uint32_t)height, 1};
    desc.mipLevels = 1;
    desc.arraySize = 1; // For TextureCube, arraySize is 1 (it has 6 faces implicit)
    desc.format = primal::graphics::rhi::DataFormat::RGBA8_UNorm;
    desc.usage = primal::graphics::rhi::TextureUsage::ShaderResource | primal::graphics::rhi::TextureUsage::CopyDest;

    primal::graphics::rhi::ResourceHandle texture = device->CreateTexture(desc);

    // Upload Data
    // For simplicity, we create a staging buffer for all faces or one by one.
    // Let's do one large buffer.
    uint64_t faceSize = width * height * 4;
    uint64_t totalSize = faceSize * 6;

    primal::graphics::rhi::BufferDesc stageDesc{};
    stageDesc.size = totalSize;
    stageDesc.usage = primal::graphics::rhi::GPUMemoryUsage::Staging; // Correct usage for CPU->GPU upload
    stageDesc.bindFlags = (uint32_t)primal::graphics::rhi::BufferUsageFlags::TransferSrc; // Correct binding flag
    
    primal::graphics::rhi::ResourceHandle stagingBuffer = device->CreateBuffer(stageDesc);
    
    void* data = device->MapBuffer(stagingBuffer);
    if (data) {
        uint8_t* dst = static_cast<uint8_t*>(data);
        for (int i = 0; i < 6; ++i) {
            memcpy(dst + i * faceSize, pixels[i], faceSize);
        }
        device->UnmapBuffer(stagingBuffer);
    }

    // Copy Command
    primal::graphics::rhi::CommandBufferHandle cmdHandle = device->CreateCommandBuffer(primal::graphics::rhi::CommandQueueType::Graphics);
    primal::graphics::rhi::RHICommandBuffer* cmd = primal::graphics::rhi::GetCommandBuffer(cmdHandle);
    
    if (cmd && cmd->Begin()) {
        std::vector<primal::graphics::rhi::BufferTextureCopyRegion> regions;
        for (int i = 0; i < 6; ++i) {
            primal::graphics::rhi::BufferTextureCopyRegion region{};
            region.bufferOffset = i * faceSize;
            region.imageSubresource.mipLevel = 0;
            region.imageSubresource.baseArrayLayer = i;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {(uint32_t)width, (uint32_t)height, 1};
            regions.push_back(region);
        }
        
        cmd->CopyBufferToTexture(stagingBuffer, texture, regions.data(), 6);
        
        // Transition to Shader Read
        primal::graphics::rhi::ResourceBarrier barrier{};
        barrier.resource = texture;
        barrier.beforeState = primal::graphics::rhi::ResourceState::CopyDest;
        barrier.afterState = primal::graphics::rhi::ResourceState::ShaderResource;
        barrier.subresource = primal::graphics::rhi::RHI_ALL_SUBRESOURCES;
        
        cmd->InsertBarrier(&barrier, 1);
        
        cmd->End();
        cmd->Submit();
        cmd->WaitForCompletion();
    }
    
    device->DestroyCommandBuffer(cmdHandle);
    device->DestroyBuffer(stagingBuffer);

    for (int i = 0; i < 6; ++i) stbi_image_free(pixels[i]);

    return texture;
}

    // Halton Sequence for TAA Jitter
    float Halton(int index, int base) {
        float f = 1.0f;
        float r = 0.0f;
        while (index > 0) {
            f /= (float)base;
            r += f * (index % base);
            index /= base;
        }
        return r;
    }

    // Scene Data Structure (must match Shader)
    struct SceneData {
        m4x4 model;
        v4 lightPos;
        v4 lightColor;
        v4 reflectionPlane;
        v4 reflectionPlane2;
        v4 reflectionPlane3;
        m4x4 previousModel; // Added for Motion Vectors
        v2 jitter; // Added for TAA
        v2 previousJitter; // Added for TAA
        v4 shCoeffs[9]; // Added SH Coefficients
    };
    
    // Global Descriptor Set for Present Pass (to avoid conflict with Debug Overlay)
    static DescriptorSetHandle presentDescriptorSet = handles::INVALID_DESCRIPTOR_SET;
    static ResourceHandle presentUniformBuffer = handles::INVALID_RESOURCE;


    // Helper Math Functions for Metal (Column-Major)
    m4x4 CreatePerspective(float fov, float aspect, float zNear, float zFar) {
        float tanHalfFov = tan(fov / 2.0f);
        primal::math::m4x4 result = simd_matrix(
            primal::math::v4{1.0f / (aspect * tanHalfFov), 0.0f, 0.0f, 0.0f},
            primal::math::v4{0.0f, 1.0f / tanHalfFov, 0.0f, 0.0f},
            primal::math::v4{0.0f, 0.0f, zFar / (zNear - zFar), -1.0f},
            primal::math::v4{0.0f, 0.0f, -(zFar * zNear) / (zFar - zNear), 0.0f}
        );
        return result;
    }

    m4x4 CreateRotationY(float angle) {
        float c = cos(angle);
        float s = sin(angle);
        return simd_matrix(
            primal::math::v4{c, 0.0f, -s, 0.0f},
            primal::math::v4{0.0f, 1.0f, 0.0f, 0.0f},
            primal::math::v4{s, 0.0f, c, 0.0f},
            primal::math::v4{0.0f, 0.0f, 0.0f, 1.0f}
        );
    }

    m4x4 CreateTranslation(v3 t) {
        return simd_matrix(
            primal::math::v4{1.0f, 0.0f, 0.0f, 0.0f},
            primal::math::v4{0.0f, 1.0f, 0.0f, 0.0f},
            primal::math::v4{0.0f, 0.0f, 1.0f, 0.0f},
            primal::math::v4{t.x, t.y, t.z, 1.0f}
        );
    }
    
    m4x4 MatrixMultiply(m4x4 a, m4x4 b) {
        return a * b;
    }
    
    m4x4 CreateLookAt(v3 eye, v3 center, v3 up) {
        v3 f = Normalize(center - eye);
        v3 s = Normalize(Cross(f, up));
        v3 u = Cross(s, f);
    
        m4x4 result = simd_matrix(
            v4{s.x, u.x, -f.x, 0.0f},
            v4{s.y, u.y, -f.y, 0.0f},
            v4{s.z, u.z, -f.z, 0.0f},
            v4{-Dot(s, eye), -Dot(u, eye), Dot(f, eye), 1.0f}
        );
        return result;
    }
    
    Engine_Test::Engine_Test() : RenderTestRunner(std::make_unique<MultiViewTestCase>()) {}
    
    /**
 * @brief Creates the cube mesh (Skybox/Environment) and the Mirror Box mesh.
 * 
 * Sets up vertex and index buffers for the scene geometry.
 */
void MultiViewTestCase::CreateCubeMesh() {
        // Cornell Box Geometry
        // Struct updated with Color
        struct Vertex {
            float position[3];
            float normal[3];
            float uv[2];
            float color[3]; // Add Color
        };

        std::cout << "DEBUG: sizeof(Vertex) = " << sizeof(Vertex) << std::endl;
        
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        
        // Helper to add Quad
        auto addQuad = [&](v3 p0, v3 p1, v3 p2, v3 p3, v3 n, v3 color) {
            uint32_t base = (uint32_t)vertices.size();
            
            Vertex v0, v1, v2, v3_vert;
            
            // 0
            v0.position[0] = p0.x; v0.position[1] = p0.y; v0.position[2] = p0.z;
            v0.normal[0] = n.x; v0.normal[1] = n.y; v0.normal[2] = n.z;
            v0.uv[0] = 0; v0.uv[1] = 0;
            v0.color[0] = color.x; v0.color[1] = color.y; v0.color[2] = color.z;

            // 1
            v1.position[0] = p1.x; v1.position[1] = p1.y; v1.position[2] = p1.z;
            v1.normal[0] = n.x; v1.normal[1] = n.y; v1.normal[2] = n.z;
            v1.uv[0] = 1; v1.uv[1] = 0;
            v1.color[0] = color.x; v1.color[1] = color.y; v1.color[2] = color.z;

            // 2
            v2.position[0] = p2.x; v2.position[1] = p2.y; v2.position[2] = p2.z;
            v2.normal[0] = n.x; v2.normal[1] = n.y; v2.normal[2] = n.z;
            v2.uv[0] = 1; v2.uv[1] = 1;
            v2.color[0] = color.x; v2.color[1] = color.y; v2.color[2] = color.z;

            // 3
            v3_vert.position[0] = p3.x; v3_vert.position[1] = p3.y; v3_vert.position[2] = p3.z;
            v3_vert.normal[0] = n.x; v3_vert.normal[1] = n.y; v3_vert.normal[2] = n.z;
            v3_vert.uv[0] = 0; v3_vert.uv[1] = 1;
            v3_vert.color[0] = color.x; v3_vert.color[1] = color.y; v3_vert.color[2] = color.z;

            vertices.push_back(v0);
            vertices.push_back(v1);
            vertices.push_back(v2);
            vertices.push_back(v3_vert);
            
            indices.push_back(base + 0);
            indices.push_back(base + 1);
            indices.push_back(base + 2);
            indices.push_back(base + 0);
            indices.push_back(base + 2);
            indices.push_back(base + 3);
        };
        
    // --- Part 1: Standard Cornell Box (Size 10, -5 to 5) ---
    // Cornell Box Dimensions: 0 to 555 usually, scaled to -5 to 5
    float size = 10.0f;
    float h = size * 0.5f;
    
    v3 white = {1.0f, 1.0f, 1.0f};
    v3 red   = {1.0f, 0.0f, 0.0f};
    v3 green = {0.0f, 1.0f, 0.0f};
    
    // Floor (White)
    addQuad(v3{-h, -h, -h}, v3{ h, -h, -h}, v3{ h, -h,  h}, v3{-h, -h,  h}, v3{0, 1, 0}, white);
    
    // Ceiling (White)
    addQuad(v3{-h,  h,  h}, v3{ h,  h,  h}, v3{ h,  h, -h}, v3{-h,  h, -h}, v3{0, -1, 0}, white);
    
    // Back Wall (White)
    addQuad(v3{ h, -h, -h}, v3{-h, -h, -h}, v3{-h,  h, -h}, v3{ h,  h, -h}, v3{0, 0, 1}, white);
    
    // Left Wall (Red)
    addQuad(v3{-h, -h, -h}, v3{-h, -h,  h}, v3{-h,  h,  h}, v3{-h,  h, -h}, v3{1, 0, 0}, red);
    
    // Right Wall (Green)
    addQuad(v3{ h, -h,  h}, v3{ h, -h, -h}, v3{ h,  h, -h}, v3{ h,  h,  h}, v3{-1, 0, 0}, green);
    
    // Helper to add Face (for legacy/debug shapes)
    auto addFace = [&](v3 n, v3 x, v3 y, v3 c) {
        addQuad(c - x - y, c + x - y, c + x + y, c - x + y, n, white);
    };
    
    // Helper for Boxes
    auto addBox = [&](v3 center, v3 size, float angleY, v3 color, std::string name) {
        uint32_t startIdx = (uint32_t)indices.size();
        
        // Basis
        float c = cos(angleY);
        float s = sin(angleY);
        
        v3 u = {c * size.x * 0.5f, 0.0f, -s * size.x * 0.5f};
        v3 v = {0.0f, size.y * 0.5f, 0.0f};
        v3 w = {s * size.z * 0.5f, 0.0f, c * size.z * 0.5f};
        
        // Top
        addQuad(center + v - u + w, center + v + u + w, center + v + u - w, center + v - u - w, v3{0, 1, 0}, color);
        // Front
        addQuad(center - v - u + w, center - v + u + w, center + v + u + w, center + v - u + w, v3{0, 0, 1}, color); // Normal approx
        // Right
        addQuad(center - v + u + w, center - v + u - w, center + v + u - w, center + v + u + w, v3{1, 0, 0}, color);
        // Back
        addQuad(center - v + u - w, center - v - u - w, center + v - u - w, center + v + u - w, v3{0, 0, -1}, color);
        // Left
        addQuad(center - v - u - w, center - v - u + w, center + v - u + w, center + v - u - w, v3{-1, 0, 0}, color);
        
        // Bottom (optional, usually hidden)
        addQuad(center - v - u - w, center - v - u + w, center + v - u + w, center + v - u - w, v3{0, -1, 0}, color);
        
        uint32_t endIdx = (uint32_t)indices.size();
        drawRanges[name] = {startIdx, endIdx - startIdx};
    };
    
    uint32_t sceneStart = (uint32_t)indices.size();
    
    // Floor (White)
    addQuad(v3{-h, -h, -h}, v3{ h, -h, -h}, v3{ h, -h,  h}, v3{-h, -h,  h}, v3{0, 1, 0}, white);
    
    // Ceiling (White)
    addQuad(v3{-h,  h,  h}, v3{ h,  h,  h}, v3{ h,  h, -h}, v3{-h,  h, -h}, v3{0, -1, 0}, white);
    
    // Back Wall (White)
    addQuad(v3{ h, -h, -h}, v3{-h, -h, -h}, v3{-h,  h, -h}, v3{ h,  h, -h}, v3{0, 0, 1}, white);
    
    // Left Wall (Red)
    addQuad(v3{-h, -h, -h}, v3{-h, -h,  h}, v3{-h,  h,  h}, v3{-h,  h, -h}, v3{1, 0, 0}, red);
    
    // Right Wall (Green)
    addQuad(v3{ h, -h,  h}, v3{ h, -h, -h}, v3{ h,  h, -h}, v3{ h,  h,  h}, v3{-1, 0, 0}, green);
    
    uint32_t sceneEnd = (uint32_t)indices.size();
    drawRanges["Room"] = {sceneStart, sceneEnd - sceneStart};
    
    // Tall Box (Right) -> Make this the Mirror
    addBox(v3{0.0f, 0.0f, 0.0f}, v3{3.0f, 6.0f, 3.0f}, 0.0f, white, "MirrorBox");
    
    // Short Box (Left)
    addBox(v3{0.0f, 0.0f, 0.0f}, v3{3.0f, 3.0f, 3.0f}, 0.0f, white, "ShortBox");

    
    // Skybox Mesh
    {
        uint32_t skyboxStart = (uint32_t)indices.size();
        float d = 50.0f;
        v3 skyboxColor{0.5f, 0.5f, 0.5f};
        // Front (+Z)
        addQuad(v3{-d, -d, d}, v3{ d, -d, d}, v3{ d,  d, d}, v3{-d,  d, d}, v3{0, 0, -1}, skyboxColor);
        // Back (-Z)
        addQuad(v3{ d, -d, -d}, v3{-d, -d, -d}, v3{-d,  d, -d}, v3{ d,  d, -d}, v3{0, 0, 1}, skyboxColor);
        // Left (-X)
        addQuad(v3{-d, -d, -d}, v3{-d, -d,  d}, v3{-d,  d,  d}, v3{-d,  d, -d}, v3{1, 0, 0}, skyboxColor);
        // Right (+X)
        addQuad(v3{ d, -d,  d}, v3{ d, -d, -d}, v3{ d,  d, -d}, v3{ d,  d,  d}, v3{-1, 0, 0}, skyboxColor);
        // Top (+Y)
        addQuad(v3{-d,  d, -d}, v3{-d,  d,  d}, v3{ d,  d,  d}, v3{ d,  d, -d}, v3{0, -1, 0}, skyboxColor);
        // Bottom (-Y)
        addQuad(v3{-d, -d,  d}, v3{-d, -d, -d}, v3{ d, -d, -d}, v3{ d, -d,  d}, v3{0, 1, 0}, skyboxColor);
        
        uint32_t skyboxEnd = (uint32_t)indices.size();
        drawRanges["Skybox"] = {skyboxStart, skyboxEnd - skyboxStart};
    }


    // Add a floating small cube at (0, 0, 5) inside the +Z view
        {
            float sd = 1.0f; // Small cube size
            primal::math::v3 center = {0.0f, 0.0f, 5.0f};
            
            // Define Rotated Basis Vectors (45 deg around Y and X)
            // Approx values for normalized vectors
            // Right (X')
            // primal::math::v3 right = {0.707f, 0.0f, -0.707f};
            // Up (Y') - Rotated 45 deg around X axis relative to world? 
            // Let's just use an arbitrary rotation matrix manually
            // Rot Y 45: X=(0.707, 0, -0.707), Y=(0,1,0), Z=(0.707, 0, 0.707)
            // Rot X 45: 
            // X_final = (0.707, 0, -0.707)
            // Y_final = (0.5, 0.707, 0.5) 
            // Z_final = (0.5, -0.707, 0.5)
            
            // Re-normalizing to be safe
            // Standard Axis Aligned Cornell Box
            // X axis
            primal::math::v3 X = {1.0f, 0.0f, 0.0f};
            // Y axis
            primal::math::v3 Y = {0.0f, 1.0f, 0.0f};
            // Z axis
            primal::math::v3 Z = {0.0f, 0.0f, 1.0f};
            
            // Scale
            X.x *= sd; X.y *= sd; X.z *= sd;
            Y.x *= sd; Y.y *= sd; Y.z *= sd;
            Z.x *= sd; Z.y *= sd; Z.z *= sd;

            // Front (+Z) - OPEN for Cornell Box
            // addFace(Z, X, Y, center + Z);
            
            // Back (-Z) - Green Wall (using Color attribute logic in addFace?)
            // Actually addFace uses normal to determine color in current implementation
            addFace(primal::math::v3{-Z.x, -Z.y, -Z.z}, primal::math::v3{-X.x, -X.y, -X.z}, Y, center - Z);
            // Right (+X) - Red
            addFace(X, primal::math::v3{-Z.x, -Z.y, -Z.z}, Y, center + X);
            // Left (-X) - Blue/Green?
            addFace(primal::math::v3{-X.x, -X.y, -X.z}, Z, Y, center - X);
            // Top (+Y) - White
            addFace(Y, X, primal::math::v3{-Z.x, -Z.y, -Z.z}, center + Y);
            // Bottom (-Y) - White
            addFace(primal::math::v3{-Y.x, -Y.y, -Y.z}, X, Z, center - Y);
        }

    // Create RHI Buffers for Cube
        // Note: In a real scenario, we'd use a RenderMesh class that manages these.
        // For this test, we create raw buffers and bind them manually.
        
        BufferDesc vbDesc{};
        vbDesc.size = vertices.size() * sizeof(Vertex);
        vbDesc.vertex.vertexStride = sizeof(Vertex);
        vbDesc.type = BufferType::Vertex;
        vbDesc.usage = GPUMemoryUsage::Dynamic;
        vbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        vbDesc.bindFlags = static_cast<uint32_t>(BufferUsageFlags::Vertex);

        vertexBuffer = device->CreateBuffer(vbDesc);
        
        void* vbData = device->MapBuffer(vertexBuffer);
        memcpy(vbData, vertices.data(), vbDesc.size);
        device->UnmapBuffer(vertexBuffer);
        
        BufferDesc ibDesc{};
        ibDesc.size = indices.size() * sizeof(uint32_t);
        ibDesc.index.format = DataFormat::R32_UInt;
        ibDesc.type = BufferType::Index;
        ibDesc.usage = GPUMemoryUsage::Dynamic;
        ibDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        ibDesc.bindFlags = static_cast<uint32_t>(BufferUsageFlags::Index);
        
        indexBuffer = device->CreateBuffer(ibDesc);
        
        void* ibData = device->MapBuffer(indexBuffer);
        memcpy(ibData, indices.data(), ibDesc.size);
        device->UnmapBuffer(indexBuffer);
        
        indexCount = (uint32_t)indices.size();
    std::cout << "DEBUG: CreateCubeMesh Complete. Vertices: " << vertices.size() << ", Indices: " << indexCount << std::endl;
}

void MultiViewTestCase::CreateSphereMesh() {
    // Generate UV Sphere
    struct Vertex {
        float position[3];
        float normal[3];
        float uv[2];
        float tangent[4]; // Add Tangent for PBR
    };
    
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    
    const int X_SEGMENTS = 64;
    const int Y_SEGMENTS = 64;
    const float PI = 3.14159265359f;
    
    for (int y = 0; y <= Y_SEGMENTS; ++y) {
        for (int x = 0; x <= X_SEGMENTS; ++x) {
            float xSegment = (float)x / (float)X_SEGMENTS;
            float ySegment = (float)y / (float)Y_SEGMENTS;
            float xPos = std::cos(xSegment * 2.0f * PI) * std::sin(ySegment * PI);
            float yPos = std::cos(ySegment * PI);
            float zPos = std::sin(xSegment * 2.0f * PI) * std::sin(ySegment * PI);
            
            Vertex v;
            v.position[0] = xPos; v.position[1] = yPos; v.position[2] = zPos;
            v.normal[0] = xPos; v.normal[1] = yPos; v.normal[2] = zPos;
            v.uv[0] = xSegment; v.uv[1] = ySegment;
            
            // Tangent calculation (simplified for sphere)
            // T = (-sin(theta)sin(phi), 0, cos(theta)sin(phi)) -> normalized
            v.tangent[0] = -std::sin(xSegment * 2.0f * PI);
            v.tangent[1] = 0.0f;
            v.tangent[2] = std::cos(xSegment * 2.0f * PI);
            v.tangent[3] = 1.0f;
            
            vertices.push_back(v);
        }
    }
    
    for (int y = 0; y < Y_SEGMENTS; ++y) {
        for (int x = 0; x < X_SEGMENTS; ++x) {
            indices.push_back((y + 1) * (X_SEGMENTS + 1) + x);
            indices.push_back(y * (X_SEGMENTS + 1) + x);
            indices.push_back(y * (X_SEGMENTS + 1) + x + 1);
            
            indices.push_back((y + 1) * (X_SEGMENTS + 1) + x);
            indices.push_back(y * (X_SEGMENTS + 1) + x + 1);
            indices.push_back((y + 1) * (X_SEGMENTS + 1) + x + 1);
        }
    }
    
    BufferDesc vbDesc{};
    vbDesc.size = vertices.size() * sizeof(Vertex);
    vbDesc.vertex.vertexStride = sizeof(Vertex);
    vbDesc.type = BufferType::Vertex;
    vbDesc.usage = GPUMemoryUsage::Dynamic;
    vbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    vbDesc.bindFlags = static_cast<uint32_t>(BufferUsageFlags::Vertex);

    sphereVertexBuffer = device->CreateBuffer(vbDesc);
    
    void* vbData = device->MapBuffer(sphereVertexBuffer);
    memcpy(vbData, vertices.data(), vbDesc.size);
    device->UnmapBuffer(sphereVertexBuffer);
    
    BufferDesc ibDesc{};
    ibDesc.size = indices.size() * sizeof(uint32_t);
    ibDesc.index.format = DataFormat::R32_UInt;
    ibDesc.type = BufferType::Index;
    ibDesc.usage = GPUMemoryUsage::Dynamic;
    ibDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    ibDesc.bindFlags = static_cast<uint32_t>(BufferUsageFlags::Index);
    
    sphereIndexBuffer = device->CreateBuffer(ibDesc);
    
    void* ibData = device->MapBuffer(sphereIndexBuffer);
    memcpy(ibData, indices.data(), ibDesc.size);
    device->UnmapBuffer(sphereIndexBuffer);
    
    sphereIndexCount = (uint32_t)indices.size();
    std::cout << "DEBUG: CreateSphereMesh Complete. Vertices: " << vertices.size() << ", Indices: " << sphereIndexCount << std::endl;
}

bool MultiViewTestCase::SetupIBL() {
    std::cout << ">>> Setting up IBL <<<" << std::endl;
    iblPrecomputer = std::make_unique<IBLPrecomputer>(device);
    if (!iblPrecomputer->Initialize()) {
        std::cout << "Failed to initialize IBLPrecomputer" << std::endl;
        return false;
    }

    // 1. Create Dummy Env Map (Cube)
    // For now, create a 1x1 cubemap with different colors
    // Or better, 32x32 to have some gradients
    uint32_t envSize = 32;
    TextureDesc desc{};
    desc.size = {envSize, envSize, 1};
    desc.mipLevels = 1; // Base only
    desc.arraySize = 1;
    desc.format = DataFormat::RGBA8_UNorm;
    desc.type = TextureType::TextureCube;
    desc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
    
    envMap = device->CreateTexture(desc);
    
    // Upload data (Gradient)
    std::vector<uint32_t> pixels(envSize * envSize);
    for (int face = 0; face < 6; ++face) {
        for (uint32_t i = 0; i < envSize * envSize; ++i) {
            // R: Face ID, G: X, B: Y
            uint8_t r = face * 40;
            uint8_t g = (i % envSize) * 255 / envSize;
            uint8_t b = (i / envSize) * 255 / envSize;
            pixels[i] = 0xFF000000 | (b << 16) | (g << 8) | r;
        }
        // device->UpdateTexture(envMap, pixels.data(), face, 0); // Need UpdateTexture helper that supports slices
        // Assuming we don't have UpdateTexture for Cube slices easily exposed in RHI wrapper yet?
        // Let's assume UpdateTexture handles it or we map it. 
        // Checking MetalDevice... MapTexture works for Shared/Managed.
        // Assuming we can copy buffer to texture.
        // For simplicity, let's skip upload if complicated and just rely on cleared color (black) -> IBL will be dark.
        // Wait, IBLPrecomputer needs something to process.
        // Let's rely on CreateTexture with initial data if possible? No.
        
        // Use CopyBufferToTexture
        BufferDesc stageDesc{};
        stageDesc.size = pixels.size() * 4;
        stageDesc.bindFlags = static_cast<uint32_t>(BufferUsageFlags::TransferSrc);
        stageDesc.usage = GPUMemoryUsage::Staging;
        stageDesc.memoryUsage = GPUMemoryUsage::Staging; // Staging heap for CPU->GPU
        
        ResourceHandle stagingBuf = device->CreateBuffer(stageDesc);
        void* ptr = device->MapBuffer(stagingBuf);
        memcpy(ptr, pixels.data(), stageDesc.size);
        device->UnmapBuffer(stagingBuf);
        
        CommandBufferHandle cmdHandle = device->CreateCommandBuffer(CommandQueueType::Graphics);
        if (cmdHandle != handles::INVALID_COMMAND_BUFFER) {
            RHICommandBuffer* cmd = rhi::GetCommandBuffer(cmdHandle);
            if (cmd && cmd->Begin()) {
                BufferTextureCopyRegion region;
                region.bufferOffset = 0;
                region.bufferRowLength = 0;
                region.bufferImageHeight = 0;
                region.imageSubresource.mipLevel = 0;
                region.imageSubresource.baseArrayLayer = face;
                region.imageSubresource.layerCount = 1;
                region.imageOffset = {0, 0, 0};
                region.imageExtent = {envSize, envSize, 1};
                
                cmd->CopyBufferToTexture(stagingBuf, envMap, &region, 1);
                cmd->End();
                cmd->Submit();
                device->WaitIdle(); // Simple sync
            }
            device->DestroyCommandBuffer(cmdHandle);
        }
        device->DestroyBuffer(stagingBuf);
    }

    std::cout << "Computing Irradiance Map..." << std::endl;
    irradianceMap = iblPrecomputer->ComputeIrradianceMap(envMap);
    
    std::cout << "Computing Prefiltered Map..." << std::endl;
    prefilteredMap = iblPrecomputer->ComputePrefilteredEnvironmentMap(envMap);
    
    std::cout << "Computing BRDF LUT..." << std::endl;
    brdfLUT = iblPrecomputer->ComputeBRDFIntegrationMap();
    
    return true;
}

// Matches StandardPBR.metal PBRMaterialParameters
struct PBRMaterialParameters {
    v4 baseColorFactor;
    v3 emissiveFactor;
    float _pad0; // Padding to match Metal float3 size (16 bytes)
    float roughnessFactor;
    float metallicFactor;
    float normalScale;
    float occlusionStrength;
    float padding;

    int hasBaseColorTexture;
    int hasNormalTexture;
    int hasMetallicRoughnessTexture;
    int hasOcclusionTexture;
    int hasEmissiveTexture;
    int padding2[3];
};

bool MultiViewTestCase::CreatePBRResources() {
    std::cout << ">>> Creating PBR Resources <<<" << std::endl;
    
    // 1. Load Shader
    std::string shaderPath = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/RHI/Shaders/StandardPBR.metal";
    std::string shaderSource = ReadShaderFile(shaderPath);
    if (shaderSource.empty()) return false;
    
    // 2. Create Descriptor Set Layout (Still needed for Material configuration)
    // Matches StandardPBR.metal bindings
    // Buffer 0: Vertices (handled by VertexInput)
    // Buffer 1: ViewUniforms
    // Buffer 2: InstanceUniforms
    // Buffer 3: MaterialParams
    // Textures 0-4: PBR Maps
    // Textures 5-7: IBL Maps
    // Samplers 0-1
    
    std::vector<DescriptorSetLayoutBinding> bindings;
    
    // View Uniforms (Set 0, Binding 1)
    DescriptorSetLayoutBinding viewUniformBinding;
    viewUniformBinding.binding = 1;
    viewUniformBinding.descriptorType = DescriptorType::UniformBuffer;
    viewUniformBinding.descriptorCount = 1;
    viewUniformBinding.stageFlags = ShaderStage::Vertex | ShaderStage::Pixel;
    bindings.push_back(viewUniformBinding);

    // Instance Uniforms (Set 0, Binding 2)
    DescriptorSetLayoutBinding instanceUniformBinding;
    instanceUniformBinding.binding = 2;
    instanceUniformBinding.descriptorType = DescriptorType::UniformBuffer;
    instanceUniformBinding.descriptorCount = 1;
    instanceUniformBinding.stageFlags = ShaderStage::Vertex | ShaderStage::Pixel;
    bindings.push_back(instanceUniformBinding);

    // Material Params (Set 0, Binding 3)
    DescriptorSetLayoutBinding materialParamsBinding;
    materialParamsBinding.binding = 3;
    materialParamsBinding.descriptorType = DescriptorType::UniformBuffer;
    materialParamsBinding.descriptorCount = 1;
    materialParamsBinding.stageFlags = ShaderStage::Vertex | ShaderStage::Pixel;
    bindings.push_back(materialParamsBinding);
    
    // Textures (0-7)
    for(int i=0; i<=7; ++i) {
        DescriptorSetLayoutBinding textureBinding;
        textureBinding.binding = (uint32_t)i;
        textureBinding.descriptorType = DescriptorType::SampledImage;
        textureBinding.descriptorCount = 1;
        textureBinding.stageFlags = ShaderStage::Pixel;
        bindings.push_back(textureBinding);
    }
    
    // Samplers (0-1)
    DescriptorSetLayoutBinding sampler0Binding;
    sampler0Binding.binding = 0;
    sampler0Binding.descriptorType = DescriptorType::Sampler;
    sampler0Binding.descriptorCount = 1;
    sampler0Binding.stageFlags = ShaderStage::Pixel;
    bindings.push_back(sampler0Binding);

    DescriptorSetLayoutBinding sampler1Binding;
    sampler1Binding.binding = 1;
    sampler1Binding.descriptorType = DescriptorType::Sampler;
    sampler1Binding.descriptorCount = 1;
    sampler1Binding.stageFlags = ShaderStage::Pixel;
    bindings.push_back(sampler1Binding);
    
    DescriptorSetLayoutDesc layoutDesc;
    layoutDesc.bindingCount = bindings.size();
    layoutDesc.bindings = bindings.data();
    
    pbrDSLayout = device->CreateDescriptorSetLayout(layoutDesc);
    
    // 3. Create Pipeline Layout
    PipelineLayoutDesc plDesc;
    plDesc.setLayoutCount = 1;
    plDesc.setLayouts = &pbrDSLayout;
    pbrPipelineLayout = device->CreatePipelineLayout(plDesc);
    
    // 4. Create Material
    pbrMaterial = std::make_unique<Material>();
    pbrMaterial->SetShader(ShaderStage::Vertex, shaderSource.data(), shaderSource.size(), "vertexMain");
    pbrMaterial->SetShader(ShaderStage::Pixel, shaderSource.data(), shaderSource.size(), "fragmentMain");
    
    pbrMaterial->SetDescriptorSetLayout(pbrDSLayout);
    pbrMaterial->SetPipelineLayout(pbrPipelineLayout);
    
    pbrMaterial->SetUniformBufferBinding(3);
    pbrMaterial->SetUniformBlockSize(sizeof(PBRMaterialParameters));
    
    // 5. Create Pipeline (Manual creation for Test Framework compatibility)
    // Note: In a full engine, Material would help create this, but here we do it manually to ensure we have the handle.
    // However, Material stores the state, so we should sync it.
    
    GraphicsPipelineDesc pipeDesc{};
    pipeDesc.layout = pbrPipelineLayout;
    pipeDesc.topology = PrimitiveTopology::TriangleList;
    pipeDesc.cullMode = CullMode::Back;
    pipeDesc.enableDepthTest = true;
    pipeDesc.enableDepthWrite = true;
    pipeDesc.depthStencilFormat = DataFormat::D32_Float;
    
    // Set Shaders from Device (Material::SetShader stores data, doesn't return handle here easily without compiling)
    // So we compile them again or assume Material cached them? 
    // Material doesn't compile until GetPipeline.
    // Let's just re-compile or use the ones from Material if we could.
    // For simplicity, compile here as before.
    ShaderHandle vs = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Vertex, "vertexMain");
    ShaderHandle fs = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Pixel, "fragmentMain");
    pipeDesc.vertexShader = vs;
    pipeDesc.pixelShader = fs;
    
    TextureDesc bbDesc = renderSystem.GetBackBufferDesc();
    pipeDesc.renderTargetCount = 1;
    pipeDesc.renderTargetFormats[0] = bbDesc.format;
    
    // Vertex Input (StandardPBR.metal VertexIn)
    VertexInputBinding vBinding;
    vBinding.binding = 0;
    vBinding.stride = sizeof(float) * (3+3+2+4); // 12 floats
    vBinding.perVertex = true;
    pipeDesc.vertexBindings.push_back(vBinding);
    
    VertexInputAttribute attrs[4];
    attrs[0] = {0, 0, DataFormat::RGB32_Float, 0}; // Pos
    attrs[1] = {1, 0, DataFormat::RGB32_Float, sizeof(float)*3}; // Normal
    attrs[2] = {2, 0, DataFormat::RG32_Float, sizeof(float)*6}; // UV
    attrs[3] = {3, 0, DataFormat::RGBA32_Float, sizeof(float)*8}; // Tangent
    
    for(auto& attr : attrs) pipeDesc.vertexAttributes.push_back(attr);
    
    pbrPipeline = device->CreateGraphicsPipeline(pipeDesc);
    if (pbrPipeline == handles::INVALID_PIPELINE) {
        std::cout << "Failed to create PBR Pipeline" << std::endl;
        return false;
    }
    
    // 6. Create Material Instance
    pbrMaterialInstance = std::make_unique<MaterialInstance>(pbrMaterial.get());
    if (!pbrMaterialInstance->Initialize(device)) {
        std::cout << "Failed to initialize PBR MaterialInstance" << std::endl;
        return false;
    }
    
    // 7. Set Initial Parameters
    PBRMaterialParameters params{};
    params.baseColorFactor = {1.0f, 0.0f, 0.0f, 1.0f}; // Red Sphere
    params.roughnessFactor = 0.1f; // Shiny
    params.metallicFactor = 1.0f; // Metal
    params.normalScale = 1.0f;
    params.occlusionStrength = 1.0f;
    params.emissiveFactor = {0,0,0};
    
    pbrMaterialInstance->SetUniformData(0, &params, sizeof(params));
    
    // 8. Bind Resources to Material Instance
    // Bind View/Instance Buffers (Binding 1 & 2)
    pbrMaterialInstance->SetBuffer(1, viewUniformBuffer, sizeof(math::m4x4)*12, 0);
    pbrMaterialInstance->SetBuffer(2, instanceUniformBuffer, sizeof(SceneData), 0);
    
    // Bind Textures (5, 6, 7)
    if (irradianceMap != handles::INVALID_RESOURCE) pbrMaterialInstance->SetTexture(5, irradianceMap);
    if (prefilteredMap != handles::INVALID_RESOURCE) pbrMaterialInstance->SetTexture(6, prefilteredMap);
    if (brdfLUT != handles::INVALID_RESOURCE) pbrMaterialInstance->SetTexture(7, brdfLUT);
    
    // Bind Samplers (0, 1)
    SamplerDesc sampDesc{};
    sampDesc.minFilter = FilterMode::Linear;
    sampDesc.magFilter = FilterMode::Linear;
    sampDesc.addressU = TextureAddressMode::Wrap;
    sampDesc.addressV = TextureAddressMode::Wrap;
    pbrDefaultSampler = device->CreateSampler(sampDesc);
    
    pbrMaterialInstance->SetSampler(0, pbrDefaultSampler);
    pbrMaterialInstance->SetSampler(1, pbrDefaultSampler);
    
    // Initial Update
    pbrMaterialInstance->Update(device);
    
    return true;
}

/**
 * @brief Initializes the Multi-View Test Case.
 * 
 * Sets up:
 * - RHI Device and Window
 * - RenderGraph
 * - Scene Geometry (CubeMesh)
 * - Pipelines (MultiView, Main, Blit, Debug, SSR)
 * - Resources (Textures, Buffers, Descriptors)
 * 
 * @return true if initialization succeeds, false otherwise.
 */
bool MultiViewTestCase::Initialize() {
    std::cout << ">>> STARTING MULTI-VIEW TEST (DIRECT RENDER MODE - DEBUG) <<<" << std::endl;
    // 1. Initialize Window
    primal::platform::window_init_info info{};
    info.caption = "Multi-View Integration Test (Grid Shader)";
    info.width = 1280;
    info.height = 720;
    info.left = 200;
    info.top = 200;
    window = primal::platform::create_window(&info);
    
    if (!window.is_valid()) return false;

    // 2. Create Device
    DeviceDesc deviceDesc{};
    deviceDesc.platform = RHIPlatform::Metal;
    deviceDesc.enableDebug = true;
    auto metalDevice = std::make_unique<MetalDevice>(deviceDesc);
    if (!metalDevice->Initialize()) {
        return false;
    }
    
    device_ownership = std::move(metalDevice);
    device = device_ownership.get();
    
    // 3. Initialize Render System
    RenderSystemInitInfo sysInfo;
    sysInfo.device = device;
    sysInfo.window = window.handle();
    sysInfo.width = info.width;
    sysInfo.height = info.height;

    if (!renderSystem.Initialize(sysInfo)) {
        return false;
    }
    
    // 4. Create RenderGraph
    renderGraph = std::make_unique<RenderGraph>(*device);
    
    // 5. Load Shader
    std::string shaderPath = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/shaders/MultiView.metal"; 
    std::string shaderSource = ReadShaderFile(shaderPath);
    if (shaderSource.empty()) {
        std::cout << "Failed to load shader: " << shaderPath << std::endl;
        return false;
    }
    
    std::cout << "Loaded Shader Source. Length: " << shaderSource.length() << std::endl;
    if (shaderSource.find("vertexMainSimple") != std::string::npos) {
        std::cout << "SHADER CHECK: Found 'vertexMainSimple' in source code." << std::endl;
    } else {
        std::cout << "SHADER CHECK: FAILED to find 'vertexMainSimple' in source code!" << std::endl;
    }

    vertexShader = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Vertex, "vertexMain");
    pixelShader = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Pixel, "fragmentMain");
    
    if (vertexShader == handles::INVALID_SHADER || pixelShader == handles::INVALID_SHADER) {
        std::cout << "Failed to create shaders" << std::endl;
        return false;
    }

    // 6. Create Pipeline Layout
    PipelineLayoutDesc layoutDesc;
    
    DescriptorSetLayoutBinding bindings[2];
    bindings[0].binding = 1; // ViewUniforms
    bindings[0].descriptorType = DescriptorType::UniformBuffer;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = ShaderStage::Vertex;
    
    bindings[1].binding = 2; // InstanceUniforms
    bindings[1].descriptorType = DescriptorType::UniformBuffer;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = ShaderStage::Vertex | ShaderStage::Pixel; // Vertex + Fragment
    
    DescriptorSetLayoutDesc dsDesc;
    dsDesc.bindingCount = 2;
    dsDesc.bindings = bindings;
    
    dsLayout = device->CreateDescriptorSetLayout(dsDesc);
    
    layoutDesc.setLayoutCount = 1;
    layoutDesc.setLayouts = &dsLayout;
    
    pipelineLayout = device->CreatePipelineLayout(layoutDesc);
    
    // 7. Create Graphics Pipeline
    GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.vertexShader = vertexShader;
    pipelineDesc.pixelShader = pixelShader;
    pipelineDesc.layout = pipelineLayout;
    pipelineDesc.topology = PrimitiveTopology::TriangleList;
    pipelineDesc.cullMode = CullMode::None; // Disable culling to see inside of cube
    
    // Vertex Input
    VertexInputBinding binding;
    binding.binding = 0;
    binding.stride = sizeof(float) * 11; // 3+3+2+3
    binding.perVertex = true;
    
    pipelineDesc.vertexBindings.push_back(binding);
    
    VertexInputAttribute attr0; // Pos
    attr0.location = 0;
    attr0.binding = 0;
    attr0.format = DataFormat::RGB32_Float;
    attr0.offset = 0;
    
    VertexInputAttribute attr1; // Normal
    attr1.location = 1;
    attr1.binding = 0;
    attr1.format = DataFormat::RGB32_Float;
    attr1.offset = sizeof(float) * 3;
    
    VertexInputAttribute attr2; // UV
    attr2.location = 2;
    attr2.binding = 0;
    attr2.format = DataFormat::RG32_Float;
    attr2.offset = sizeof(float) * 6;

    VertexInputAttribute attr3; // Color
    attr3.location = 3;
    attr3.binding = 0;
    attr3.format = DataFormat::RGB32_Float;
    attr3.offset = sizeof(float) * 8;
    
    pipelineDesc.vertexAttributes.push_back(attr0);
    pipelineDesc.vertexAttributes.push_back(attr1);
    pipelineDesc.vertexAttributes.push_back(attr2);
    pipelineDesc.vertexAttributes.push_back(attr3);
    
    TextureDesc backBufferDesc = renderSystem.GetBackBufferDesc(); // Get actual format
    pipelineDesc.renderTargetCount = 1;
    pipelineDesc.renderTargetFormats[0] = backBufferDesc.format; // Use actual format
    std::cout << "DEBUG: Pipeline Target Format: " << (int)backBufferDesc.format << std::endl;

    // Enable Depth Test for MultiView Pass (so small cube occludes big cube)
    pipelineDesc.depthStencilFormat = DataFormat::D32_Float;
    pipelineDesc.enableDepthTest = true;
    pipelineDesc.enableDepthWrite = true;
    pipelineDesc.cullMode = CullMode::None; // Ensure double-sided rendering for safety

    std::cout << "DEBUG: Pipeline Setup - Depth Test: Enabled, CompareOp: Less, CullMode: None" << std::endl;

    pipeline = device->CreateGraphicsPipeline(pipelineDesc);
    if (pipeline == handles::INVALID_PIPELINE) {
        std::cout << "Failed to create pipeline" << std::endl;
        return false;
    }

    // 7.05 Create Simple Pipeline
    // USE DEBUG SHADER TO VERIFY DRAW CALLS
    // simpleVertexShader = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Vertex, "vertexMainBufferDebug");
    simpleVertexShader = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Vertex, "vertexMainSimple");
    if (simpleVertexShader == handles::INVALID_SHADER) std::cout << "CRITICAL: Failed to create vertexMainSimple" << std::endl;

    simplePixelShader = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Pixel, "fragmentMainSimple");
    if (simplePixelShader == handles::INVALID_SHADER) std::cout << "CRITICAL: Failed to create fragmentMainSimple" << std::endl;

    // Create Empty Layout
    // PipelineLayoutDesc emptyLayoutDesc;
    // PipelineLayoutHandle emptyLayout = device->CreatePipelineLayout(emptyLayoutDesc);

    GraphicsPipelineDesc simpleDesc = pipelineDesc;
    simpleDesc.vertexShader = simpleVertexShader;
    simpleDesc.pixelShader = simplePixelShader;
    simpleDesc.layout = pipelineLayout; // Use Correct Layout with Uniforms
    // simpleDesc.layout = emptyLayout; 
    
    // simpleDesc.vertexAttributes.clear();
    // simpleDesc.vertexBindings.clear();
    simpleDesc.cullMode = CullMode::None;
    simpleDesc.enableDepthTest = false;
    simpleDesc.enableDepthWrite = false;
    simpleDesc.topology = PrimitiveTopology::TriangleList; // Explicitly set topology

    backBufferDesc = renderSystem.GetBackBufferDesc(); 
    simpleDesc.renderTargetCount = 1;
    simpleDesc.renderTargetFormats[0] = backBufferDesc.format;
    std::cout << "DEBUG: Pipeline Target Format: " << (int)backBufferDesc.format << std::endl;
    
    simplePipeline = device->CreateGraphicsPipeline(simpleDesc);
    if (simplePipeline == handles::INVALID_PIPELINE) {
        std::cout << "CRITICAL ERROR: Failed to create Simple Pipeline!" << std::endl;
        return false;
    } else {
        std::cout << "DEBUG: Simple Pipeline Created Successfully." << std::endl;
    }

    // 7.05 Create Main View Pipeline (Matches BackBuffer Format)
    // Use Single View Shaders (No Layer Output)
    rhi::ShaderHandle vertexShaderSingle = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Vertex, "vertexMainSingle");
    rhi::ShaderHandle pixelShaderSingle = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Pixel, "fragmentMainSingle");

    GraphicsPipelineDesc mainPipeDesc = pipelineDesc;
    mainPipeDesc.vertexShader = vertexShaderSingle;
    mainPipeDesc.pixelShader = pixelShaderSingle;
    
    // Update Target Format to match BackBuffer
    TextureDesc swapchainDesc = renderSystem.GetBackBufferDesc();
    mainPipeDesc.renderTargetFormats[0] = swapchainDesc.format;
    mainPipeDesc.renderTargetFormats[1] = DataFormat::RG16_Float; // Velocity
    mainPipeDesc.renderTargetCount = 2;
    
    // Ensure Layout is correct (should be same as pipelineLayout)
    mainPipeDesc.layout = pipelineLayout;
    
    // Create Pipeline
    mainPipeline = device->CreateGraphicsPipeline(mainPipeDesc);
    if (mainPipeline == handles::INVALID_PIPELINE) {
        std::cout << "CRITICAL: Failed to create Main View Pipeline!" << std::endl;
    } else {
        std::cout << "DEBUG: Main View Pipeline Created. Format: " << (int)swapchainDesc.format << std::endl;
    }

    // 7.1 Create Blit Shaders & Pipeline
    blitVertexShader = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Vertex, "blitVertex");
    // Use blitFragment2D for Texture2D sampling
    blitPixelShader = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Pixel, "blitFragment2D");
    if (blitPixelShader == handles::INVALID_SHADER) std::cout << "CRITICAL: Failed to create blitFragment2D shader" << std::endl;
    
    DescriptorSetLayoutDesc blitDSDesc{};
    DescriptorSetLayoutBinding blitBindings[2];
    
    // Binding 0: Texture
    blitBindings[0].binding = 0;
    blitBindings[0].descriptorType = DescriptorType::SampledImage;
    blitBindings[0].descriptorCount = 1;
    blitBindings[0].stageFlags = ShaderStage::Pixel;
    
    // Binding 1: Uniform Buffer (Rotation)
    blitBindings[1].binding = 1;
    blitBindings[1].descriptorType = DescriptorType::UniformBuffer;
    blitBindings[1].descriptorCount = 1;
    blitBindings[1].stageFlags = ShaderStage::Pixel;
    
    blitDSDesc.bindingCount = 2;
    blitDSDesc.bindings = blitBindings;
    blitDSLayout = device->CreateDescriptorSetLayout(blitDSDesc);
    
    // Create Blit Uniform Buffer
    BufferDesc bubDesc{};
    bubDesc.size = sizeof(math::m4x4); // Rotation Matrix
    bubDesc.usage = GPUMemoryUsage::Dynamic;
    bubDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    bubDesc.type = BufferType::Constant;
    bubDesc.bindFlags = static_cast<uint32_t>(BufferUsageFlags::Uniform);
    blitUniformBuffer = device->CreateBuffer(bubDesc);

    DescriptorSetDesc blitSetDesc;
    blitSetDesc.layout = blitDSLayout;
    
    blitDescriptorSets.resize(MAX_FRAMES_IN_FLIGHT);
    for(uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        blitDescriptorSets[i] = device->CreateDescriptorSet(blitSetDesc);
        
        // Update Buffer Binding immediately
        WriteDescriptorSet bufUpdate{};
        DescriptorBufferInfo bufInfo{};
        bufInfo.buffer = blitUniformBuffer;
        bufInfo.offset = 0;
        bufInfo.range = sizeof(math::m4x4);
        
        bufUpdate.dstSet = blitDescriptorSets[i];
        bufUpdate.dstBinding = 1; // Binding 1
        bufUpdate.descriptorCount = 1;
        bufUpdate.descriptorType = DescriptorType::UniformBuffer;
        bufUpdate.bufferInfo = &bufInfo;
        
        device->UpdateDescriptorSets(1, &bufUpdate);
    }
    
    // Create Present Uniform Buffer (Identity)
    {
        BufferDesc bubDesc{};
        bubDesc.size = sizeof(math::m4x4);
        bubDesc.usage = GPUMemoryUsage::Dynamic;
        bubDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        bubDesc.type = BufferType::Constant;
        bubDesc.bindFlags = static_cast<uint32_t>(BufferUsageFlags::Uniform);
        presentUniformBuffer = device->CreateBuffer(bubDesc);
        
        m4x4 identity = rhi::math::MatrixIdentity();
        void* data = device->MapBuffer(presentUniformBuffer);
        if (data) {
            memcpy(data, &identity, sizeof(m4x4));
            device->UnmapBuffer(presentUniformBuffer);
        }
    }

    // Create Present Descriptor Set
    presentDescriptorSet = device->CreateDescriptorSet(blitSetDesc);
    {
        WriteDescriptorSet bufUpdate{};
        DescriptorBufferInfo bufInfo{};
        bufInfo.buffer = presentUniformBuffer; // Use Present Uniform Buffer
        bufInfo.offset = 0;
        bufInfo.range = sizeof(math::m4x4);
        
        bufUpdate.dstSet = presentDescriptorSet;
        bufUpdate.dstBinding = 1; // Binding 1
        bufUpdate.descriptorCount = 1;
        bufUpdate.descriptorType = DescriptorType::UniformBuffer;
        bufUpdate.bufferInfo = &bufInfo;
        
        device->UpdateDescriptorSets(1, &bufUpdate);
    }
    
    PipelineLayoutDesc blitPLDesc{};
    blitPLDesc.setLayouts = &blitDSLayout;
    blitPLDesc.setLayoutCount = 1;
    blitPipelineLayout = device->CreatePipelineLayout(blitPLDesc);
    
    GraphicsPipelineDesc blitPipeDesc{};
    blitPipeDesc.vertexShader = blitVertexShader;
    blitPipeDesc.pixelShader = blitPixelShader;
    blitPipeDesc.layout = blitPipelineLayout;
    blitPipeDesc.cullMode = CullMode::None;
    blitPipeDesc.enableDepthTest = false; // Explicitly disable depth test
    blitPipeDesc.enableDepthWrite = false;
    blitPipeDesc.renderTargetCount = 1;
    blitPipeDesc.renderTargetFormats[0] = renderSystem.GetBackBufferDesc().format; // Use correct format
    blitPipeDesc.depthStencilFormat = DataFormat::Unknown;
    blitPipeDesc.topology = PrimitiveTopology::TriangleList; // CRITICAL FIX
    blitPipeline = device->CreateGraphicsPipeline(blitPipeDesc);
    
    if (blitPipeline == handles::INVALID_PIPELINE) {
        std::cout << "CRITICAL: Failed to create Blit Pipeline!" << std::endl;
    }

    // 7.2 Create Debug Overlay Shaders & Pipeline
    debugVertexShader = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Vertex, "debugVertex");
    if (debugVertexShader == handles::INVALID_SHADER) std::cout << "CRITICAL: Failed to create debugVertex shader" << std::endl;
    
    debugPixelShader = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Pixel, "debugFragment");
    if (debugPixelShader == handles::INVALID_SHADER) std::cout << "CRITICAL: Failed to create debugFragment shader" << std::endl;

    GraphicsPipelineDesc debugPipeDesc = blitPipeDesc;
    debugPipeDesc.vertexShader = debugVertexShader;
    debugPipeDesc.pixelShader = debugPixelShader;
    
    // Disable Depth Test for Debug Overlay
    debugPipeDesc.enableDepthTest = false;
    debugPipeDesc.enableDepthWrite = false;

    // Keep same layout (blitPipelineLayout) and other settings
    
    debugPipeline = device->CreateGraphicsPipeline(debugPipeDesc);
    if (debugPipeline == handles::INVALID_PIPELINE) {
        std::cout << "CRITICAL: Failed to create Debug Pipeline!" << std::endl;
    } else {
        std::cout << "DEBUG: Debug Pipeline Created Successfully." << std::endl;
    }

    // 7.3 Create SSR Composite Pipeline (Additive Blending)
    GraphicsPipelineDesc ssrCompDesc = blitPipeDesc;
    ssrCompDesc.vertexShader = blitVertexShader;
    ssrCompDesc.pixelShader = blitPixelShader;
    ssrCompDesc.enableDepthTest = false;
    ssrCompDesc.enableDepthWrite = false;
    
    // Enable Additive Blending
    ssrCompDesc.enableBlend = true;
    ssrCompDesc.srcColorBlendFactor = BlendFactor::One;
    ssrCompDesc.dstColorBlendFactor = BlendFactor::One;
    ssrCompDesc.colorBlendOp = BlendOp::Add;
    ssrCompDesc.srcAlphaBlendFactor = BlendFactor::One;
    ssrCompDesc.dstAlphaBlendFactor = BlendFactor::One;
    ssrCompDesc.alphaBlendOp = BlendOp::Add;

    ssrCompositePipeline = device->CreateGraphicsPipeline(ssrCompDesc);
    if (ssrCompositePipeline == handles::INVALID_PIPELINE) {
        std::cout << "CRITICAL: Failed to create SSR Composite Pipeline!" << std::endl;
    } else {
        std::cout << "DEBUG: SSR Composite Pipeline Created Successfully." << std::endl;
    }
    
    // 8. Create Uniform Buffers
    BufferDesc ubDesc{};
    ubDesc.size = sizeof(math::m4x4) * 12; // View Uniforms (6 ViewProjs + 6 PreviousViewProjs)
    ubDesc.usage = GPUMemoryUsage::Dynamic;
    ubDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    ubDesc.type = BufferType::Constant;
    ubDesc.bindFlags = static_cast<uint32_t>(BufferUsageFlags::Uniform);
    viewUniformBuffer = device->CreateBuffer(ubDesc);
    
    // Fill View Uniforms
    {
        void* data = device->MapBuffer(viewUniformBuffer);
        if (data) {
            m4x4* matrices = static_cast<m4x4*>(data);
            
            // Projection (90 deg, 1.0 aspect)
            // Metal NDC is Z [0, 1]
            m4x4 proj = CreatePerspective(math::constants::HALF_PI, 1.0f, 0.1f, 100.0f);
            
            // 6 Faces
            v3 eye = primal::math::v3{0, 0, 0};
            
            // DEBUG: Print Matrices
            std::cout << "DEBUG: Projection Matrix:" << std::endl;
            // ... (Simple print logic if needed)

            // Current Frame Matrices
            // +X
            matrices[0] = proj * CreateLookAt(eye, primal::math::v3{1, 0, 0}, primal::math::v3{0, -1, 0});
            // -X
            matrices[1] = proj * CreateLookAt(eye, primal::math::v3{-1, 0, 0}, primal::math::v3{0, -1, 0});
            // +Y
            matrices[2] = proj * CreateLookAt(eye, primal::math::v3{0, 1, 0}, primal::math::v3{0, 0, 1});
            // -Y
            matrices[3] = proj * CreateLookAt(eye, primal::math::v3{0, -1, 0}, primal::math::v3{0, 0, -1});
            // +Z
            matrices[4] = proj * CreateLookAt(eye, primal::math::v3{0, 0, 1}, primal::math::v3{0, -1, 0});
            // -Z
            matrices[5] = proj * CreateLookAt(eye, primal::math::v3{0, 0, -1}, primal::math::v3{0, -1, 0});

            // Previous Frame Matrices (Initialize to same as Current)
            for(int i=0; i<6; ++i) {
                matrices[6+i] = matrices[i];
            }
            
            device->UnmapBuffer(viewUniformBuffer);
        }
    }

    // 8.1 Create Main View Uniform Buffer
    {
        BufferDesc bufDesc{};
        bufDesc.size = sizeof(math::m4x4) * 12;
        bufDesc.usage = GPUMemoryUsage::Dynamic;
        bufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        bufDesc.type = BufferType::Constant;
        bufDesc.bindFlags = static_cast<uint32_t>(BufferUsageFlags::Uniform);
        mainViewUniformBuffer = device->CreateBuffer(bufDesc);
    }
    
    // 8.2 Create Depth Textures
    {
        // MultiView Depth (CubeMap / Array)
        TextureDesc depthDesc{};
        depthDesc.size = {512, 512, 1};
        depthDesc.mipLevels = 1;
        depthDesc.arraySize = 6;
        depthDesc.format = DataFormat::D32_Float;
        depthDesc.type = TextureType::Texture2DArray;
        depthDesc.usage = TextureUsage::DepthStencil;
        multiViewDepthTexture = device->CreateTexture(depthDesc);

        // Main View Depth (Match BackBuffer Size)
        TextureDesc bbDesc = renderSystem.GetBackBufferDesc();
        if (bbDesc.size.x == 0) {
            bbDesc.size.x = info.width * 2; // Assume Retina 2x fallback
            bbDesc.size.y = info.height * 2;
        }
        std::cout << "DEBUG: Creating Main Depth Texture: " << bbDesc.size.x << "x" << bbDesc.size.y << std::endl;

        // Main View Color
        TextureDesc colorDesc = bbDesc;
        colorDesc.format = DataFormat::BGRA8_UNorm;
        colorDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
        mainColorTexture = device->CreateTexture(colorDesc);

        // Create TAA Textures
        TextureDesc velocityDesc = bbDesc;
        velocityDesc.format = DataFormat::RG16_Float;
        velocityDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
        mainVelocityTexture = device->CreateTexture(velocityDesc);
        
        TextureDesc taaDesc = bbDesc;
        taaDesc.format = DataFormat::BGRA8_UNorm;
        taaDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
        taaHistoryTexture = device->CreateTexture(taaDesc);
        taaResultTexture = device->CreateTexture(taaDesc);
        
        // Initialize TAA Pass
        if (!taaPass.Initialize(device, bbDesc.size.x, bbDesc.size.y, taaDesc.format)) {
             std::cout << "Failed to initialize TAA Pass" << std::endl;
             return false;
        }

        TextureDesc mainDepthDesc{};
        mainDepthDesc.size = {bbDesc.size.x, bbDesc.size.y, 1};
        mainDepthDesc.mipLevels = 1;
        mainDepthDesc.arraySize = 1;
        mainDepthDesc.format = DataFormat::D32_Float;
        mainDepthDesc.type = TextureType::Texture2D;
        mainDepthDesc.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource;
        mainDepthTexture = device->CreateTexture(mainDepthDesc);
    }

    ubDesc.size = sizeof(SceneData); // Instance Uniforms + Light Data
    instanceUniformBuffer = device->CreateBuffer(ubDesc);
    
    // Fill Instance Uniforms
    {
        void* data = device->MapBuffer(instanceUniformBuffer);
        if (data) {
            SceneData* sceneData = static_cast<SceneData*>(data);
            
            // Calculate Model Matrix: Translation * Rotation
            sceneData->model = primal::graphics::rhi::math::MatrixIdentity();
            sceneData->lightPos = {0.0f, 4.0f, 0.0f, 1.0f}; // Top Light
            sceneData->lightColor = {1.0f, 1.0f, 1.0f, 1.0f}; // White
            
            // Initialize SH Coefficients (Neutral Ambient)
            for(int i=0; i<9; ++i) sceneData->shCoeffs[i] = {0,0,0,0};
            sceneData->shCoeffs[0] = {0.1f, 0.1f, 0.1f, 1.0f}; // Low ambient gray
            
            device->UnmapBuffer(instanceUniformBuffer);
        }
    }
    
    // 9. Create Descriptor Set
    DescriptorSetDesc setDesc;
    setDesc.layout = dsLayout;
    descriptorSet = device->CreateDescriptorSet(setDesc);
    
    // 9.1 Create Main View Descriptor Set
    mainDescriptorSet = device->CreateDescriptorSet(setDesc);

    WriteDescriptorSet updates[4]; // Updated count
    DescriptorBufferInfo bufInfo0;
    bufInfo0.buffer = viewUniformBuffer;
    bufInfo0.offset = 0;
    bufInfo0.range = sizeof(math::m4x4) * 12;

    updates[0].dstSet = descriptorSet;
    updates[0].dstBinding = 1;
    updates[0].dstArrayElement = 0;
    updates[0].descriptorCount = 1;
    updates[0].descriptorType = DescriptorType::UniformBuffer;
    updates[0].bufferInfo = &bufInfo0;
    
    DescriptorBufferInfo bufInfo1;
    bufInfo1.buffer = instanceUniformBuffer;
    bufInfo1.offset = 0;
    bufInfo1.range = sizeof(SceneData);

    updates[1].dstSet = descriptorSet;
    updates[1].dstBinding = 2;
    updates[1].dstArrayElement = 0;
    updates[1].descriptorCount = 1;
    updates[1].descriptorType = DescriptorType::UniformBuffer;
    updates[1].bufferInfo = &bufInfo1;

    // Updates for Main View Descriptor Set
    DescriptorBufferInfo bufInfoMain;
    bufInfoMain.buffer = mainViewUniformBuffer;
    bufInfoMain.offset = 0;
    bufInfoMain.range = sizeof(math::m4x4) * 12;

    updates[2].dstSet = mainDescriptorSet;
    updates[2].dstBinding = 1;
    updates[2].dstArrayElement = 0;
    updates[2].descriptorCount = 1;
    updates[2].descriptorType = DescriptorType::UniformBuffer;
    updates[2].bufferInfo = &bufInfoMain;

    // Reuse instance buffer for main view (same lights)
    updates[3].dstSet = mainDescriptorSet;
    updates[3].dstBinding = 2;
    updates[3].dstArrayElement = 0;
    updates[3].descriptorCount = 1;
    updates[3].descriptorType = DescriptorType::UniformBuffer;
    updates[3].bufferInfo = &bufInfo1;
    
    device->UpdateDescriptorSets(4, updates);
    
    // 10. Create Cube Mesh
    CreateCubeMesh();
    CreateSphereMesh();
    SetupIBL();
    CreatePBRResources();
    
    // 11. Create Command Buffers (Per Frame)
    // commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    // for(uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
    //     commandBuffers[i] = device->CreateCommandBuffer(CommandQueueType::Graphics);
    // }

    // 12. Setup Input for Debug Toggle
    using namespace primal::input;
    input_source source{};
    source.binding = std::hash<std::string>()("debug_toggle");
    source.source_type = input_source::keyboard;
    source.code = input_code::key_f1;
    source.multiplier = 1.0f;
    bind(source);

    // Bind F2 for SH Toggle
    source.binding = std::hash<std::string>()("sh_toggle");
    source.code = input_code::key_f2;
    bind(source);
    
    if (!CreateReflectionResources()) return false;

    // 12.1 Initialize Skybox Resources
    std::vector<std::string> skyboxFaces = {
        "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/images/skybox_r.jpg", // +X
        "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/images/skybox_l.jpg", // -X
        "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/images/skybox_u.jpg", // +Y
        "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/images/skybox_d.jpg", // -Y
        "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/images/skybox_f.jpg", // +Z
        "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/images/skybox_b.jpg"  // -Z
    };
    
    primal::graphics::rhi::ResourceHandle skyboxTexture = LoadCubemap(skyboxFaces);
    if (skyboxTexture == primal::graphics::rhi::handles::INVALID_RESOURCE) {
        std::cout << "Failed to load Skybox Texture!" << std::endl;
    } else {
        std::cout << "Skybox Texture Loaded Successfully." << std::endl;
        
        // Create Skybox Shaders
        skyboxVertexShader = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Vertex, "vertexSkybox");
        skyboxPixelShader = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Pixel, "fragmentSkybox");
        
        // Create Skybox DS Layout
        DescriptorSetLayoutBinding skyboxBindings[3];
        // Binding 0: Uniforms (Buffer 1) -> Metal uses [[buffer(1)]]
        skyboxBindings[0].binding = 1; 
        skyboxBindings[0].descriptorType = DescriptorType::UniformBuffer;
        skyboxBindings[0].descriptorCount = 1;
        skyboxBindings[0].stageFlags = ShaderStage::Vertex;
        
        // Binding 1: SceneData (Buffer 2) -> Metal uses [[buffer(2)]]
        skyboxBindings[1].binding = 2;
        skyboxBindings[1].descriptorType = DescriptorType::UniformBuffer;
        skyboxBindings[1].descriptorCount = 1;
        skyboxBindings[1].stageFlags = ShaderStage::Vertex;
        
        // Binding 2: Cubemap (Texture 0) -> Metal uses [[texture(0)]]
        skyboxBindings[2].binding = 0; 
        skyboxBindings[2].descriptorType = DescriptorType::SampledImage; 
        skyboxBindings[2].descriptorCount = 1;
        skyboxBindings[2].stageFlags = ShaderStage::Pixel;
        
        DescriptorSetLayoutDesc skyboxDSDesc;
        skyboxDSDesc.bindingCount = 3;
        skyboxDSDesc.bindings = skyboxBindings;
        skyboxDSLayout = device->CreateDescriptorSetLayout(skyboxDSDesc);
        
        // Create Skybox Pipeline Layout
        PipelineLayoutDesc skyboxPLDesc;
        skyboxPLDesc.setLayoutCount = 1;
        skyboxPLDesc.setLayouts = &skyboxDSLayout;
        skyboxPipelineLayout = device->CreatePipelineLayout(skyboxPLDesc);
        
        // Create Skybox Pipeline
        GraphicsPipelineDesc skyboxPipeDesc = pipelineDesc;
        skyboxPipeDesc.vertexShader = skyboxVertexShader;
        skyboxPipeDesc.pixelShader = skyboxPixelShader;
        skyboxPipeDesc.layout = skyboxPipelineLayout;
        skyboxPipeDesc.cullMode = CullMode::None; // Render inside
        skyboxPipeDesc.enableDepthTest = true;
        skyboxPipeDesc.enableDepthWrite = false; // Background
        skyboxPipeDesc.depthFunc = primal::graphics::rhi::ComparisonFunc::LessEqual;
        
        skyboxPipeline = device->CreateGraphicsPipeline(skyboxPipeDesc);
        
        // Create Descriptor Set
        DescriptorSetDesc setDesc;
        setDesc.layout = skyboxDSLayout;
        skyboxDescriptorSet = device->CreateDescriptorSet(setDesc);
        
        // Update Descriptor Set
        WriteDescriptorSet updates[3];
        DescriptorBufferInfo bufInfo0; bufInfo0.buffer = viewUniformBuffer; bufInfo0.offset = 0; bufInfo0.range = sizeof(math::m4x4) * 12;
        DescriptorBufferInfo bufInfo1; bufInfo1.buffer = instanceUniformBuffer; bufInfo1.offset = 0; bufInfo1.range = sizeof(SceneData);
        DescriptorImageInfo imgInfo; imgInfo.imageView = skyboxTexture; imgInfo.imageLayout = ResourceState::ShaderResource;
        
        updates[0].dstSet = skyboxDescriptorSet; updates[0].dstBinding = 1; updates[0].descriptorType = DescriptorType::UniformBuffer; updates[0].bufferInfo = &bufInfo0; updates[0].descriptorCount = 1;
        updates[1].dstSet = skyboxDescriptorSet; updates[1].dstBinding = 2; updates[1].descriptorType = DescriptorType::UniformBuffer; updates[1].bufferInfo = &bufInfo1; updates[1].descriptorCount = 1;
        updates[2].dstSet = skyboxDescriptorSet; updates[2].dstBinding = 0; updates[2].descriptorType = DescriptorType::SampledImage; updates[2].imageInfo = &imgInfo; updates[2].descriptorCount = 1;
        
        device->UpdateDescriptorSets(3, updates);

        // Create Reflection Skybox Descriptor Sets
        skyboxReflDescriptorSet1 = device->CreateDescriptorSet(setDesc);
        skyboxReflDescriptorSet2 = device->CreateDescriptorSet(setDesc);
        skyboxReflDescriptorSet3 = device->CreateDescriptorSet(setDesc);
        
        // Update Refl 1
        WriteDescriptorSet updates1[3];
        DescriptorBufferInfo bufInfoR1; bufInfoR1.buffer = reflectionUniformBuffer; bufInfoR1.offset = 0; bufInfoR1.range = sizeof(math::m4x4) * 12;
        updates1[0].dstSet = skyboxReflDescriptorSet1; updates1[0].dstBinding = 1; updates1[0].descriptorType = DescriptorType::UniformBuffer; updates1[0].bufferInfo = &bufInfoR1; updates1[0].descriptorCount = 1;
        updates1[1].dstSet = skyboxReflDescriptorSet1; updates1[1].dstBinding = 2; updates1[1].descriptorType = DescriptorType::UniformBuffer; updates1[1].bufferInfo = &bufInfo1; updates1[1].descriptorCount = 1; 
        updates1[2].dstSet = skyboxReflDescriptorSet1; updates1[2].dstBinding = 0; updates1[2].descriptorType = DescriptorType::SampledImage; updates1[2].imageInfo = &imgInfo; updates1[2].descriptorCount = 1; 
        device->UpdateDescriptorSets(3, updates1);

        // Update Refl 2
        WriteDescriptorSet updates2[3];
        DescriptorBufferInfo bufInfoR2; bufInfoR2.buffer = reflectionUniformBuffer2; bufInfoR2.offset = 0; bufInfoR2.range = sizeof(math::m4x4) * 12;
        updates2[0].dstSet = skyboxReflDescriptorSet2; updates2[0].dstBinding = 1; updates2[0].descriptorType = DescriptorType::UniformBuffer; updates2[0].bufferInfo = &bufInfoR2; updates2[0].descriptorCount = 1;
        updates2[1].dstSet = skyboxReflDescriptorSet2; updates2[1].dstBinding = 2; updates2[1].descriptorType = DescriptorType::UniformBuffer; updates2[1].bufferInfo = &bufInfo1; updates2[1].descriptorCount = 1;
        updates2[2].dstSet = skyboxReflDescriptorSet2; updates2[2].dstBinding = 0; updates2[2].descriptorType = DescriptorType::SampledImage; updates2[2].imageInfo = &imgInfo; updates2[2].descriptorCount = 1;
        device->UpdateDescriptorSets(3, updates2);

        // Update Refl 3
        WriteDescriptorSet updates3[3];
        DescriptorBufferInfo bufInfoR3; bufInfoR3.buffer = reflectionUniformBuffer3; bufInfoR3.offset = 0; bufInfoR3.range = sizeof(math::m4x4) * 12;
        updates3[0].dstSet = skyboxReflDescriptorSet3; updates3[0].dstBinding = 1; updates3[0].descriptorType = DescriptorType::UniformBuffer; updates3[0].bufferInfo = &bufInfoR3; updates3[0].descriptorCount = 1;
        updates3[1].dstSet = skyboxReflDescriptorSet3; updates3[1].dstBinding = 2; updates3[1].descriptorType = DescriptorType::UniformBuffer; updates3[1].bufferInfo = &bufInfo1; updates3[1].descriptorCount = 1;
        updates3[2].dstSet = skyboxReflDescriptorSet3; updates3[2].dstBinding = 0; updates3[2].descriptorType = DescriptorType::SampledImage; updates3[2].imageInfo = &imgInfo; updates3[2].descriptorCount = 1;
        device->UpdateDescriptorSets(3, updates3);
    }

    // 12.2 Initialize ShortBox Resources (Independent Movement & SH)
    {
        BufferDesc ubDesc{};
        ubDesc.size = sizeof(SceneData);
        ubDesc.type = BufferType::Constant;
        ubDesc.usage = GPUMemoryUsage::Dynamic;
        ubDesc.bindFlags = (uint32_t)BufferUsageFlags::Uniform;
        shortBoxUniformBuffer = device->CreateBuffer(ubDesc);
        
        DescriptorSetDesc setDesc;
        setDesc.layout = dsLayout; // Reuse main layout
        shortBoxDescriptorSet = device->CreateDescriptorSet(setDesc);
        
        WriteDescriptorSet updates[2];
        DescriptorBufferInfo bufInfo0; bufInfo0.buffer = viewUniformBuffer; bufInfo0.offset = 0; bufInfo0.range = sizeof(math::m4x4) * 12;
        DescriptorBufferInfo bufInfo1; bufInfo1.buffer = shortBoxUniformBuffer; bufInfo1.offset = 0; bufInfo1.range = sizeof(SceneData);
        
        updates[0].dstSet = shortBoxDescriptorSet; updates[0].dstBinding = 1; updates[0].descriptorType = DescriptorType::UniformBuffer; updates[0].bufferInfo = &bufInfo0; updates[0].descriptorCount = 1;
        updates[1].dstSet = shortBoxDescriptorSet; updates[1].dstBinding = 2; updates[1].descriptorType = DescriptorType::UniformBuffer; updates[1].bufferInfo = &bufInfo1; updates[1].descriptorCount = 1;
        
        device->UpdateDescriptorSets(2, updates);

        // Create ShortBox Main Descriptor Set (For Main Pass)
        DescriptorSetDesc setDescMain;
        setDescMain.layout = dsLayout; // Main Layout (View + Instance)
        shortBoxMainDescriptorSet = device->CreateDescriptorSet(setDescMain);
        
        WriteDescriptorSet updatesMain[2];
        DescriptorBufferInfo bufInfoMainView; bufInfoMainView.buffer = mainViewUniformBuffer; bufInfoMainView.offset = 0; bufInfoMainView.range = sizeof(math::m4x4) * 12;
        DescriptorBufferInfo bufInfoShortBox; bufInfoShortBox.buffer = shortBoxUniformBuffer; bufInfoShortBox.offset = 0; bufInfoShortBox.range = sizeof(SceneData);
        
        updatesMain[0].dstSet = shortBoxMainDescriptorSet; updatesMain[0].dstBinding = 1; updatesMain[0].descriptorType = DescriptorType::UniformBuffer; updatesMain[0].bufferInfo = &bufInfoMainView; updatesMain[0].descriptorCount = 1;
        updatesMain[1].dstSet = shortBoxMainDescriptorSet; updatesMain[1].dstBinding = 2; updatesMain[1].descriptorType = DescriptorType::UniformBuffer; updatesMain[1].bufferInfo = &bufInfoShortBox; updatesMain[1].descriptorCount = 1;
        
        device->UpdateDescriptorSets(2, updatesMain);
    }

    // 13. Initialize SSR Pass
    if (!ssrPass.Initialize(device)) {
        std::cout << "Failed to initialize SSR Pass" << std::endl;
        return false;
    }

    // Generate Halton Sequence (Base 2, 3) - 16 samples
    auto halton = [](uint32_t index, uint32_t base) {
        float f = 1.0f;
        float r = 0.0f;
        while (index > 0) {
            f = f / (float)base;
            r = r + f * (float)(index % base);
            index = index / base;
        }
        return r;
    };
    
    jitterSamples.resize(16);
    for (uint32_t i = 0; i < 16; ++i) {
        // Range [-0.5, 0.5]
        jitterSamples[i].x = (halton(i + 1, 2) - 0.5f);
        jitterSamples[i].y = (halton(i + 1, 3) - 0.5f);
    }
    
    // Initialize TAA Pass
    if (!taaPass.Initialize(device, 1280 * 2, 720 * 2, DataFormat::BGRA8_UNorm)) {
        std::cout << "Failed to initialize TAA Pass" << std::endl;
        return false;
    }
    
    // 14. Create SSR Output Texture
    TextureDesc ssrDesc = renderSystem.GetBackBufferDesc();
    if (ssrDesc.size.x == 0) ssrDesc.size = {1280*2, 720*2, 1}; // Fallback
    ssrDesc.format = DataFormat::RGBA8_UNorm; // Use RGBA8 for storage compatibility
    ssrDesc.usage = TextureUsage::UnorderedAccess | TextureUsage::ShaderResource;
    ssrOutputTexture = device->CreateTexture(ssrDesc);
    
    if (ssrOutputTexture == handles::INVALID_RESOURCE) {
        std::cout << "Failed to create SSR Output Texture" << std::endl;
        return false;
    }

    // 14.5 Setup Composite Descriptor Sets
    DescriptorSetDesc compSetDesc;
    compSetDesc.layout = blitDSLayout;

    // Main Composite Set
    mainCompositeDescriptorSet = device->CreateDescriptorSet(compSetDesc);
    {
        DescriptorImageInfo imageInfo{};
        imageInfo.imageView = mainColorTexture;
        imageInfo.sampler = handles::INVALID_SAMPLER;
        imageInfo.imageLayout = ResourceState::ShaderResource;

        WriteDescriptorSet updates[2];
        updates[0].dstSet = mainCompositeDescriptorSet;
        updates[0].dstBinding = 0;
        updates[0].dstArrayElement = 0;
        updates[0].descriptorCount = 1;
        updates[0].descriptorType = DescriptorType::SampledImage;
        updates[0].imageInfo = &imageInfo;

        DescriptorBufferInfo bufInfo{};
        bufInfo.buffer = blitUniformBuffer;
        bufInfo.offset = 0;
        bufInfo.range = sizeof(math::m4x4);

        updates[1].dstSet = mainCompositeDescriptorSet;
        updates[1].dstBinding = 1;
        updates[1].dstArrayElement = 0;
        updates[1].descriptorCount = 1;
        updates[1].descriptorType = DescriptorType::UniformBuffer;
        updates[1].bufferInfo = &bufInfo;

        device->UpdateDescriptorSets(2, updates);
    }

    // SSR Composite Set
    ssrCompositeDescriptorSet = device->CreateDescriptorSet(compSetDesc);
    {
        DescriptorImageInfo imageInfo{};
        imageInfo.imageView = ssrOutputTexture;
        imageInfo.sampler = handles::INVALID_SAMPLER;
        imageInfo.imageLayout = ResourceState::ShaderResource;

        WriteDescriptorSet updates[2];
        updates[0].dstSet = ssrCompositeDescriptorSet;
        updates[0].dstBinding = 0;
        updates[0].dstArrayElement = 0;
        updates[0].descriptorCount = 1;
        updates[0].descriptorType = DescriptorType::SampledImage;
        updates[0].imageInfo = &imageInfo;

        DescriptorBufferInfo bufInfo{};
        bufInfo.buffer = blitUniformBuffer;
        bufInfo.offset = 0;
        bufInfo.range = sizeof(math::m4x4);

        updates[1].dstSet = ssrCompositeDescriptorSet;
        updates[1].dstBinding = 1;
        updates[1].dstArrayElement = 0;
        updates[1].descriptorCount = 1;
        updates[1].descriptorType = DescriptorType::UniformBuffer;
        updates[1].bufferInfo = &bufInfo;

        device->UpdateDescriptorSets(2, updates);
    }

    // 15. Add Reflection Plane to Scene - REMOVED (Handled by ID 101 below)



    
    // 15. Setup Initial Reflection Plane (Mirror Box)
    {
        reflectionPlane.entityId = 101;
        // Logic will be handled in Run() updates
        scene.AddReflectionPlane(reflectionPlane);
    }

    return true;
}

/**
 * @brief Helper to read shader source code from a file.
 * 
 * @param filepath Absolute or relative path to the shader file.
 * @return std::string Content of the shader file, or empty string if failed.
 */

namespace {
    std::string ReadShaderFileRecursive(const std::string& filepath, std::set<std::string>& includedFiles) {
        if (includedFiles.find(filepath) != includedFiles.end()) {
            return "";
        }
        includedFiles.insert(filepath);

        std::string finalPath = filepath;
        std::ifstream file(finalPath);
        if (!file.is_open()) {
            // Try common shader directory
            finalPath = "Engine/Graphics/RHI/Shaders/" + filepath;
            file.open(finalPath);
            if (!file.is_open()) {
                 // Try test shader directory
                 finalPath = "EngineTest/shaders/" + filepath;
                 file.open(finalPath);
                 if (!file.is_open()) {
                    std::cout << "Failed to find shader file: " << filepath << std::endl;
                    return "";
                 }
            }
        }
    
        std::stringstream buffer;
        std::string line;
        while (std::getline(file, line)) {
            if (line.find("#include") != std::string::npos && line.find("\"") != std::string::npos) {
                size_t start = line.find("\"");
                size_t end = line.rfind("\"");
                if (start != std::string::npos && end != std::string::npos && end > start) {
                    std::string includeFile = line.substr(start + 1, end - start - 1);
                    std::string includedSource = ReadShaderFileRecursive(includeFile, includedFiles);
                    if (!includedSource.empty()) {
                        buffer << "\n// Included from " << includeFile << "\n";
                        buffer << includedSource << "\n";
                    }
                } else {
                    buffer << line << "\n";
                }
            } else {
                if (line.find("#pragma once") == std::string::npos) {
                    buffer << line << "\n";
                }
            }
        }
        return buffer.str();
    }
}

std::string MultiViewTestCase::ReadShaderFile(const std::string& filepath) {
    std::set<std::string> includedFiles;
    return ReadShaderFileRecursive(filepath, includedFiles);
}

/**
 * @brief Creates resources specifically for Reflection and Mirror rendering.
 * 
 * Includes:
 * - Reflection Texture and Depth
 * - Reflection and Mirror Pipelines/Shaders
 * - Uniform Buffers for Reflection Camera and Plane
 * 
 * @return true if creation succeeds.
 */
bool MultiViewTestCase::CreateReflectionResources() {
    TextureDesc texDesc;
    texDesc.size = {1280, 720, 1}; // Default size
    texDesc.mipLevels = 1;
    texDesc.arraySize = 1;
    texDesc.type = TextureType::Texture2D;

    texDesc.format = DataFormat::BGRA8_UNorm;
    texDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
    reflectionTexture = device->CreateTexture(texDesc);
    reflectionTexture2 = device->CreateTexture(texDesc);
    reflectionTexture3 = device->CreateTexture(texDesc);
    
    // Depth
    texDesc.format = DataFormat::D32_Float;
    texDesc.usage = TextureUsage::DepthStencil;
    reflectionDepthTexture = device->CreateTexture(texDesc);
    reflectionDepthTexture2 = device->CreateTexture(texDesc);
    reflectionDepthTexture3 = device->CreateTexture(texDesc);
    
    // 2. Buffers
    BufferDesc ubDesc{};
    ubDesc.size = sizeof(math::m4x4) * 6; // Standard View Buffer size
    ubDesc.usage = GPUMemoryUsage::Dynamic;
    ubDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    ubDesc.type = BufferType::Constant;
    ubDesc.bindFlags = static_cast<uint32_t>(BufferUsageFlags::Uniform);
    reflectionUniformBuffer = device->CreateBuffer(ubDesc);
    reflectionUniformBuffer2 = device->CreateBuffer(ubDesc);
    reflectionUniformBuffer3 = device->CreateBuffer(ubDesc);
    
    ubDesc.size = sizeof(math::v4); // Plane Equation
    reflectionPlaneBuffer = device->CreateBuffer(ubDesc);
    reflectionPlaneBuffer2 = device->CreateBuffer(ubDesc);
    reflectionPlaneBuffer3 = device->CreateBuffer(ubDesc);
    
    ubDesc.size = sizeof(SceneData); // Mirror Instance Data
    mirrorUniformBuffer = device->CreateBuffer(ubDesc);
    
    // Initialize Mirror Uniform Buffer
    {
        void* data = device->MapBuffer(mirrorUniformBuffer);
        if (data) {
            SceneData* sceneData = static_cast<SceneData*>(data);
            sceneData->model = primal::graphics::rhi::math::MatrixIdentity();
            sceneData->lightPos = {0.0f, 4.0f, 0.0f, 1.0f};
            sceneData->lightColor = {1.0f, 1.0f, 1.0f, 1.0f};
            sceneData->reflectionPlane = {0.0f, 1.0f, 0.0f, 0.0f}; // Default Plane
            device->UnmapBuffer(mirrorUniformBuffer);
        }
    }
    
    // 3. Shaders
    std::string shaderPath = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/shaders/MultiView.metal"; 
    std::string shaderSource = ReadShaderFile(shaderPath);
    
    reflectionVertexShader = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Vertex, "vertexMainReflection");
    mirrorVertexShader = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Vertex, "vertexMainMirror");
    mirrorPixelShader = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Pixel, "fragmentMainMirror");
    
    if (reflectionVertexShader == handles::INVALID_SHADER || mirrorVertexShader == handles::INVALID_SHADER) {
        std::cout << "CRITICAL: Failed to create Reflection Shaders" << std::endl;
        return false;
    }
    
    // 4. Pipelines & Layouts
    
    // --- Reflection Pipeline ---
    // Layout: Set 0 (Binding 1: View, Binding 2: Scene, Binding 3: Plane)
    DescriptorSetLayoutBinding reflBindings[3];
    reflBindings[0].binding = 1; reflBindings[0].descriptorType = DescriptorType::UniformBuffer; reflBindings[0].descriptorCount = 1; reflBindings[0].stageFlags = ShaderStage::Vertex;
    reflBindings[1].binding = 2; reflBindings[1].descriptorType = DescriptorType::UniformBuffer; reflBindings[1].descriptorCount = 1; reflBindings[1].stageFlags = ShaderStage::Vertex | ShaderStage::Pixel;
    reflBindings[2].binding = 3; reflBindings[2].descriptorType = DescriptorType::UniformBuffer; reflBindings[2].descriptorCount = 1; reflBindings[2].stageFlags = ShaderStage::Vertex;
    
    DescriptorSetLayoutDesc reflLayoutDesc;
    reflLayoutDesc.bindingCount = 3;
    reflLayoutDesc.bindings = reflBindings;
    DescriptorSetLayoutHandle reflDSLayout = device->CreateDescriptorSetLayout(reflLayoutDesc);
    
    PipelineLayoutDesc reflPLDesc;
    reflPLDesc.setLayoutCount = 1;
    reflPLDesc.setLayouts = &reflDSLayout;
    reflectionPipelineLayout = device->CreatePipelineLayout(reflPLDesc);
    
    GraphicsPipelineDesc reflPipeDesc;
    reflPipeDesc.vertexShader = reflectionVertexShader;
    reflPipeDesc.pixelShader = pixelShader; // Reuse standard fragment shader (just lighting)
    reflPipeDesc.layout = reflectionPipelineLayout;
    reflPipeDesc.topology = PrimitiveTopology::TriangleList;
    reflPipeDesc.cullMode = CullMode::None; // Safe
    reflPipeDesc.enableDepthTest = true;
    reflPipeDesc.enableDepthWrite = true;
    reflPipeDesc.depthStencilFormat = DataFormat::D32_Float;
    reflPipeDesc.renderTargetCount = 1;
    reflPipeDesc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
    reflPipeDesc.cullMode = CullMode::None; // Disable culling for reflection passes to see backfaces of walls from outside
    
    // Vertex Input (Standard)
    reflPipeDesc.vertexBindings.push_back({0, sizeof(float)*11, true});
    reflPipeDesc.vertexAttributes.push_back({0, 0, DataFormat::RGB32_Float, 0}); // Pos
    reflPipeDesc.vertexAttributes.push_back({1, 0, DataFormat::RGB32_Float, sizeof(float)*3}); // Normal
    reflPipeDesc.vertexAttributes.push_back({2, 0, DataFormat::RG32_Float, sizeof(float)*6}); // UV
    reflPipeDesc.vertexAttributes.push_back({3, 0, DataFormat::RGB32_Float, sizeof(float)*8}); // Color
    
    reflectionPipeline = device->CreateGraphicsPipeline(reflPipeDesc);
    
    // --- Mirror Pipeline ---
    // Layout: Binding 0: Texture, Binding 1: View, Binding 2: Scene
    DescriptorSetLayoutBinding mirBindings[5];
    mirBindings[0].binding = 1; mirBindings[0].descriptorType = DescriptorType::UniformBuffer; mirBindings[0].descriptorCount = 1; mirBindings[0].stageFlags = ShaderStage::Vertex;
    mirBindings[1].binding = 2; mirBindings[1].descriptorType = DescriptorType::UniformBuffer; mirBindings[1].descriptorCount = 1; mirBindings[1].stageFlags = ShaderStage::Vertex | ShaderStage::Pixel;
    mirBindings[2].binding = 0; mirBindings[2].descriptorType = DescriptorType::SampledImage; mirBindings[2].descriptorCount = 1; mirBindings[2].stageFlags = ShaderStage::Pixel;
    mirBindings[3].binding = 3; mirBindings[3].descriptorType = DescriptorType::SampledImage; mirBindings[3].descriptorCount = 1; mirBindings[3].stageFlags = ShaderStage::Pixel; // Texture 2
    mirBindings[4].binding = 4; mirBindings[4].descriptorType = DescriptorType::SampledImage; mirBindings[4].descriptorCount = 1; mirBindings[4].stageFlags = ShaderStage::Pixel; // Texture 3
    
    DescriptorSetLayoutDesc mirLayoutDesc;
    mirLayoutDesc.bindingCount = 5;
    mirLayoutDesc.bindings = mirBindings;
    DescriptorSetLayoutHandle mirDSLayout = device->CreateDescriptorSetLayout(mirLayoutDesc);
    
    PipelineLayoutDesc mirPLDesc;
    mirPLDesc.setLayoutCount = 1;
    mirPLDesc.setLayouts = &mirDSLayout;
    mirrorPipelineLayout = device->CreatePipelineLayout(mirPLDesc);
    
    GraphicsPipelineDesc mirPipeDesc = reflPipeDesc; // Copy basics
    mirPipeDesc.vertexShader = mirrorVertexShader;
    mirPipeDesc.pixelShader = mirrorPixelShader;
    mirPipeDesc.layout = mirrorPipelineLayout;
    mirPipeDesc.renderTargetFormats[0] = renderSystem.GetBackBufferDesc().format; // Main Pass Format
    mirPipeDesc.cullMode = CullMode::Back; // Restore backface culling for the mirror object itself
    
    mirrorPipeline = device->CreateGraphicsPipeline(mirPipeDesc);
    
    // 5. Create Descriptor Sets
    DescriptorSetDesc setDesc;
    setDesc.layout = reflDSLayout;
    reflectionDescriptorSet = device->CreateDescriptorSet(setDesc);
    reflectionDescriptorSet2 = device->CreateDescriptorSet(setDesc);
    reflectionDescriptorSet3 = device->CreateDescriptorSet(setDesc);
    
    // Create ShortBox Reflection Descriptor Sets
    shortBoxReflDescriptorSet = device->CreateDescriptorSet(setDesc);
    shortBoxReflDescriptorSet2 = device->CreateDescriptorSet(setDesc);
    shortBoxReflDescriptorSet3 = device->CreateDescriptorSet(setDesc);
    
    setDesc.layout = mirDSLayout;
    mirrorDescriptorSet = device->CreateDescriptorSet(setDesc);
    
    // Define SceneData locally - REMOVED (Using global definition)
    // struct SceneData { ... };
    
    // Update Reflection Descriptor Set
    {
        WriteDescriptorSet updates[3];
        DescriptorBufferInfo info1{}; info1.buffer = reflectionUniformBuffer; info1.offset = 0; info1.range = sizeof(math::m4x4)*6;
        DescriptorBufferInfo info2{}; info2.buffer = instanceUniformBuffer; info2.offset = 0; info2.range = sizeof(SceneData);
        DescriptorBufferInfo info3{}; info3.buffer = reflectionPlaneBuffer; info3.offset = 0; info3.range = sizeof(math::v4);
        
        updates[0].dstSet = reflectionDescriptorSet; updates[0].dstBinding = 1; updates[0].descriptorCount = 1; updates[0].descriptorType = DescriptorType::UniformBuffer; updates[0].bufferInfo = &info1;
        updates[1].dstSet = reflectionDescriptorSet; updates[1].dstBinding = 2; updates[1].descriptorCount = 1; updates[1].descriptorType = DescriptorType::UniformBuffer; updates[1].bufferInfo = &info2;
        updates[2].dstSet = reflectionDescriptorSet; updates[2].dstBinding = 3; updates[2].descriptorCount = 1; updates[2].descriptorType = DescriptorType::UniformBuffer; updates[2].bufferInfo = &info3;
        
        device->UpdateDescriptorSets(3, updates);

        // Update ShortBox Reflection Descriptor Set (Uses ShortBox Buffer)
        DescriptorBufferInfo infoShortBox{}; infoShortBox.buffer = shortBoxUniformBuffer; infoShortBox.offset = 0; infoShortBox.range = sizeof(SceneData);
        
        updates[0].dstSet = shortBoxReflDescriptorSet; // View Buffer (Same)
        updates[1].dstSet = shortBoxReflDescriptorSet; updates[1].bufferInfo = &infoShortBox; // ShortBox Buffer
        updates[2].dstSet = shortBoxReflDescriptorSet; // Plane Buffer (Same)
        
        device->UpdateDescriptorSets(3, updates);

        // Update Reflection Descriptor Set 2
        info1.buffer = reflectionUniformBuffer2;
        info3.buffer = reflectionPlaneBuffer2;
        
        updates[0].dstSet = reflectionDescriptorSet2; updates[0].bufferInfo = &info1;
        updates[1].dstSet = reflectionDescriptorSet2; updates[1].bufferInfo = &info2; // Instance buffer shared
        updates[2].dstSet = reflectionDescriptorSet2; updates[2].bufferInfo = &info3;
        
        device->UpdateDescriptorSets(3, updates);

        // Update ShortBox Reflection Descriptor Set 2
        updates[0].dstSet = shortBoxReflDescriptorSet2; // View Buffer 2
        updates[1].dstSet = shortBoxReflDescriptorSet2; updates[1].bufferInfo = &infoShortBox; // ShortBox Buffer
        updates[2].dstSet = shortBoxReflDescriptorSet2; // Plane Buffer 2
        
        device->UpdateDescriptorSets(3, updates);
        
        // Update Reflection Descriptor Set 3
        info1.buffer = reflectionUniformBuffer3;
        info3.buffer = reflectionPlaneBuffer3;
        
        updates[0].dstSet = reflectionDescriptorSet3; updates[0].bufferInfo = &info1;
        updates[1].dstSet = reflectionDescriptorSet3; updates[1].bufferInfo = &info2; // Instance buffer shared
        updates[2].dstSet = reflectionDescriptorSet3; updates[2].bufferInfo = &info3;
        
        device->UpdateDescriptorSets(3, updates);

        // Update ShortBox Reflection Descriptor Set 3
        updates[0].dstSet = shortBoxReflDescriptorSet3; // View Buffer 3
        updates[1].dstSet = shortBoxReflDescriptorSet3; updates[1].bufferInfo = &infoShortBox; // ShortBox Buffer
        updates[2].dstSet = shortBoxReflDescriptorSet3; // Plane Buffer 3
        
        device->UpdateDescriptorSets(3, updates);
    }
    
    // Update Mirror Descriptor Set
    {
        WriteDescriptorSet updates[5];
        DescriptorBufferInfo info1{}; info1.buffer = mainViewUniformBuffer; info1.offset = 0; info1.range = sizeof(math::m4x4)*6;
        DescriptorBufferInfo info2{}; info2.buffer = mirrorUniformBuffer; info2.offset = 0; info2.range = sizeof(SceneData); // Use Mirror Buffer
        DescriptorImageInfo infoTex{}; infoTex.imageView = reflectionTexture; infoTex.sampler = handles::INVALID_RESOURCE; // Metal uses inline sampler
        DescriptorImageInfo infoTex2{}; infoTex2.imageView = reflectionTexture2; infoTex2.sampler = handles::INVALID_RESOURCE;
        DescriptorImageInfo infoTex3{}; infoTex3.imageView = reflectionTexture3; infoTex3.sampler = handles::INVALID_RESOURCE;
        
        updates[0].dstSet = mirrorDescriptorSet; updates[0].dstBinding = 1; updates[0].descriptorCount = 1; updates[0].descriptorType = DescriptorType::UniformBuffer; updates[0].bufferInfo = &info1;
        updates[1].dstSet = mirrorDescriptorSet; updates[1].dstBinding = 2; updates[1].descriptorCount = 1; updates[1].descriptorType = DescriptorType::UniformBuffer; updates[1].bufferInfo = &info2;
        updates[2].dstSet = mirrorDescriptorSet; updates[2].dstBinding = 0; updates[2].descriptorCount = 1; updates[2].descriptorType = DescriptorType::SampledImage; updates[2].imageInfo = &infoTex;
        updates[3].dstSet = mirrorDescriptorSet; updates[3].dstBinding = 3; updates[3].descriptorCount = 1; updates[3].descriptorType = DescriptorType::SampledImage; updates[3].imageInfo = &infoTex2;
        updates[4].dstSet = mirrorDescriptorSet; updates[4].dstBinding = 4; updates[4].descriptorCount = 1; updates[4].descriptorType = DescriptorType::SampledImage; updates[4].imageInfo = &infoTex3;
        
        device->UpdateDescriptorSets(5, updates);
    }
    
    return true;
}

/**
 * @brief Main Render Loop.
 * 
 * Executes per frame:
 * 1. Updates Scene Logic (Lights, Reflection Plane).
 * 2. Builds RenderGraph:
 *    - Reflection Pass: Renders scene from reflection camera.
 *    - MultiView Pass: Renders scene to CubeMap.
 *    - Main Pass: Renders scene from main camera (with Mirror).
 *    - SSR Pass: Computes screen-space reflections.
 *    - Composite Pass: Blends Main, SSR, and Debug Overlay.
 * 3. Compiles and Executes RenderGraph.
 */
void MultiViewTestCase::Run() {
    if (!window.is_valid()) return;
    // std::cout << "DEBUG: RUNNING MULTI-VIEW TEST (RenderGraph)" << std::endl;
    
    ResourceHandle backBuffer;
    SyncHandle fence;
    
    // Animate Light
    static float time = 0.0f;
    time += 0.02f;

    // TAA Jitter Management
    static uint32_t frameIndex = 0;
    math::v2 currentJitter = jitterSamples[frameIndex % 16];
    math::v2 previousJitter = jitterSamples[(frameIndex + 15) % 16]; // Previous frame jitter
    
    // Scale jitter by resolution (e.g. 1/Width, 1/Height)
    // For 1280x720, pixel size is ~0.00078, ~0.00138
    // TAA Jitter is typically < 1 pixel
    math::v2 jitterScale = { 1.0f / 1280.0f, 1.0f / 720.0f }; // Should match Render Target size
    math::v2 appliedJitter = { currentJitter.x * jitterScale.x, currentJitter.y * jitterScale.y };
    math::v2 appliedPrevJitter = { previousJitter.x * jitterScale.x, previousJitter.y * jitterScale.y };

    static int debugMode = 0; // 0: Off, 1: ScreenUV, 2: MotionVectors
    static bool useComputedSH = false;

    {
        // Toggle Debug Overlay with F1
        using namespace primal::input;
        input_value val;
        get(std::hash<std::string>()("debug_toggle"), val);
        
        static bool wasF1Down = false;
        bool isF1Down = (val.current.x > 0.0f);
        
        if (isF1Down && !wasF1Down) {
            debugMode = (debugMode + 1) % 3;
            std::cout << "Debug Mode: " << debugMode << " (0: Off, 1: ScreenUV, 2: MotionVectors)" << std::endl;
        }
        wasF1Down = isF1Down;

        // Toggle SH Mode with F2
        input_value valF2;
        get(std::hash<std::string>()("sh_toggle"), valF2); // Assuming "sh_toggle" mapped to F2 or add mapping
        // If not mapped, we can check key code directly if input system allows, or just map it.
        // Input system uses hashing. Let's map F2.
        
        static bool wasF2Down = false;
        bool isF2Down = (valF2.current.x > 0.0f); // Need to bind F2 first
        
        if (isF2Down && !wasF2Down) {
            useComputedSH = !useComputedSH;
            std::cout << "SH Mode: " << (useComputedSH ? "Computed (Skybox)" : "Manual (Cornell Box)") << std::endl;
        }
        wasF2Down = isF2Down;
        
        // Define struct again or move to header (local is fine) - REMOVED (Using global definition)
        // struct SceneData { ... };

        static m4x4 s_previousModel = primal::graphics::rhi::math::MatrixIdentity();

        void* data = device->MapBuffer(instanceUniformBuffer);
        if (data) {
            SceneData* sceneData = static_cast<SceneData*>(data);
            
            // Move Light in circle
            sceneData->lightPos.x = sin(time) * 3.0f;
            sceneData->lightPos.z = cos(time) * 3.0f;
            sceneData->lightPos.y = 4.0f + sin(time * 0.5f); // Up/Down slightly
            
            // Change Color (RGB Cycle) - Make it softer/brighter
            sceneData->lightColor.x = (sin(time) * 0.3f + 0.7f);
            sceneData->lightColor.y = (sin(time + 2.09f) * 0.3f + 0.7f);
            sceneData->lightColor.z = (sin(time + 4.18f) * 0.3f + 0.7f);

            // Pass Debug Flag in lightColor.w
            sceneData->lightColor.w = (float)debugMode;


            sceneData->reflectionPlane = {0.0f, 0.0f, 0.0f, 0.0f}; // Unused for standard objects
            sceneData->reflectionPlane2 = {0.0f, 0.0f, 0.0f, 0.0f};
            sceneData->reflectionPlane3 = {0.0f, 0.0f, 0.0f, 0.0f};
            
            // TAA Jitter Update for Scene Data (Vertex Shader Jitter)
            sceneData->jitter = appliedJitter;
            sceneData->previousJitter = appliedPrevJitter;
            
            // Store previous model (Identity for now for main scene objects as they don't move)
            sceneData->previousModel = s_previousModel;
            s_previousModel = sceneData->model;

            device->UnmapBuffer(instanceUniformBuffer);
        }
    }
    
    // Update ShortBox Uniform Buffer (Animation + SH)
    if (shortBoxUniformBuffer != primal::graphics::rhi::handles::INVALID_RESOURCE) {
        void* data = device->MapBuffer(shortBoxUniformBuffer);
        if (data) {
            SceneData* sd = static_cast<SceneData*>(data);
            
            // Sync Light from Main Scene (re-calculate as we don't have easy access to instanceUniformBuffer data here)
            sd->lightPos.x = sin(time) * 3.0f;
            sd->lightPos.z = cos(time) * 3.0f;
            sd->lightPos.y = 4.0f + sin(time * 0.5f);
            sd->lightColor.x = (sin(time) * 0.3f + 0.7f);
            sd->lightColor.y = (sin(time + 2.09f) * 0.3f + 0.7f);
            sd->lightColor.z = (sin(time + 4.18f) * 0.3f + 0.7f);
            sd->lightColor.w = (float)debugMode;
            
            // Animate ShortBox (Up/Down)
            float yOffset = std::sin(time * 2.0f) * 0.5f;
            
            // Reconstruct Transform (since geometry is now at origin)
            // Fix Clipping: Raise basePos.y from -3.5f to -3.0f so min Y = -3.0 - 0.5 - 1.5 = -5.0 (Floor level)
            v3 basePos = {-2.0f, -3.0f, -1.0f};
            m4x4 rot = CreateRotationY(0.3f);
            m4x4 trans = CreateTranslation(v3{basePos.x, basePos.y + yOffset, basePos.z});
            
            sd->model = MatrixMultiply(trans, rot);
            sd->previousModel = sd->model;
            
            sd->jitter = appliedJitter;
            sd->previousJitter = appliedPrevJitter;
            
            if (useComputedSH) {
                // Use Computed SH from Skybox
                for(int i=0; i<9; ++i) {
                    sd->shCoeffs[i] = computedSH[i];
                }
            } else {
                // Set SH Coefficients (Approximating Cornell Box Environment)
                // Left Wall: Red, Right Wall: Green, Ceiling: White Light, Floor: White, Back: White
                for(int i=0; i<9; ++i) sd->shCoeffs[i] = {0,0,0,0};
                
                // L0,0 - Ambient: Average of all walls (Warm White base)
                sd->shCoeffs[0] = {0.4f, 0.4f, 0.4f, 1.0f}; 
                
                // L1,-1 - Y Gradient: Top is Bright Light (+), Bottom is Shadowed (-)
                sd->shCoeffs[1] = {0.2f, 0.2f, 0.2f, 1.0f}; 
                
                // L1,0 - Z Gradient: Back Wall (-Z) is White/Bright, Front (+Z) is Open/Skybox (Darker)
                // So +Z direction gets negative coefficient (darker), -Z gets positive (brighter)
                sd->shCoeffs[2] = {-0.1f, -0.1f, -0.1f, 1.0f};
                
                // L1,1 - X Gradient: Right (+X) is Green, Left (-X) is Red
                // Normal +X: Base + Coeff -> Should be Greenish (-Red, +Green)
                // Normal -X: Base - Coeff -> Should be Reddish (+Red, -Green)
                // So Coeff should be {-Red, +Green, 0}
                sd->shCoeffs[3] = {-0.3f, 0.3f, 0.0f, 1.0f};
            }
            
            device->UnmapBuffer(shortBoxUniformBuffer);
        }
    }

    // ---------------------------------------------------------
    // REFLECTION UPDATE
    // ---------------------------------------------------------
    {
        // 1. Calculate Mirror Plane
        // MirrorBox Geometry is at Origin. We apply Model Matrix here.
        // Target Position: (2.0, -2.0, 0.0)
        v3 mirrorPos = v3{2.0f, -2.0f, 0.0f}; 
        float angle = -0.8f; // Rotate to ~45 degrees to show two faces
        
        float c = cos(angle);
        float s = sin(angle);
        
        // Normal calculation:
        // In addBox, w (Z-axis) is defined as {s, 0, c}.
        // The front face is in the +w direction.
        v3 planeNormal = v3{s, 0.0f, c};
        planeNormal = Normalize(planeNormal); 
        
        // Point on plane:
        // Front face center is at Center + HalfSize.z * Normal
        // HalfSize.z = 1.5f (since full Z size is 3.0f)
        v3 planePoint = mirrorPos + planeNormal * 1.5f; 
        
        // Second Plane (Right Face, Local +X)
        // Normal 2: (c, 0, -s) -> Rotated +X
        // This direction corresponds to the Right Face of the Mirror Box.
        // We use this normal for the Reflection Camera and the Shader Plane Check.
        v3 planeNormal2 = v3{c, 0.0f, -s}; 
        planeNormal2 = Normalize(planeNormal2);
        v3 planePoint2 = mirrorPos + planeNormal2 * 1.5f;

        // Update Reflection Plane Struct
        reflectionPlane.position = planePoint;
        reflectionPlane.normal = planeNormal;
        scene.UpdateReflectionPlane(reflectionPlane.entityId, reflectionPlane);

        // Plane Eq: dot(N, P) + D = 0 => D = -dot(N, P)
        float planeD = -Dot(reflectionPlane.normal, reflectionPlane.position);
        v4 planeEq = {reflectionPlane.normal.x, reflectionPlane.normal.y, reflectionPlane.normal.z, planeD};
        
        // Plane Eq 2
        float planeD2 = -Dot(planeNormal2, planePoint2);
        v4 planeEq2 = {planeNormal2.x, planeNormal2.y, planeNormal2.z, planeD2};

        // 3. Reflection Plane 3 (Top Face)
        // Normal: (0, 1, 0)
        // Point: Center + (0, 1.5, 0) -> Correct Half Height
        // Fixed: Previously using 3.0f offset which was incorrect.
        v3 planeNormal3 = v3{0.0f, 1.0f, 0.0f};
        v3 planePoint3 = mirrorPos + v3{0.0f, 1.5f, 0.0f}; 

        float planeD3 = -Dot(planeNormal3, planePoint3);
        v4 planeEq3 = {planeNormal3.x, planeNormal3.y, planeNormal3.z, planeD3};

        // Update Plane Buffer
        void* data = device->MapBuffer(reflectionPlaneBuffer);
        if (data) {
            memcpy(data, &planeEq, sizeof(v4));
            device->UnmapBuffer(reflectionPlaneBuffer);
        }
        
        // Update Plane Buffer 2
        data = device->MapBuffer(reflectionPlaneBuffer2);
        if (data) {
            memcpy(data, &planeEq2, sizeof(v4));
            device->UnmapBuffer(reflectionPlaneBuffer2);
        }

        // Update Plane Buffer 3
        data = device->MapBuffer(reflectionPlaneBuffer3);
        if (data) {
            memcpy(data, &planeEq3, sizeof(v4));
            device->UnmapBuffer(reflectionPlaneBuffer3);
        }
        
        // Update Mirror Uniform Buffer (for Shader Masking)
        data = device->MapBuffer(mirrorUniformBuffer);
        if (data) {
            SceneData* sceneData = static_cast<SceneData*>(data);
            
            // Calculate Model Matrix: Translation * Rotation
            // Note: Metal uses Column-Major. Multiply order depends on library.
            // We want Translate * Rotate * v.
            m4x4 rot = CreateRotationY(angle);
            m4x4 trans = CreateTranslation(mirrorPos);
            sceneData->model = MatrixMultiply(trans, rot);
            
            // Sync Light
            sceneData->lightPos.x = sin(time) * 3.0f;
            sceneData->lightPos.z = cos(time) * 3.0f;
            sceneData->lightPos.y = 4.0f + sin(time * 0.5f);
            sceneData->lightColor.x = (sin(time) * 0.3f + 0.7f);
            sceneData->lightColor.y = (sin(time + 2.09f) * 0.3f + 0.7f);
            sceneData->lightColor.z = (sin(time + 4.18f) * 0.3f + 0.7f);
            
            sceneData->reflectionPlane = planeEq;
            sceneData->reflectionPlane2 = planeEq2;
            sceneData->reflectionPlane3 = planeEq3;
            
            device->UnmapBuffer(mirrorUniformBuffer);
        }
        
        // 2. Calculate Reflection Camera
        // Main Camera (Fixed)
        v3 eye = v3{0.0f, 0.0f, 18.0f}; 
        v3 center = v3{0.0f, 0.0f, 0.0f};
        v3 up = v3{0.0f, 1.0f, 0.0f};
        
        // Reflection Function
        auto Reflect = [&](v3 p) {
            float dist = Dot(p, reflectionPlane.normal) + planeD;
            return p - reflectionPlane.normal * (2.0f * dist);
        };
        
        v3 eyeRefl = Reflect(eye);
        v3 centerRefl = Reflect(center);
        
        // Reflect Up Vector (Direction only)
        // R_dir = Dir - 2 * dot(Dir, N) * N
        float upDot = Dot(up, reflectionPlane.normal);
        v3 upRefl = up - reflectionPlane.normal * (2.0f * upDot);
        
        m4x4 viewRefl = CreateLookAt(eyeRefl, centerRefl, upRefl);
        
        // Projection (Same as main)
        float aspect = (float)renderSystem.GetBackBufferDesc().size.x / (float)renderSystem.GetBackBufferDesc().size.y;
        if (aspect < 0.1f) aspect = 1280.0f / 720.0f;
        m4x4 projRefl = CreatePerspective(math::constants::PI / 4.0f, aspect, 0.1f, 100.0f);
        
        // Update Reflection View Buffer
        data = device->MapBuffer(reflectionUniformBuffer);
        if (data) {
            m4x4* matrices = static_cast<m4x4*>(data);
            matrices[0] = projRefl * viewRefl;
            device->UnmapBuffer(reflectionUniformBuffer);
        }
        
        // Reflection Camera 2
        v3 eyeRefl2 = eye - planeNormal2 * (2.0f * (Dot(eye, planeNormal2) + planeD2));
        v3 centerRefl2 = center - planeNormal2 * (2.0f * (Dot(center, planeNormal2) + planeD2));
        
        float upDot2 = Dot(up, planeNormal2);
        v3 upRefl2 = up - planeNormal2 * (2.0f * upDot2);
        
        m4x4 viewRefl2 = CreateLookAt(eyeRefl2, centerRefl2, upRefl2);
        
        // Update Reflection View Buffer 2
        data = device->MapBuffer(reflectionUniformBuffer2);
        if (data) {
            m4x4* matrices = static_cast<m4x4*>(data);
            matrices[0] = projRefl * viewRefl2;
            device->UnmapBuffer(reflectionUniformBuffer2);
        }

        // 3. Reflection Plane 3 (Top Face)
        // (Moved calculation up)

        // Reflection Camera 3
        v3 eyeRefl3 = eye - planeNormal3 * (2.0f * (Dot(eye, planeNormal3) + planeD3));
        v3 centerRefl3 = center - planeNormal3 * (2.0f * (Dot(center, planeNormal3) + planeD3));
        v3 upRefl3 = up - planeNormal3 * (2.0f * Dot(up, planeNormal3));
        
        // Ensure Up vector is not parallel to View Direction (Eye -> Center)
        // Eye is (0,0,18), Center is (0,0,0). Direction is -Z.
        // UpRefl is -Y (since Normal is Y).
        // So they are perpendicular. No fix needed.

        m4x4 viewRefl3 = CreateLookAt(eyeRefl3, centerRefl3, upRefl3);

        // Update Reflection View Buffer 3
        data = device->MapBuffer(reflectionUniformBuffer3);
        if (data) {
            m4x4* matrices = static_cast<m4x4*>(data);
            matrices[0] = projRefl * viewRefl3;
            device->UnmapBuffer(reflectionUniformBuffer3);
        }
    }

    // Update Main View Camera (Fixed External View)
    {
        // 1. Calculate Jitter
        frameCount++;
        int jitterIndex = frameCount % 16;
        float jitterX = (Halton(jitterIndex + 1, 2) - 0.5f);
        float jitterY = (Halton(jitterIndex + 1, 3) - 0.5f);
        
        float texelWidth = 1.0f / (float)renderSystem.GetBackBufferDesc().size.x;
        float texelHeight = 1.0f / (float)renderSystem.GetBackBufferDesc().size.y;
        
        // Jitter in Clip Space
        // Halton gives [-0.5, 0.5]. We map to pixel offset.
        // Clip Space = PixelOffset * 2.0 / Resolution.
        v2 jitter = {jitterX * 2.0f * texelWidth, jitterY * 2.0f * texelHeight};
        
        // Update Global SceneData Jitter
        {
             void* data = device->MapBuffer(instanceUniformBuffer);
             if (data) {
                 SceneData* sceneData = static_cast<SceneData*>(data);
                 sceneData->jitter = jitter;
                 device->UnmapBuffer(instanceUniformBuffer);
             }
        }

        void* data = device->MapBuffer(mainViewUniformBuffer);
        if (data) {
            m4x4* matrices = static_cast<m4x4*>(data);
            
            // Perspective (Correct Aspect Ratio)
            float aspect = (float)renderSystem.GetBackBufferDesc().size.x / (float)renderSystem.GetBackBufferDesc().size.y;
            if (aspect < 0.1f) aspect = 1280.0f / 720.0f; // Fallback
            
            // Use narrower FOV (45 degrees)
            m4x4 proj = CreatePerspective(math::constants::PI / 4.0f, aspect, 0.1f, 100.0f);
            
            // Apply Jitter to Projection Matrix
            m4x4 jitteredProj = proj;
            jitteredProj.columns[2][0] += jitter.x;
            jitteredProj.columns[2][1] += jitter.y;
            
            v3 eye = primal::math::v3{0, 0, 18.0f}; 
            v3 center = primal::math::v3{0, 0, 0};
            v3 up = primal::math::v3{0, 1, 0};
            
            m4x4 view = CreateLookAt(eye, center, up);
            
            // Set Jittered VP into index 0
            matrices[0] = jitteredProj * view;
            
            // Initialize Previous VP
            if (frameCount == 1) {
                previousViewProjection = proj * view;
            }
            
            // Set Previous Unjittered VP into index 6
            matrices[6] = previousViewProjection;
            
            // Update Previous VP for next frame
            previousViewProjection = proj * view;
            
            device->UnmapBuffer(mainViewUniformBuffer);
        }
    }

    if (!renderSystem.BeginFrame(backBuffer, fence)) return;
    
    // Reset Graph
    renderGraph->Clear();
    
    // Import BackBuffer to Graph
    TextureDesc backBufferDesc = renderSystem.GetBackBufferDesc();
    
    // DEBUG: Force Correct Size if suspect
    if (backBufferDesc.size.x == 0 || backBufferDesc.size.y == 0) {
        std::cout << "CRITICAL WARNING: backBufferDesc size is 0! Forcing to window info." << std::endl;
        backBufferDesc.size.x = 2560; // Assume Retina
        backBufferDesc.size.y = 1440;
    }
    std::cout << "DEBUG: BackBuffer Size: " << backBufferDesc.size.x << "x" << backBufferDesc.size.y << std::endl;
    std::cout << "DEBUG: BackBuffer Format: " << (int)backBufferDesc.format << std::endl;

    RGResourceHandle rgBackBuffer = renderGraph->ImportTexture("BackBuffer", backBuffer, backBufferDesc);
    renderGraph->MarkAsOutput(rgBackBuffer);
    
    // Pass 0: Reflection Pass
    struct ReflectionPassData {
        RGResourceHandle output;
        RGResourceHandle depth;
    };
    
    // Import Reflection Resources
    TextureDesc reflDesc = renderSystem.GetBackBufferDesc();
    if (reflDesc.size.x == 0) reflDesc.size = {1280, 720, 1};
    reflDesc.size.x /= 2;
    reflDesc.size.y /= 2;
    reflDesc.format = DataFormat::BGRA8_UNorm;
    reflDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
    
    RGResourceHandle rgReflectionTex = renderGraph->ImportTexture("ReflectionTex", reflectionTexture, reflDesc);
    
    TextureDesc reflDepthDesc = reflDesc;
    reflDepthDesc.format = DataFormat::D32_Float;
    reflDepthDesc.usage = TextureUsage::DepthStencil;
    RGResourceHandle rgReflectionDepth = renderGraph->ImportTexture("ReflectionDepth", reflectionDepthTexture, reflDepthDesc);

    auto& reflData = renderGraph->AddPass<ReflectionPassData>("ReflectionPass", RGPassType::Graphics, RGPassCategory::Main,
        [&](ReflectionPassData& data, RenderGraphBuilder& builder) {
            data.output = builder.Write(rgReflectionTex);
            data.depth = builder.Write(rgReflectionDepth);
            
            RGRenderPassDesc rpDesc;
            
            RGAttachmentDesc colorAtt;
            colorAtt.texture = data.output;
            colorAtt.loadOp = LoadAction::Clear;
            colorAtt.storeOp = StoreAction::Store;
            colorAtt.clearColor = ClearValue{math::v4{0.1f, 0.1f, 0.1f, 0.0f}};
            rpDesc.colors.push_back(colorAtt);
            
            RGAttachmentDesc depthAtt;
            depthAtt.texture = data.depth;
            depthAtt.loadOp = LoadAction::Clear;
            depthAtt.storeOp = StoreAction::Store;
            depthAtt.clearDepth = 1.0f;
            rpDesc.depthStencil = depthAtt;
            
            builder.DeclareRenderPass(rpDesc);
        },
        [&](const ReflectionPassData&, RenderGraphContext& context) {
            RHICommandBuffer* cmd = context.cmdBuffer;
            
            // Check desc again or capture
            uint32_t w = renderSystem.GetBackBufferDesc().size.x / 2;
            uint32_t h = renderSystem.GetBackBufferDesc().size.y / 2;
            
            rhi::ViewportDesc viewport;
            viewport.topLeft = {0, 0};
            viewport.size = {(float)w, (float)h};
            viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;
            cmd->SetViewport(viewport);
            
            rhi::Rect scissor;
            scissor.offset = {0, 0};
            scissor.extent = {w, h};
            cmd->SetScissor(scissor);
            
            cmd->BindGraphicsPipeline(reflectionPipeline);
            
            const DescriptorSetHandle sets[] = { reflectionDescriptorSet };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, reflectionPipelineLayout, 0, 1, sets, 0, nullptr);
            
            uint64_t offsets[] = {0};
            cmd->BindVertexBuffers(0, 1, &vertexBuffer, offsets);
            cmd->BindIndexBuffer(indexBuffer, DataFormat::R32_UInt, 0);
            
            for (auto& [name, range] : drawRanges) {
                if (name == "MirrorBox") continue; // Don't render mirror in reflection

                if (name == "Skybox") {
                    cmd->BindGraphicsPipeline(skyboxPipeline);
                    const DescriptorSetHandle sets[] = { skyboxReflDescriptorSet1 };
                    cmd->BindDescriptorSets(PipelineBindPoint::Graphics, skyboxPipelineLayout, 0, 1, sets, 0, nullptr);
                } else if (name == "ShortBox") {
                    cmd->BindGraphicsPipeline(reflectionPipeline);
                    const DescriptorSetHandle sets[] = { shortBoxReflDescriptorSet };
                    cmd->BindDescriptorSets(PipelineBindPoint::Graphics, reflectionPipelineLayout, 0, 1, sets, 0, nullptr);
                } else {
                    cmd->BindGraphicsPipeline(reflectionPipeline);
                    const DescriptorSetHandle sets[] = { reflectionDescriptorSet };
                    cmd->BindDescriptorSets(PipelineBindPoint::Graphics, reflectionPipelineLayout, 0, 1, sets, 0, nullptr);
                }
                cmd->DrawIndexed(range.count, range.start, 0, 1, 0);
            }
            
            // Draw PBR Sphere
            if (pbrPipeline != handles::INVALID_PIPELINE && sphereIndexCount > 0) {
                cmd->BindGraphicsPipeline(pbrPipeline);
                
                // Bind PBR Material Set
                const DescriptorSetHandle pbrSets[] = { pbrMaterialInstance->GetDescriptorSet() };
                cmd->BindDescriptorSets(PipelineBindPoint::Graphics, pbrPipelineLayout, 0, 1, pbrSets, 0, nullptr);
                
                // Bind Sphere Mesh
                uint64_t sphereOffsets[] = {0};
                cmd->BindVertexBuffers(0, 1, &sphereVertexBuffer, sphereOffsets);
                cmd->BindIndexBuffer(sphereIndexBuffer, DataFormat::R32_UInt, 0);
                
                cmd->DrawIndexed(sphereIndexCount, 0, 0, 1, 0);
            }
        }
    );
    
    struct MultiViewPassData {
        RGResourceHandle cubeMap;
    };
    
    // Pass 0.5: Reflection Pass 2
    RGResourceHandle rgReflectionTex2 = renderGraph->ImportTexture("ReflectionTex2", reflectionTexture2, reflDesc);
    RGResourceHandle rgReflectionDepth2 = renderGraph->ImportTexture("ReflectionDepth2", reflectionDepthTexture2, reflDepthDesc);
    
    auto& reflData2 = renderGraph->AddPass<ReflectionPassData>("ReflectionPass2", RGPassType::Graphics, RGPassCategory::Main,
        [&](ReflectionPassData& data, RenderGraphBuilder& builder) {
            data.output = builder.Write(rgReflectionTex2);
            data.depth = builder.Write(rgReflectionDepth2);
            
            RGRenderPassDesc rpDesc;
            
            RGAttachmentDesc colorAtt;
            colorAtt.texture = data.output;
            colorAtt.loadOp = LoadAction::Clear;
            colorAtt.storeOp = StoreAction::Store;
            colorAtt.clearColor = ClearValue{math::v4{0.1f, 0.1f, 0.1f, 0.0f}};
            rpDesc.colors.push_back(colorAtt);
            
            RGAttachmentDesc depthAtt;
            depthAtt.texture = data.depth;
            depthAtt.loadOp = LoadAction::Clear;
            depthAtt.storeOp = StoreAction::Store;
            depthAtt.clearDepth = 1.0f;
            rpDesc.depthStencil = depthAtt;
            
            builder.DeclareRenderPass(rpDesc);
        },
        [&](const ReflectionPassData&, RenderGraphContext& context) {
            RHICommandBuffer* cmd = context.cmdBuffer;
            
            uint32_t w = renderSystem.GetBackBufferDesc().size.x / 2;
            uint32_t h = renderSystem.GetBackBufferDesc().size.y / 2;
            
            rhi::ViewportDesc viewport;
            viewport.topLeft = {0, 0};
            viewport.size = {(float)w, (float)h};
            viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;
            cmd->SetViewport(viewport);
            
            rhi::Rect scissor;
            scissor.offset = {0, 0};
            scissor.extent = {w, h};
            cmd->SetScissor(scissor);
            
            cmd->BindGraphicsPipeline(reflectionPipeline);
            
            const DescriptorSetHandle sets[] = { reflectionDescriptorSet2 };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, reflectionPipelineLayout, 0, 1, sets, 0, nullptr);
            
            uint64_t offsets[] = {0};
            cmd->BindVertexBuffers(0, 1, &vertexBuffer, offsets);
            cmd->BindIndexBuffer(indexBuffer, DataFormat::R32_UInt, 0);
            
            for (auto& [name, range] : drawRanges) {
                if (name == "MirrorBox") continue; 

                if (name == "Skybox") {
                    cmd->BindGraphicsPipeline(skyboxPipeline);
                    const DescriptorSetHandle sets[] = { skyboxReflDescriptorSet2 };
                    cmd->BindDescriptorSets(PipelineBindPoint::Graphics, skyboxPipelineLayout, 0, 1, sets, 0, nullptr);
                } else if (name == "ShortBox") {
                    cmd->BindGraphicsPipeline(reflectionPipeline);
                    const DescriptorSetHandle sets[] = { shortBoxReflDescriptorSet2 };
                    cmd->BindDescriptorSets(PipelineBindPoint::Graphics, reflectionPipelineLayout, 0, 1, sets, 0, nullptr);
                } else {
                    cmd->BindGraphicsPipeline(reflectionPipeline);
                    const DescriptorSetHandle sets[] = { reflectionDescriptorSet2 };
                    cmd->BindDescriptorSets(PipelineBindPoint::Graphics, reflectionPipelineLayout, 0, 1, sets, 0, nullptr);
                }
                cmd->DrawIndexed(range.count, range.start, 0, 1, 0);
            }

            // Draw PBR Sphere
            if (pbrPipeline != handles::INVALID_PIPELINE && pbrMaterialInstance) {
                cmd->BindGraphicsPipeline(pbrPipeline);
                
                // Bind PBR Material Descriptor Set
                DescriptorSetHandle pbrSet = pbrMaterialInstance->GetDescriptorSet();
                if (pbrSet != handles::INVALID_DESCRIPTOR_SET) {
                    const DescriptorSetHandle pbrSets[] = { pbrSet };
                    cmd->BindDescriptorSets(PipelineBindPoint::Graphics, pbrPipelineLayout, 0, 1, pbrSets, 0, nullptr);
                }
                
                uint64_t offsets[] = {0};
                cmd->BindVertexBuffers(0, 1, &sphereVertexBuffer, offsets);
                cmd->BindIndexBuffer(sphereIndexBuffer, DataFormat::R32_UInt, 0);
                
                // Draw Sphere (1 Instance)
                cmd->DrawIndexed(sphereIndexCount, 0, 0, 1, 0);
            }
        }
    );

    // Pass 0.7: Reflection Pass 3
    RGResourceHandle rgReflectionTex3 = renderGraph->ImportTexture("ReflectionTex3", reflectionTexture3, reflDesc);
    RGResourceHandle rgReflectionDepth3 = renderGraph->ImportTexture("ReflectionDepth3", reflectionDepthTexture3, reflDepthDesc);
    
    auto& reflData3 = renderGraph->AddPass<ReflectionPassData>("ReflectionPass3", RGPassType::Graphics, RGPassCategory::Main,
        [&](ReflectionPassData& data, RenderGraphBuilder& builder) {
            data.output = builder.Write(rgReflectionTex3);
            data.depth = builder.Write(rgReflectionDepth3);
            
            RGRenderPassDesc rpDesc;
            
            RGAttachmentDesc colorAtt;
            colorAtt.texture = data.output;
            colorAtt.loadOp = LoadAction::Clear;
            colorAtt.storeOp = StoreAction::Store;
            colorAtt.clearColor = ClearValue{math::v4{0.1f, 0.1f, 0.1f, 0.0f}};
            rpDesc.colors.push_back(colorAtt);
            
            RGAttachmentDesc depthAtt;
            depthAtt.texture = data.depth;
            depthAtt.loadOp = LoadAction::Clear;
            depthAtt.storeOp = StoreAction::Store;
            depthAtt.clearDepth = 1.0f;
            rpDesc.depthStencil = depthAtt;
            
            builder.DeclareRenderPass(rpDesc);
        },
        [&](const ReflectionPassData&, RenderGraphContext& context) {
            RHICommandBuffer* cmd = context.cmdBuffer;
            
            uint32_t w = renderSystem.GetBackBufferDesc().size.x / 2;
            uint32_t h = renderSystem.GetBackBufferDesc().size.y / 2;
            
            rhi::ViewportDesc viewport;
            viewport.topLeft = {0, 0};
            viewport.size = {(float)w, (float)h};
            viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;
            cmd->SetViewport(viewport);
            
            rhi::Rect scissor;
            scissor.offset = {0, 0};
            scissor.extent = {w, h};
            cmd->SetScissor(scissor);
            
            cmd->BindGraphicsPipeline(reflectionPipeline);
            
            const DescriptorSetHandle sets[] = { reflectionDescriptorSet3 };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, reflectionPipelineLayout, 0, 1, sets, 0, nullptr);
            
            uint64_t offsets[] = {0};
            cmd->BindVertexBuffers(0, 1, &vertexBuffer, offsets);
            cmd->BindIndexBuffer(indexBuffer, DataFormat::R32_UInt, 0);
            
            for (auto& [name, range] : drawRanges) {
                if (name == "MirrorBox") continue; 

                if (name == "Skybox") {
                    cmd->BindGraphicsPipeline(skyboxPipeline);
                    const DescriptorSetHandle sets[] = { skyboxReflDescriptorSet3 };
                    cmd->BindDescriptorSets(PipelineBindPoint::Graphics, skyboxPipelineLayout, 0, 1, sets, 0, nullptr);
                } else if (name == "ShortBox") {
                    cmd->BindGraphicsPipeline(reflectionPipeline);
                    const DescriptorSetHandle sets[] = { shortBoxReflDescriptorSet3 };
                    cmd->BindDescriptorSets(PipelineBindPoint::Graphics, reflectionPipelineLayout, 0, 1, sets, 0, nullptr);
                } else {
                    cmd->BindGraphicsPipeline(reflectionPipeline);
                    const DescriptorSetHandle sets[] = { reflectionDescriptorSet3 };
                    cmd->BindDescriptorSets(PipelineBindPoint::Graphics, reflectionPipelineLayout, 0, 1, sets, 0, nullptr);
                }
                cmd->DrawIndexed(range.count, range.start, 0, 1, 0);
            }
        }
    );

    // Pass 1: Multi-View Rendering (Render to CubeMap)
    auto& mvData = renderGraph->AddPass<MultiViewPassData>("MultiViewPass", RGPassType::Graphics, RGPassCategory::Main,
        [&](MultiViewPassData& data, RenderGraphBuilder& builder) {
            // Create CubeMap (Size 512x512, 6 Faces)
            TextureDesc desc;
            desc.size = {512, 512, 1};
            desc.arraySize = 1; // For TextureCube, arraySize 1 means 1 Cube (6 faces)
            desc.mipLevels = 1;
            desc.format = DataFormat::BGRA8_UNorm;
            desc.type = TextureType::TextureCube;
            desc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            
            data.cubeMap = builder.CreateTexture("SceneCubeMap", desc);

            // Import Depth Texture
            TextureDesc depthDesc{};
            depthDesc.size = {512, 512, 1};
            depthDesc.mipLevels = 1;
            depthDesc.arraySize = 6;
            depthDesc.format = DataFormat::D32_Float;
            depthDesc.type = TextureType::Texture2DArray;
            depthDesc.usage = TextureUsage::DepthStencil;
            RGResourceHandle rgDepth = renderGraph->ImportTexture("MultiViewDepth", multiViewDepthTexture, depthDesc);
            
            // Declare Render Pass
            RGRenderPassDesc rpDesc;
            
            // Attachment 0: Color
            RGAttachmentDesc colorAtt;
            colorAtt.texture = data.cubeMap;
            colorAtt.loadOp = LoadAction::Clear;
            colorAtt.storeOp = StoreAction::Store;
            colorAtt.clearColor = ClearValue{math::v4{0.1f, 0.2f, 0.3f, 1.0f}};
            colorAtt.slice = 0; // Base slice
            rpDesc.colors.push_back(colorAtt);
            
            // Depth Stencil Attachment
            RGAttachmentDesc depthAtt;
            depthAtt.texture = rgDepth;
            depthAtt.loadOp = LoadAction::Clear;
            depthAtt.storeOp = StoreAction::Store;
            depthAtt.clearDepth = 1.0f;
            depthAtt.slice = 0;
            rpDesc.depthStencil = depthAtt;
            
            rpDesc.renderTargetArrayLength = 6; // Enable Layered Rendering for 6 faces
            
            builder.DeclareRenderPass(rpDesc);
        },
        [&](const MultiViewPassData& /*data*/, RenderGraphContext& context) {
            RHICommandBuffer* cmd = context.cmdBuffer;
            
            // Set Viewport & Scissor for CubeMap (512x512)
            rhi::ViewportDesc viewport;
            viewport.topLeft = primal::math::v2{0, 0};
            viewport.size = primal::math::v2{512, 512};
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            cmd->SetViewport(viewport);

            rhi::Rect scissor;
            scissor.offset = primal::math::s32v2{0, 0};
            scissor.extent = primal::math::u32v2{512, 512};
            cmd->SetScissor(scissor);
            
            // Bind Pipeline
            cmd->BindGraphicsPipeline(pipeline);
            
            // Bind Descriptor Sets
            const DescriptorSetHandle sets[] = { descriptorSet };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, pipelineLayout, 0, 1, sets, 0, nullptr);
            
            // Bind Vertex Buffers & Draw
            uint64_t offsets[] = {0};
            cmd->BindVertexBuffers(0, 1, &vertexBuffer, offsets);
            cmd->BindIndexBuffer(indexBuffer, DataFormat::R32_UInt, 0);
            
            // Draw 6 instances for 6 faces (Using InstanceID for Layer Selection)
            // DrawIndexed(indexCount, startIndex, baseVertex, instanceCount, startInstance)
            cmd->DrawIndexed(indexCount, 0, 0, 6, 0);
        }
    );
    
    // Ensure Scene Color Texture exists
    if (sceneColorTexture == handles::INVALID_RESOURCE) {
        TextureDesc sceneColorDesc = backBufferDesc;
        sceneColorDesc.format = DataFormat::BGRA8_UNorm;
        sceneColorDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
        sceneColorTexture = device->CreateTexture(sceneColorDesc);
    }

    // Pass 2: Main View (Render to mainColorTexture)
    struct MainPassData {
        RGResourceHandle output;
        RGResourceHandle velocity;
        RGResourceHandle depth;
    };

    auto& mainData = renderGraph->AddPass<MainPassData>("MainPass", RGPassType::Graphics, RGPassCategory::Main,
        [&](MainPassData& data, RenderGraphBuilder& builder) {
            // Import Main Color Texture
            TextureDesc colorDesc = backBufferDesc;
            colorDesc.format = DataFormat::BGRA8_UNorm;
            colorDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            RGResourceHandle rgMainColor = renderGraph->ImportTexture("MainColor", mainColorTexture, colorDesc);

            // Import Main Velocity Texture
            TextureDesc velocityDesc = backBufferDesc;
            velocityDesc.format = DataFormat::RG16_Float;
            velocityDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            RGResourceHandle rgMainVelocity = renderGraph->ImportTexture("MainVelocity", mainVelocityTexture, velocityDesc);

            // Import Main Depth Texture
            TextureDesc mainDepthDesc{};
            mainDepthDesc.size = {backBufferDesc.size.x, backBufferDesc.size.y, 1};
            mainDepthDesc.mipLevels = 1;
            mainDepthDesc.arraySize = 1;
            mainDepthDesc.format = DataFormat::D32_Float;
            mainDepthDesc.type = TextureType::Texture2D;
            mainDepthDesc.usage = TextureUsage::DepthStencil;
            RGResourceHandle rgMainDepth = renderGraph->ImportTexture("MainDepth", mainDepthTexture, mainDepthDesc);

            data.output = builder.Write(rgMainColor);
            data.velocity = builder.Write(rgMainVelocity);
            data.depth = builder.Write(rgMainDepth);

            // Read Reflection Texture for Mirror Box
            builder.Read(reflData.output, ResourceState::ShaderResource);
            builder.Read(reflData2.output, ResourceState::ShaderResource);
            builder.Read(reflData3.output, ResourceState::ShaderResource);

            RGRenderPassDesc rpDesc;
            RGAttachmentDesc colorAtt;
            colorAtt.texture = data.output;
            colorAtt.loadOp = LoadAction::Clear;
            colorAtt.storeOp = StoreAction::Store;
            colorAtt.clearColor = ClearValue{math::v4{0.1f, 0.1f, 0.1f, 0.0f}};
            rpDesc.colors.push_back(colorAtt);

            RGAttachmentDesc velocityAtt;
            velocityAtt.texture = data.velocity;
            velocityAtt.loadOp = LoadAction::Clear;
            velocityAtt.storeOp = StoreAction::Store;
            velocityAtt.clearColor = ClearValue{math::v4{0.0f, 0.0f, 0.0f, 0.0f}};
            rpDesc.colors.push_back(velocityAtt);

            RGAttachmentDesc depthAtt;
            depthAtt.texture = data.depth;
            depthAtt.loadOp = LoadAction::Clear;
            depthAtt.storeOp = StoreAction::Store; // Need to store for SSR
            depthAtt.clearDepth = 1.0f;
            rpDesc.depthStencil = depthAtt;

            builder.DeclareRenderPass(rpDesc);
        },
        [&](const MainPassData& data, RenderGraphContext& context) {
            // std::cout << "DEBUG: Executing MainPass" << std::endl;
            RHICommandBuffer* cmd = context.cmdBuffer;
            
            // Viewport & Scissor
            rhi::ViewportDesc viewport;
            viewport.topLeft = primal::math::v2{0, 0};
            viewport.size = primal::math::v2{(float)backBufferDesc.size.x, (float)backBufferDesc.size.y};
            viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;
            cmd->SetViewport(viewport);
            
            // Apply TAA Jitter to Projection Matrix (Vertex Shader Jitter)
            // But we need to keep the viewport unjittered to avoid edge artifacts.
            // The jitter is already applied in the SceneData uniform buffer (sceneData->jitter).
            
            rhi::Rect scissor;
            scissor.offset = primal::math::s32v2{0, 0};
            scissor.extent = primal::math::u32v2{backBufferDesc.size.x, backBufferDesc.size.y};
            cmd->SetScissor(scissor);


            // Bind Vertex Buffers
            uint64_t offsets[] = {0};
            cmd->BindVertexBuffers(0, 1, &vertexBuffer, offsets);
            cmd->BindIndexBuffer(indexBuffer, DataFormat::R32_UInt, 0);

            // Draw
            for (auto& [name, range] : drawRanges) {
                if (name == "MirrorBox") {
                    cmd->BindGraphicsPipeline(mirrorPipeline);
                    const DescriptorSetHandle mirrorSets[] = { mirrorDescriptorSet };
                    cmd->BindDescriptorSets(PipelineBindPoint::Graphics, mirrorPipelineLayout, 0, 1, mirrorSets, 0, nullptr);
                } else if (name == "ShortBox") {
            cmd->BindGraphicsPipeline(mainPipeline);
            const DescriptorSetHandle sets[] = { shortBoxMainDescriptorSet };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, pipelineLayout, 0, 1, sets, 0, nullptr);
        } else if (name == "Skybox") {
                    cmd->BindGraphicsPipeline(skyboxPipeline);
                    const DescriptorSetHandle sets[] = { skyboxDescriptorSet };
                    cmd->BindDescriptorSets(PipelineBindPoint::Graphics, skyboxPipelineLayout, 0, 1, sets, 0, nullptr);
                } else {
                    cmd->BindGraphicsPipeline(mainPipeline);
                    const DescriptorSetHandle mainSets[] = { mainDescriptorSet };
                    cmd->BindDescriptorSets(PipelineBindPoint::Graphics, pipelineLayout, 0, 1, mainSets, 0, nullptr);
                }
                cmd->DrawIndexed(range.count, range.start, 0, 1, 0);
            }
        }
    );

    // Pass 3: SSR Pass
    struct SSRPassData {
        RGResourceHandle sceneColor;
        RGResourceHandle sceneDepth;
        RGResourceHandle output;
    };

    auto& ssrData = renderGraph->AddPass<SSRPassData>("SSRPass", RGPassType::Compute, RGPassCategory::Main,
        [&](SSRPassData& data, RenderGraphBuilder& builder) {
            data.sceneColor = builder.Read(mainData.output, ResourceState::ShaderResource);
            data.sceneDepth = builder.Read(mainData.depth, ResourceState::ShaderResource);

            // SSR Output
            TextureDesc ssrDesc = backBufferDesc;
            ssrDesc.format = DataFormat::RGBA8_UNorm;
            ssrDesc.usage = TextureUsage::UnorderedAccess | TextureUsage::ShaderResource;
            RGResourceHandle rgSSROutput = renderGraph->ImportTexture("SSROutput", ssrOutputTexture, ssrDesc);
            data.output = builder.Write(rgSSROutput); 
        },
        [&](const SSRPassData& data, RenderGraphContext& context) {
            // Reconstruct View/Proj Matrix for SSR
            float aspect = (float)backBufferDesc.size.x / (float)backBufferDesc.size.y;
            if (aspect < 0.1f) aspect = 1280.0f / 720.0f;
            m4x4 projMatrix = CreatePerspective(math::constants::PI / 4.0f, aspect, 0.1f, 100.0f);
            v3 eye = primal::math::v3{0, 0, 18.0f}; 
            v3 center = primal::math::v3{0, 0, 0};
            v3 up = primal::math::v3{0, 1, 0};
            m4x4 viewMatrix = CreateLookAt(eye, center, up);

            ssrPass.Execute(context.cmdBuffer, 
                            mainColorTexture, 
                            mainDepthTexture, 
                            ssrOutputTexture, 
                            backBufferDesc.size.x, backBufferDesc.size.y, 
                            renderSystem.GetCurrentFrameIndex(),
                            viewMatrix, projMatrix);
        }
    );

    // Pass 4: Composite Pass (Writes to SceneColor)
    struct CompositePassData {
        RGResourceHandle mainColor;
        RGResourceHandle ssrColor;
        RGResourceHandle cubeMap; // For Debug Overlay
        RGResourceHandle output;
    };

    auto& compData = renderGraph->AddPass<CompositePassData>("CompositePass", RGPassType::Graphics, RGPassCategory::Main,
        [&](CompositePassData& data, RenderGraphBuilder& builder) {
            data.mainColor = builder.Read(mainData.output, ResourceState::ShaderResource);
            data.ssrColor = builder.Read(ssrData.output, ResourceState::ShaderResource);
            data.cubeMap = builder.Read(mvData.cubeMap, ResourceState::ShaderResource);

            RGResourceHandle rgSceneColor = renderGraph->ImportTexture("SceneColor", sceneColorTexture, backBufferDesc);
            data.output = builder.Write(rgSceneColor);

            RGRenderPassDesc rpDesc;
            RGAttachmentDesc colorAtt;
            colorAtt.texture = data.output;
            colorAtt.loadOp = LoadAction::Clear;
            colorAtt.storeOp = StoreAction::Store;
            colorAtt.clearColor = ClearValue{math::v4{0.0f, 0.0f, 0.0f, 1.0f}};
            rpDesc.colors.push_back(colorAtt);
            
            builder.DeclareRenderPass(rpDesc);
        },
        [&](const CompositePassData& data, RenderGraphContext& context) {
            RHICommandBuffer* cmd = context.cmdBuffer;
            
            // Viewport
            rhi::ViewportDesc viewport;
            viewport.topLeft = primal::math::v2{0, 0};
            viewport.size = primal::math::v2{(float)backBufferDesc.size.x, (float)backBufferDesc.size.y};
            viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;
            cmd->SetViewport(viewport);

            rhi::Rect scissor;
            scissor.offset = primal::math::s32v2{0, 0};
            scissor.extent = primal::math::u32v2{backBufferDesc.size.x, backBufferDesc.size.y};
            cmd->SetScissor(scissor);

            // 1. Draw Main Color (Opaque)
            cmd->BindGraphicsPipeline(blitPipeline); 
            const DescriptorSetHandle mainSets[] = { mainCompositeDescriptorSet };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, blitPipelineLayout, 0, 1, mainSets, 0, nullptr);
            cmd->Draw(6, 0, 1, 0); // Fullscreen Quad

            // 2. Draw SSR (Additive)
            cmd->BindGraphicsPipeline(ssrCompositePipeline);
            const DescriptorSetHandle ssrSets[] = { ssrCompositeDescriptorSet };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, blitPipelineLayout, 0, 1, ssrSets, 0, nullptr);
            cmd->Draw(6, 0, 1, 0);

            // 3. Debug Overlay
            if (showDebugOverlay) {
                // 1. Get CubeMap Texture Handle
                RenderGraphResource* res = context.graph->GetResource(data.cubeMap);
                if (!res) return;
                ResourceHandle cubeMapHandle = res->GetPhysicalHandle();
                
                // 2. Update Blit Descriptor Set (Per Frame)
                DescriptorImageInfo imageInfo{};
                imageInfo.imageView = cubeMapHandle;
                imageInfo.sampler = handles::INVALID_SAMPLER;
                imageInfo.imageLayout = ResourceState::ShaderResource;

                uint32_t frameIndex = renderSystem.GetCurrentFrameIndex();
                DescriptorSetHandle currentSet = blitDescriptorSets[frameIndex];
                
                // Update Uniform Buffer (Rotation)
                {
                    static float angle = 0.0f;
                    angle += 0.005f; 
                    m4x4 rot = rhi::math::CreateRotationMatrixY(angle);
                    
                    void* data = device->MapBuffer(blitUniformBuffer);
                    if (data) {
                        memcpy(data, &rot, sizeof(m4x4));
                        device->UnmapBuffer(blitUniformBuffer);
                    }
                }
                
                WriteDescriptorSet updateDesc{};
                updateDesc.dstSet = currentSet;
                updateDesc.dstBinding = 0;
                updateDesc.dstArrayElement = 0;
                updateDesc.descriptorCount = 1;
                updateDesc.descriptorType = DescriptorType::SampledImage;
                updateDesc.imageInfo = &imageInfo;
                
                device->UpdateDescriptorSets(1, &updateDesc);
                
                // 3. Bind Debug Pipeline
                cmd->BindGraphicsPipeline(debugPipeline);
                const DescriptorSetHandle sets[] = { currentSet };
                cmd->BindDescriptorSets(PipelineBindPoint::Graphics, blitPipelineLayout, 0, 1, sets, 0, nullptr);
                cmd->Draw(6, 0, 6, 0); 
            }
        }
    );

    // Pass 5: TAA Pass
    struct TAAPassData {
        RGResourceHandle colorInput;
        RGResourceHandle historyInput;
        RGResourceHandle velocityInput;
        RGResourceHandle output;
    };
    
    auto& taaData = renderGraph->AddPass<TAAPassData>("TAAPass", RGPassType::Graphics, RGPassCategory::Main,
        [&](TAAPassData& data, RenderGraphBuilder& builder) {
            data.colorInput = builder.Read(compData.output, ResourceState::ShaderResource);
            data.velocityInput = builder.Read(mainData.velocity, ResourceState::ShaderResource);
            
            RGResourceHandle rgHistory = renderGraph->ImportTexture("TAAHistory", taaHistoryTexture, backBufferDesc);
            data.historyInput = builder.Read(rgHistory, ResourceState::ShaderResource);
            
            RGResourceHandle rgResult = renderGraph->ImportTexture("TAAResult", taaResultTexture, backBufferDesc);
            data.output = builder.Write(rgResult);
            
            RGRenderPassDesc rpDesc;
            RGAttachmentDesc colorAtt;
            colorAtt.texture = data.output;
            colorAtt.loadOp = LoadAction::DontCare; // Optimization: We overwrite everything
            colorAtt.storeOp = StoreAction::Store;
            rpDesc.colors.push_back(colorAtt);
            
            builder.DeclareRenderPass(rpDesc);
        },
        [&](const TAAPassData& data, RenderGraphContext& context) {
             taaPass.Execute(context.cmdBuffer, 
                             sceneColorTexture, 
                             taaHistoryTexture, 
                             mainVelocityTexture, 
                             taaResultTexture, 
                             backBufferDesc.size.x, backBufferDesc.size.y,
                             renderSystem.GetCurrentFrameIndex(),
                             appliedJitter.x, appliedJitter.y,
                             appliedPrevJitter.x, appliedPrevJitter.y);
        }
    );

    // Pass 6: Present Pass (Blit TAA Result to BackBuffer)
    struct PresentPassData {
        RGResourceHandle input;
        RGResourceHandle backBuffer;
    };
    
    renderGraph->AddPass<PresentPassData>("PresentPass", RGPassType::Graphics, RGPassCategory::Present,
        [&](PresentPassData& data, RenderGraphBuilder& builder) {
            data.input = builder.Read(taaData.output, ResourceState::ShaderResource);
            // data.input = builder.Read(compData.output, ResourceState::ShaderResource); // DIRECT OUTPUT
            data.backBuffer = builder.Write(rgBackBuffer);
            
            RGRenderPassDesc rpDesc;
            RGAttachmentDesc colorAtt;
            colorAtt.texture = data.backBuffer;
            colorAtt.loadOp = LoadAction::DontCare;
            colorAtt.storeOp = StoreAction::Store;
            rpDesc.colors.push_back(colorAtt);
            builder.DeclareRenderPass(rpDesc);
        },
        [&](const PresentPassData& data, RenderGraphContext& context) {
            RHICommandBuffer* cmd = context.cmdBuffer;
            
            rhi::ViewportDesc viewport;
            viewport.topLeft = primal::math::v2{0, 0};
            viewport.size = primal::math::v2{(float)backBufferDesc.size.x, (float)backBufferDesc.size.y};
            viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;
            cmd->SetViewport(viewport);

            rhi::Rect scissor;
            scissor.offset = primal::math::s32v2{0, 0};
            scissor.extent = primal::math::u32v2{backBufferDesc.size.x, backBufferDesc.size.y};
            cmd->SetScissor(scissor);
            
            // Use Blit Pipeline
            cmd->BindGraphicsPipeline(blitPipeline); 
            
            // Use Present Descriptor Set
            DescriptorSetHandle presentSet = presentDescriptorSet;
            
            // Update to point to TAA Result (Ping-Pong) -> NOW POINTING TO SCENE COLOR
            DescriptorImageInfo imageInfo{};
            imageInfo.imageView = taaResultTexture;
            // imageInfo.imageView = sceneColorTexture; // DIRECT OUTPUT
            imageInfo.sampler = handles::INVALID_SAMPLER;
            imageInfo.imageLayout = ResourceState::ShaderResource;
            
            WriteDescriptorSet updateDesc{};
            updateDesc.dstSet = presentSet;
            updateDesc.dstBinding = 0;
            updateDesc.dstArrayElement = 0;
            updateDesc.descriptorCount = 1;
            updateDesc.descriptorType = DescriptorType::SampledImage;
            updateDesc.imageInfo = &imageInfo;
            
            // Binding 1 is already presentUniformBuffer (Identity), so no update needed.
            
            device->UpdateDescriptorSets(1, &updateDesc);
            
            const DescriptorSetHandle sets[] = { presentSet };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, blitPipelineLayout, 0, 1, sets, 0, nullptr);
            cmd->Draw(6, 0, 1, 0); 
        }
    );

    // Compile & Execute
    renderGraph->Compile();
    
    // Synchronous Execution (Match TestCSMIntegration logic)
    CommandBufferHandle cmdHandle = device->CreateCommandBuffer(CommandQueueType::Graphics);
    if (cmdHandle != handles::INVALID_COMMAND_BUFFER) {
        RHICommandBuffer* cmd = rhi::GetCommandBuffer(cmdHandle);
        
        if (cmd && cmd->Begin()) {
            // Wait for Swapchain Image Available
            if (fence != handles::INVALID_SYNC) {
                cmd->AddWaitSemaphore(fence, 0);
            }
            
            renderGraph->Execute(cmd);
            cmd->End();
            
            // Submit & Wait
            cmd->Submit();
            cmd->WaitForCompletion();
            
            device->DestroyCommandBuffer(cmdHandle);
            
            // Swap History
            std::swap(taaHistoryTexture, taaResultTexture);
        }
    }
 

    renderSystem.EndFrame();
}

/**
 * @brief Cleans up all created resources.
 */
void MultiViewTestCase::Shutdown() {
    // Unbind Input
    primal::input::unbind(std::hash<std::string>()("debug_toggle"));

    // 1. Destroy scene resources
    if (cubeMesh) {
        cubeMesh->Destroy(device);
        delete cubeMesh;
        cubeMesh = nullptr;
    }
    
    if (device) {
        // 2. Wait for GPU to finish
        device->WaitIdle();

        // 3. Destroy Command Buffer
        // commandBuffers.clear();

        // 4. Destroy RenderGraph (releases its internal resources)
        renderGraph.reset();
        
        // 5. Shutdown RenderSystem
        renderSystem.Shutdown();
        
        // 6. Destroy Manually Created Resources
        device->DestroyBuffer(viewUniformBuffer);
        device->DestroyBuffer(mainViewUniformBuffer); // New
        device->DestroyBuffer(instanceUniformBuffer);
        device->DestroyBuffer(vertexBuffer);
        device->DestroyBuffer(indexBuffer);
        
        device->DestroyTexture(multiViewDepthTexture); // New
        device->DestroyTexture(mainDepthTexture); // New
        device->DestroyTexture(mainColorTexture); // New
        device->DestroyTexture(mainVelocityTexture); // New
        device->DestroyTexture(taaHistoryTexture); // New
        device->DestroyTexture(taaResultTexture); // New

        taaPass.Shutdown(); // Shutdown TAA Pass
        
        device->DestroyDescriptorSetLayout(dsLayout);
        device->DestroyDescriptorSet(descriptorSet);
        device->DestroyDescriptorSet(mainDescriptorSet); // New
        device->DestroyPipelineLayout(pipelineLayout);
        device->DestroyPipeline(pipeline);
        device->DestroyPipeline(mainPipeline); // New
        device->DestroyShader(vertexShader);
        device->DestroyShader(pixelShader);

        // 7. Destroy Blit Resources
        // Cleanup SSR Resources
        ssrPass.Shutdown();
        device->DestroyTexture(ssrOutputTexture);
        device->DestroyPipeline(ssrCompositePipeline);
        device->DestroyDescriptorSet(ssrCompositeDescriptorSet);
        device->DestroyDescriptorSet(mainCompositeDescriptorSet);
        device->DestroyDescriptorSet(presentDescriptorSet);
        device->DestroyBuffer(presentUniformBuffer);

        device->DestroyPipeline(blitPipeline);
        device->DestroyPipelineLayout(blitPipelineLayout);
        for(auto set : blitDescriptorSets) {
            device->DestroyDescriptorSet(set);
        }
        blitDescriptorSets.clear();
        device->DestroyDescriptorSetLayout(blitDSLayout);
        device->DestroyBuffer(blitUniformBuffer);
        device->DestroyShader(blitVertexShader);
        device->DestroyShader(blitPixelShader);

        // Destroy Reflection Resources
        device->DestroyTexture(reflectionTexture);
        device->DestroyTexture(reflectionDepthTexture);
        device->DestroyBuffer(reflectionUniformBuffer);
        device->DestroyBuffer(reflectionPlaneBuffer);
        device->DestroyDescriptorSet(reflectionDescriptorSet);
        device->DestroyDescriptorSet(mirrorDescriptorSet);
        device->DestroyPipeline(reflectionPipeline);
        device->DestroyPipeline(mirrorPipeline);
        device->DestroyPipelineLayout(reflectionPipelineLayout);
        device->DestroyPipelineLayout(mirrorPipelineLayout);
        device->DestroyShader(reflectionVertexShader);
        device->DestroyShader(mirrorVertexShader);
        device->DestroyShader(mirrorPixelShader);

        // Destroy Simple Pipeline Resources
        device->DestroyPipeline(simplePipeline);
        device->DestroyShader(simpleVertexShader);
        device->DestroyShader(simplePixelShader);
    }
    
    // 8. Destroy Device
    
    // Destroy PBR Resources
    device->DestroyBuffer(sphereVertexBuffer);
    device->DestroyBuffer(sphereIndexBuffer);
    device->DestroyTexture(envMap);
    device->DestroyTexture(irradianceMap);
    device->DestroyTexture(prefilteredMap);
    device->DestroyTexture(brdfLUT);
    device->DestroyPipeline(pbrPipeline);
    device->DestroyPipelineLayout(pbrPipelineLayout);
    device->DestroyDescriptorSetLayout(pbrDSLayout);
    device->DestroySampler(pbrDefaultSampler);
    device->DestroySampler(pbrBRDFSampler);
    pbrMaterialInstance.reset();
    pbrMaterial.reset();
    iblPrecomputer.reset();

    device_ownership.reset();
    
    // 9. Close Window
    primal::platform::remove_window(window.get_id());
}


