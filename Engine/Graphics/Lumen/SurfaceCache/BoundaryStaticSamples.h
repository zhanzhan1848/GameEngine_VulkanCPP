#pragma once
#include "SurfaceStaticPages.h"
#include "Engine/Graphics/RHI/Core/CardBoundarySidecar.h"

namespace primal::graphics::lumen {
// CPU attachment only. Records must match an already attached primary page
// triple. Instance transforms and lighting are deliberately left to consumers.
class BoundaryStaticSamples {
public:
    using Mesh=rhi::card_boundary::Mesh;
    void Detach() { meshes_.clear(); slots_.clear(); }
    bool Attach(const char* path, const SurfaceStaticPages& primary) {
        Detach();
        if (!primary.IsAttached()) return false;
        std::vector<Mesh> loaded;
        if (!rhi::card_boundary::Read(path,loaded)) return false;
        std::vector<u32> slots(primary.MeshCount(),0xffffffffu);
        for (u32 i=0;i<loaded.size();++i) {
            const auto& m=loaded[i];
            const u32 slot=primary.FindMesh(m.geometry_digest,m.layout_digest,m.material_digest);
            if (slot>=slots.size() || slots[slot]!=0xffffffffu) return false;
            slots[slot]=i;
        }
        meshes_=std::move(loaded); slots_=std::move(slots); return true;
    }
    const Mesh* Find(u32 static_slot,const SurfaceStaticPages& primary) const {
        if (!primary.IsAttached() || static_slot>=slots_.size() || slots_[static_slot]>=meshes_.size()) return nullptr;
        const auto& m=meshes_[slots_[static_slot]];
        // Revalidate the immutable identity directory even if the caller has
        // detached/replaced/reordered primary pages since the initial binding.
        return primary.FindMesh(m.geometry_digest,m.layout_digest,m.material_digest)==static_slot ? &m : nullptr;
    }
    u32 MeshCount() const { return static_cast<u32>(meshes_.size()); }
    u64 SampleCount() const { u64 n=0; for (const auto& m:meshes_) n+=m.samples.size(); return n; }
private:
    std::vector<Mesh> meshes_;
    std::vector<u32> slots_;
};
} // namespace primal::graphics::lumen
