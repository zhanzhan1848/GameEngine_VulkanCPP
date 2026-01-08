#include "Engine/Graphics/RHI/Platforms/Metal/MetalMath.h"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace primal::graphics::rhi::metal;
using namespace primal::math;

// Helper to print vector
void print_vec4(const char* name, simd::float4 v) {
    std::cout << name << ": [" << v.x << ", " << v.y << ", " << v.z << ", " << v.w << "]" << std::endl;
}

bool TestPerspectiveMatrix() {
    float fov = 90.0f * (M_PI / 180.0f);
    float aspect = 16.0f / 9.0f;
    float near = 0.1f;
    float far = 100.0f;

    m4x4 proj = CreatePerspectiveMatrix(fov, aspect, near, far);

    // Test Near Plane Point (0, 0, -near)
    simd::float4 p_near = { 0.0f, 0.0f, -near, 1.0f };
    simd::float4 p_near_clip = proj * p_near;
    
    // Check W (should be positive for valid clipping, but let's see what we get)
    // If w is negative, we just check the ratio z/w
    
    float z_ndc_near = p_near_clip.z / p_near_clip.w;
    
    std::cout << "Near Plane Test:" << std::endl;
    print_vec4("Clip", p_near_clip);
    std::cout << "NDC Z: " << z_ndc_near << std::endl;

    if (std::abs(z_ndc_near - 0.0f) > 1e-5f) {
        std::cerr << "Near plane Z mapping failed! Expected 0.0, got " << z_ndc_near << std::endl;
        return false;
    }

    // Test Far Plane Point (0, 0, -far)
    simd::float4 p_far = { 0.0f, 0.0f, -far, 1.0f };
    simd::float4 p_far_clip = proj * p_far;
    
    float z_ndc_far = p_far_clip.z / p_far_clip.w;
    
    std::cout << "Far Plane Test:" << std::endl;
    print_vec4("Clip", p_far_clip);
    std::cout << "NDC Z: " << z_ndc_far << std::endl;

    if (std::abs(z_ndc_far - 1.0f) > 1e-5f) {
        std::cerr << "Far plane Z mapping failed! Expected 1.0, got " << z_ndc_far << std::endl;
        return false;
    }

    return true;
}

int main() {
    if (TestPerspectiveMatrix()) {
        std::cout << "TestPerspectiveMatrix Passed" << std::endl;
    } else {
        std::cout << "TestPerspectiveMatrix Failed" << std::endl;
        return 1;
    }
    return 0;
}
