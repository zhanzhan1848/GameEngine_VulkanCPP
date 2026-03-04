#include "PRTBaker.h"
#include "BVH.hpp"
#include <random>
#include <cmath>

namespace primal::graphics::utl
{
    // Helper for Dot product
    inline float Dot(const math::v3& a, const math::v3& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }
    
    // Helper for vector operations if not defined
    inline math::v3 Add(const math::v3& a, const math::v3& b)
    {
        return math::v3{a.x + b.x, a.y + b.y, a.z + b.z};
    }

    inline math::v3 Scale(const math::v3& a, float s)
    {
        return math::v3{a.x * s, a.y * s, a.z * s};
    }

    bool PRTBaker::BakeShadowedTransfer(const PRTBakingDesc& desc, primal::utl::vector<math::sh::SH9>& out_coeffs)
    {
        if (!desc.positions || !desc.normals || desc.vertex_count == 0)
            return false;

        // 1. Build BVH
        primal::utl::vector<math::v3> vertices(desc.vertex_count);
        for(u32 i=0; i<desc.vertex_count; ++i) vertices[i] = desc.positions[i];
        
        primal::utl::vector<u32> indices;
        if (desc.indices && desc.index_count > 0)
        {
            indices.resize(desc.index_count);
            for(u32 i=0; i<desc.index_count; ++i) indices[i] = desc.indices[i];
        }

        BVH bvh;
        bvh.Build(vertices, indices);

        // 2. Prepare output
        out_coeffs.resize(desc.vertex_count);

        // 3. Precompute uniform spherical samples
        primal::utl::vector<math::v3> sample_dirs;
        sample_dirs.reserve(desc.num_samples);
        
        std::mt19937 rng(12345);
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);

        for (u32 i = 0; i < desc.num_samples; ++i)
        {
            float u = dist(rng);
            float v = dist(rng);
            float theta = 2.0f * math::pi * u;
            float phi = std::acos(2.0f * v - 1.0f);

            float sin_phi = std::sin(phi);
            float x = sin_phi * std::cos(theta);
            float y = sin_phi * std::sin(theta);
            float z = std::cos(phi);
            sample_dirs.push_back(math::v3{x, y, z});
        }

        // 4. Bake per vertex
        float weight = 4.0f * math::pi / desc.num_samples;

        for (u32 v_idx = 0; v_idx < desc.vertex_count; ++v_idx)
        {
            math::v3 pos = desc.positions[v_idx];
            math::v3 normal = desc.normals[v_idx];
            
            // Offset pos slightly along normal to avoid self-intersection
            math::v3 ray_origin = Add(pos, Scale(normal, 0.01f));

            math::sh::SH9 sh; // Initialize to 0

            for (u32 s = 0; s < desc.num_samples; ++s)
            {
                math::v3 dir = sample_dirs[s];
                
                // Cosine term
                float ndotl = Dot(normal, dir);
                if (ndotl <= 0.0f) continue;

                // Visibility test
                BVH::Ray ray(ray_origin, dir);
                if (bvh.IntersectAny(ray, 1000.0f)) 
                {
                    // Blocked, V = 0
                    continue;
                }

                // Not blocked, V = 1
                math::sh::SH9 basis;
                math::sh::EvalSHBasis(dir, basis);

                for (int k = 0; k < 9; ++k)
                {
                    sh.coeffs[k] += ndotl * basis.coeffs[k];
                }
            }

            // Scale by integration weight
            for (int k = 0; k < 9; ++k)
            {
                sh.coeffs[k] *= weight;
            }

            out_coeffs[v_idx] = sh;
        }

        return true;
    }
}
