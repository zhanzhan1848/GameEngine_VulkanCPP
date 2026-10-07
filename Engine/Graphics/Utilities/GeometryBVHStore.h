#pragma once
#include "GeometryFirstHit.h"
#include <filesystem>
namespace primal::graphics::ray_geometry {
// Versioned portable little-endian cache; independent of pointer/asset ID values.
// Cache failures rebuild valid source geometry, never make missing geometry sky.
std::filesystem::path GeometryCacheDirectory();
std::shared_ptr<const MeshBLAS> LoadOrBuildMeshBLAS(const rhi::RHIMeshAsset&,
    const std::filesystem::path& directory,bool* loaded=nullptr,bool* persisted=nullptr);
u64 SceneSourceDigest(std::span<const InstanceInput>);
bool SaveMeshBLAS(const std::filesystem::path&,const MeshBLAS&);
std::shared_ptr<const MeshBLAS> LoadMeshBLAS(const std::filesystem::path&,u64 geometry_digest);
bool SaveSceneBVH(const std::filesystem::path&,const SceneBVH&,u64 source_digest);
bool LoadSceneBVH(const std::filesystem::path&,u64 source_digest,SceneBVH&);
bool LoadOrBuildSceneBVH(std::span<const InstanceInput>,const std::filesystem::path&,
    SceneBVH&,bool* loaded=nullptr,bool* persisted=nullptr);
}
