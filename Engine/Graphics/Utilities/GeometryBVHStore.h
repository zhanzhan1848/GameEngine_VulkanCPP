#pragma once
#include "GeometryFirstHit.h"
#include <filesystem>
namespace primal::graphics::ray_geometry {
struct CompactTraversalCertificate {
    bool valid=false;
    u32 top_max_pending=0, bottom_max_pending=0;
    bool Fits() const { return valid && top_max_pending<=16 && bottom_max_pending<=32; }
};
CompactTraversalCertificate CertifyCompactTraversal(const SceneBVH& scene);
// Immutable CPU certificate; never implies that a GPU copy has been uploaded.
struct EqualBoxSidecar {
    std::vector<u32> flags;
    u64 payload_hash=0, flags_hash=0;
    u32 top_count=0, bottom_count=0;
    bool valid=false;
};
std::shared_ptr<const EqualBoxSidecar> TryBuildEqualBoxSidecar(const SceneBVH& scene,u64 max_bytes);
bool EqualBoxSidecarIntact(const EqualBoxSidecar& sidecar);

// CPU-only ownership proof; success does not imply any GPU upload or dispatch.
enum class InstanceNormalOwnerFailure { None, Size, Budget, SharedRoot, InvalidScene, Allocation };
struct InstanceNormalOwnerCertificate {
    InstanceNormalOwnerFailure failure=InstanceNormalOwnerFailure::InvalidScene;
    std::vector<u32> owner;
    u64 source_payload_hash=0, owner_hash=0, owner_bytes=0, normal_bytes=0;
    bool Valid() const { return failure==InstanceNormalOwnerFailure::None; }
};
// maxBufferBytes is the real storage-buffer limit; totalBudgetBytes covers all
// three slots including owner and vec4 payload. Both must be supplied by caller.
InstanceNormalOwnerCertificate CertifyInstanceNormalOwners(const SceneBVH& scene,
    u64 maxBufferBytes,u64 totalBudgetBytes);

// Allocation failures are caught within the exception-enabled certificate TU.
std::shared_ptr<const InstanceNormalOwnerCertificate> TryCertifyInstanceNormalOwners(
    const SceneBVH& scene,u64 maxBufferBytes,u64 totalBudgetBytes);

struct UpperBVHStats {
    u32 roots_changed=0, original_max_pending=0, candidate_max_pending=0;
    u32 original_max_depth=0, candidate_max_depth=0;
    double original_cost=0, candidate_cost=0, build_ms=0;
    u64 original_payload_hash=0, candidate_payload_hash=0, extra_gpu_bytes=0;
};
// False means no candidate; output is left empty and original must be used.
bool BuildUpperSAH16(const SceneBVH& source, SceneBVH& out, UpperBVHStats& stats);
// Identity of an uploaded slot; source digest alone is not a tree certificate.
inline bool CompactBindingMatches(const CompactTraversalCertificate& cert,
    u64 certEpoch,u64 certDigest,u32 certSlot,const u64* certBuffers,const u64* certSizes,
    u64 sceneEpoch,u64 sceneDigest,u32 slot,const u64* buffers,const u64* sizes) {
    if(!cert.Fits() || certEpoch!=sceneEpoch || certDigest!=sceneDigest || !sceneDigest || certSlot!=slot) return false;
    for(u32 i=0;i<4;++i) if(certBuffers[i]!=buffers[i] || certSizes[i]!=sizes[i] || !sizes[i]) return false;
    return true;
}
inline bool UpperBindingMatches(u64 epoch,u64 digest,u32 slot,u64 originalHash,u64 candidateHash,
    u64 privateHandle,u64 privateSize,const u64* sourceHandles,const u64* sourceSizes,
    u64 sceneEpoch,u64 sceneDigest,u32 frameSlot,u64 expectedOriginal,u64 expectedCandidate,
    u64 currentPrivate,u64 currentPrivateSize,const u64* currentHandles,const u64* currentSizes) {
    if(epoch!=sceneEpoch || !digest || digest!=sceneDigest || slot!=frameSlot || !originalHash || !candidateHash ||
       originalHash!=expectedOriginal || candidateHash!=expectedCandidate || privateHandle!=currentPrivate ||
       privateSize!=currentPrivateSize || !privateSize) return false;
    for(u32 i=0;i<4;++i)if(sourceHandles[i]!=currentHandles[i] || sourceSizes[i]!=currentSizes[i] || !currentSizes[i]) return false;
    return true;
}
inline bool EqualBoxBindingMatches(u64 epoch,u64 digest,u32 slot,u64 payload,u64 flagsHash,u64 privateHandle,u64 privateSize,
    const u64* sourceHandles,const u64* sourceSizes,u64 currentEpoch,u64 currentDigest,u32 currentSlot,u64 expectedPayload,u64 expectedFlagsHash,
    u64 currentHandle,u64 currentSize,const u64* currentHandles,const u64* currentSizes) {
    if(epoch!=currentEpoch || !digest || digest!=currentDigest || slot!=currentSlot || !payload || payload!=expectedPayload ||
       !flagsHash || flagsHash!=expectedFlagsHash || privateHandle!=currentHandle || !privateSize || privateSize!=currentSize)return false;
    for(u32 i=0;i<4;++i)if(sourceHandles[i]!=currentHandles[i] || sourceSizes[i]!=currentSizes[i] || !currentSizes[i])return false;
    return true;
}
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
