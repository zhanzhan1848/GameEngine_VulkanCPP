#pragma once
// Opt-in, in-memory experiment. No cache read/write or runtime binding occurs
// here. Each primitive inherits its original leaf bounds, avoiding any loss of
// conservative coverage through reconstitution of quantized triangle edges.
#include "GeometryFirstHit.h"
#include "ExperimentalBinnedSAH.h"
#include <bit>
#include <unordered_map>
#include <vector>

namespace primal::graphics::ray_geometry {
inline std::shared_ptr<const MeshBLAS> BuildExperimentalSAHMesh(const MeshBLAS& input, const char** failure=nullptr) {
    using namespace primal::experimental_bvh;
    if(failure)*failure="none";
    auto reject=[&](const char* reason)->std::shared_ptr<const MeshBLAS>{if(failure)*failure=reason;return {};};
    if (input.nodes.empty() || input.triangles.empty() || !input.geometry_digest) return reject("empty mesh or digest");
    std::vector<Bounds> bounds(input.triangles.size());
    std::vector<u8> nodes_seen(input.nodes.size()), triangles_seen(input.triangles.size()), ids_seen(input.triangles.size());
    std::vector<std::pair<u32, u32>> stack{{0, 0}};
    while (!stack.empty()) {
        auto [index, depth] = stack.back(); stack.pop_back();
        if (index >= input.nodes.size() || depth >= 63 || nodes_seen[index]++) return reject("invalid, repeated, or over-depth source node");
        const auto& node = input.nodes[index]; Bounds b{};
        for (u32 k = 0; k < 3; ++k) {
            b.lo[k] = node.lo[k]; b.hi[k] = node.hi[k];
            if ((std::bit_cast<u32>(b.lo[k]) & 0x7f800000u) == 0x7f800000u ||
                (std::bit_cast<u32>(b.hi[k]) & 0x7f800000u) == 0x7f800000u || b.lo[k] > b.hi[k]) return reject("invalid source bounds");
        }
        if (node.links[3]) {
            const u32 first = node.links[2], count = node.links[3];
            if (first > input.triangles.size() || count > input.triangles.size() - first) return reject("source leaf range");
            for (u32 i = first; i < first + count; ++i) {
                if (triangles_seen[i]++) return reject("repeated source triangle");
                bounds[i] = b;
                const auto& tri = input.triangles[i];
                const u32 original_id=std::bit_cast<u32>(tri.a[3]);
                if (original_id>=ids_seen.size() || ids_seen[original_id]++) return reject("invalid original triangle identity");
                for (u32 k = 0; k < 3; ++k) {
                    for (float p : {tri.a[k], tri.e1[k], tri.e2[k], tri.a[k]+tri.e1[k], tri.a[k]+tri.e2[k]})
                        if ((std::bit_cast<u32>(p) & 0x7f800000u) == 0x7f800000u) return reject("nonfinite triangle");
                    if (tri.a[k] < b.lo[k] || tri.a[k] > b.hi[k] ||
                        tri.a[k]+tri.e1[k] < b.lo[k] || tri.a[k]+tri.e1[k] > b.hi[k] ||
                        tri.a[k]+tri.e2[k] < b.lo[k] || tri.a[k]+tri.e2[k] > b.hi[k]) return reject("triangle outside source leaf");
                }
            }
        } else {
            for (u32 k = 0; k < 2; ++k) {
                u32 child = node.links[k];
                if (child >= input.nodes.size()) return reject("source child range");
                for (u32 a = 0; a < 3; ++a)
                    if (input.nodes[child].lo[a] < node.lo[a] || input.nodes[child].hi[a] > node.hi[a]) return reject("source child outside parent");
                stack.emplace_back(child, depth + 1);
            }
        }
    }
    for (u8 value : nodes_seen) if (!value) return reject("unreachable source node");
    for (u8 value : triangles_seen) if (!value) return reject("unreachable source triangle");
    Tree tree;
    if (!build_binned_sah(bounds, tree)) return reject("SAH build failed: bounds, count or depth");
    auto result = std::make_shared<MeshBLAS>(); result->geometry_digest = input.geometry_digest;
    result->nodes.resize(tree.nodes.size()); result->triangles.resize(tree.primitives.size());
    for (u32 i = 0; i < tree.nodes.size(); ++i) {
        const auto& n = tree.nodes[i]; auto& out = result->nodes[i];
        for (u32 k = 0; k < 3; ++k) { out.lo[k] = n.bounds.lo[k]; out.hi[k] = n.bounds.hi[k]; }
        out.links[0] = n.left; out.links[1] = n.right; out.links[2] = n.first; out.links[3] = n.count;
    }
    for (u32 i = 0; i < tree.primitives.size(); ++i) result->triangles[i] = input.triangles[tree.primitives[i]];
    return result;
}
// Rebuild each distinct mesh once, retaining per-instance identity and world
// transforms. The output is independent of the caller and is never persisted.
// A caller must separately certify its scene source/epoch before GPU use.
inline bool BuildExperimentalSAHScene(std::span<const InstanceInput> inputs, SceneBVH& out,
                                      const char** failure=nullptr, u64* failed_mesh_digest=nullptr) {
    out = {};
    if(failure)*failure="none";if(failed_mesh_digest)*failed_mesh_digest=0;
    if (inputs.empty()) {if(failure)*failure="empty scene";return false;}
    std::unordered_map<const MeshBLAS*, std::shared_ptr<const MeshBLAS>> meshes;
    std::vector<InstanceInput> rebuilt;
    rebuilt.reserve(inputs.size());
    for (const auto& input : inputs) {
        if (!input.mesh) {if(failure)*failure="missing mesh";return false;}
        auto [it, fresh] = meshes.try_emplace(input.mesh.get());
        if (fresh) it->second = BuildExperimentalSAHMesh(*input.mesh,failure);
        if (!it->second) {if(failed_mesh_digest)*failed_mesh_digest=input.mesh->geometry_digest;return false;}
        rebuilt.push_back({it->second, input.world, input.instance_id});
    }
    SceneBVH result;
    if (!result.Build(rebuilt)) {if(failure)*failure="scene transform or build rejected";return false;}
    out = std::move(result);
    return true;
}
} // namespace primal::graphics::ray_geometry
