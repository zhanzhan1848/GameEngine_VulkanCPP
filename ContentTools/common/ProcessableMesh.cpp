#include "ProcessableMesh.h"

namespace primal::tools {

// Sanity check the IR invariants documented on ProcessableMesh. Returns false
// on the first violated invariant; does not attempt to fix anything. Called
// at module IR boundaries (e.g. MeshConverters, AssetPipeline between stages)
// as a tripwire — module-level code is responsible for actually maintaining
// these invariants.
bool validate_invariants(const ProcessableMesh& m) {
    if (m.positions.empty()) return false;
    if (m.indices.size() % 3 != 0) return false;

    const u32 n = (u32)m.positions.size();

    if (!m.normals.empty()  && (u32)m.normals.size()  != n) return false;
    if (!m.tangents.empty() && (u32)m.tangents.size() != n) return false;
    if (!m.colors.empty()   && (u32)m.colors.size()   != n) return false;

    for (const auto& s : m.uv_sets) {
        if (!s.coords.empty() && (u32)s.coords.size() != n) return false;
    }

    return true;
}

}  // namespace primal::tools
