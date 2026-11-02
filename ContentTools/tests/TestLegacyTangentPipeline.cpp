#include "../Geometry.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace primal::tools;
static void run(float zsign, bool trusted) {
    const float q=std::sqrt(.5f);
    scene s; auto& m=s.lod_groups.emplace_back().meshes.emplace_back();
    m.positions.emplace_back(primal::math::v3{0,0,0});
    m.positions.emplace_back(primal::math::v3{q,0,zsign*q});
    m.positions.emplace_back(primal::math::v3{0,1,0});
    m.raw_indices.emplace_back(0);m.raw_indices.emplace_back(1);m.raw_indices.emplace_back(2);
    m.uv_sets.resize(1);
    m.uv_sets[0].emplace_back(primal::math::v2{0,0});m.uv_sets[0].emplace_back(primal::math::v2{1,0});m.uv_sets[0].emplace_back(primal::math::v2{0,1});
    for(int k=0;k<3;++k) {m.normals.emplace_back(primal::math::v3{-zsign*q,0,q});m.tangents.emplace_back(primal::math::v4{q,0,zsign*q,-1});}
    m.tangents_corner_trusted=trusted;
    geometry_import_settings settings{}; settings.calculate_tangents=0;
    progression p; process_scene(s,settings,&p);
    const auto& out=s.lod_groups[0].meshes[0];
    assert(out.vertices.size()==3 && out.indices.size()==3);
    assert(out.elements_type==elements::elements_type::static_normal_texture);
    for(unsigned j=0;j<3;++j) {
        const auto idx=out.indices[j]; const auto& v=out.vertices[idx];
        assert(v.tangent.z*zsign>0.7f);
        const primal::math::v3 expected_positions[3]={{0,0,0},{q,0,zsign*q},{0,1,0}};
        const primal::math::v2 expected_uv[3]={{0,0},{1,0},{0,1}};
        assert(std::fabs(v.position.x-expected_positions[j].x)<1e-6f && std::fabs(v.position.y-expected_positions[j].y)<1e-6f && std::fabs(v.position.z-expected_positions[j].z)<1e-6f);
        assert(std::fabs(v.uv.x-expected_uv[j].x)<1e-6f && std::fabs(v.uv.y-expected_uv[j].y)<1e-6f);
        assert(std::fabs(v.normal.x+zsign*q)<1e-6f && std::fabs(v.normal.y)<1e-6f && std::fabs(v.normal.z-q)<1e-6f);
        elements::static_normal_texture packed{};
        std::memcpy(&packed,&out.element_buffer[idx*sizeof(packed)],sizeof(packed));
        assert(bool(packed.t_sign & 4)==(zsign>0));
        assert(bool(packed.t_sign & 1)==trusted);
        const float x=float(packed.tangent[0])/65535.f*2.f-1.f;
        const float y=float(packed.tangent[1])/65535.f*2.f-1.f;
        const float z=(packed.t_sign&4?1.f:-1.f)*std::sqrt(std::fmax(0.f,1.f-x*x-y*y));
        assert(std::fabs(z-v.tangent.z)<0.0002f && std::fabs(x-v.tangent.x)<0.0001f);
    }
}
int main() {run(1,false);run(-1,false);run(1,true);std::puts("Legacy tangent pipeline packed readback passed");}
