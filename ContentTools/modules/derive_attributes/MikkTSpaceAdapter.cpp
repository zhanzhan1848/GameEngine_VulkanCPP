#include "MikkTSpaceAdapter.h"
#include "mikktspace.h"

#include <cstdlib>

namespace primal::tools::derive {
namespace {

// The context object MikkTSpace receives — we extend it with our own state
// so we don't need a separate lookup from `m_pUserData`.
struct AdapterContext {
    SMikkTSpaceContext  ctx{};
    SMikkTSpaceInterface iface{};
    ProcessableMesh*    mesh{nullptr};
    u32                 uv_set_index{0};
};

int get_num_faces(const SMikkTSpaceContext* c) {
    auto* a = static_cast<AdapterContext*>(c->m_pUserData);
    return static_cast<int>(a->mesh->indices.size() / 3);
}

int get_num_vertices_of_face(const SMikkTSpaceContext* /*c*/, const int /*iFace*/) {
    return 3;  // ProcessableMesh.indices is always a triangle list
}

void get_position(const SMikkTSpaceContext* c, float fvPosOut[],
                  const int iFace, const int iVert) {
    auto* a = static_cast<AdapterContext*>(c->m_pUserData);
    const u32 idx = a->mesh->indices[iFace * 3 + iVert];
    const math::v3& p = a->mesh->positions[idx];
    fvPosOut[0] = p.x; fvPosOut[1] = p.y; fvPosOut[2] = p.z;
}

void get_normal(const SMikkTSpaceContext* c, float fvNormOut[],
                const int iFace, const int iVert) {
    auto* a = static_cast<AdapterContext*>(c->m_pUserData);
    const u32 idx = a->mesh->indices[iFace * 3 + iVert];
    if (!a->mesh->normals.empty()) {
        const math::v3& n = a->mesh->normals[idx];
        fvNormOut[0] = n.x; fvNormOut[1] = n.y; fvNormOut[2] = n.z;
    } else {
        fvNormOut[0] = 0.f; fvNormOut[1] = 1.f; fvNormOut[2] = 0.f;
    }
}

void get_texcoord(const SMikkTSpaceContext* c, float fvTexcOut[],
                  const int iFace, const int iVert) {
    auto* a = static_cast<AdapterContext*>(c->m_pUserData);
    const u32 idx = a->mesh->indices[iFace * 3 + iVert];
    if (a->uv_set_index < a->mesh->uv_sets.size() &&
        !a->mesh->uv_sets[a->uv_set_index].coords.empty()) {
        const math::v2& uv = a->mesh->uv_sets[a->uv_set_index].coords[idx];
        fvTexcOut[0] = uv.x; fvTexcOut[1] = uv.y;
    } else {
        fvTexcOut[0] = 0.f; fvTexcOut[1] = 0.f;
    }
}

void set_tspace_basic(const SMikkTSpaceContext* c, const float fvTangent[],
                      const float fSign, const int iFace, const int iVert) {
    auto* a = static_cast<AdapterContext*>(c->m_pUserData);
    const u32 idx = a->mesh->indices[iFace * 3 + iVert];
    if (a->mesh->tangents.size() <= idx) {
        a->mesh->tangents.resize(a->mesh->positions.size(),
                                 math::v4{0.f, 0.f, 0.f, 1.f});
    }
    // Last-write-wins when multiple face-corners share a vertex (see header
    // comment for the trade-off versus strict per-corner MikkTSpace).
    a->mesh->tangents[idx] = math::v4{
        fvTangent[0], fvTangent[1], fvTangent[2], fSign};
}

}  // namespace

SMikkTSpaceContext* create_context(ProcessableMesh& mesh, u32 uv_set_index) {
    auto* a = new AdapterContext{};
    a->mesh = &mesh;
    a->uv_set_index = uv_set_index;

    a->iface.m_getNumFaces         = get_num_faces;
    a->iface.m_getNumVerticesOfFace = get_num_vertices_of_face;
    a->iface.m_getPosition         = get_position;
    a->iface.m_getNormal           = get_normal;
    a->iface.m_getTexCoord         = get_texcoord;
    a->iface.m_setTSpaceBasic      = set_tspace_basic;
    a->iface.m_setTSpace           = nullptr;

    a->ctx.m_pInterface = &a->iface;
    a->ctx.m_pUserData  = a;
    return &a->ctx;
}

void destroy_context(SMikkTSpaceContext* ctx) {
    if (!ctx) return;
    auto* a = static_cast<AdapterContext*>(ctx->m_pUserData);
    delete a;
}

}  // namespace primal::tools::derive
