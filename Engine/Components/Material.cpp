#include "Material.h"
#include "Entity.h"

namespace primal::material {

namespace {
    utl::vector<u8>                          exists_flags;
    utl::vector<graphics::ShaderTechnique>   techniques;
    utl::vector<id::id_type>                 albedo_textures;
    utl::vector<id::id_type>                 normal_textures;
    utl::vector<id::id_type>                 orm_textures;
    utl::vector<f32>                         base_color_r;
    utl::vector<f32>                         base_color_g;
    utl::vector<f32>                         base_color_b;
    utl::vector<f32>                         base_color_a;
    utl::vector<f32>                         roughnesses;
    utl::vector<f32>                         metallics;
    utl::vector<f32>                         alpha_cutoffs;
} // anonymous namespace

material::component create(init_info info, game_entity::entity entity)
{
    assert(entity.is_valid());
    const id::id_type eindex{ id::index(entity.get_id()) };

    if (exists_flags.size() <= eindex) {
        exists_flags.resize(eindex + 1, 0);
        techniques.resize(eindex + 1, graphics::ShaderTechnique::Opaque);
        albedo_textures.resize(eindex + 1, id::invalid_id);
        normal_textures.resize(eindex + 1, id::invalid_id);
        orm_textures.resize(eindex + 1, id::invalid_id);
        base_color_r.resize(eindex + 1, 1.f);
        base_color_g.resize(eindex + 1, 1.f);
        base_color_b.resize(eindex + 1, 1.f);
        base_color_a.resize(eindex + 1, 1.f);
        roughnesses.resize(eindex + 1, 0.5f);
        metallics.resize(eindex + 1, 0.f);
        alpha_cutoffs.resize(eindex + 1, 0.5f);
    }

    techniques[eindex] = info.technique;
    albedo_textures[eindex] = info.albedo_texture;
    normal_textures[eindex] = info.normal_texture;
    orm_textures[eindex] = info.orm_texture;
    base_color_r[eindex] = info.base_color[0];
    base_color_g[eindex] = info.base_color[1];
    base_color_b[eindex] = info.base_color[2];
    base_color_a[eindex] = info.base_color[3];
    roughnesses[eindex] = info.roughness;
    metallics[eindex] = info.metallic;
    alpha_cutoffs[eindex] = info.alpha_cutoff;
    exists_flags[eindex] = 1;

    return material::component{ material_component_id{ entity.get_id() } };
}

void remove(material::component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    if (eindex < exists_flags.size()) {
        exists_flags[eindex] = 0;
    }
}

graphics::ShaderTechnique get_technique(component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    return techniques[eindex];
}

f32 get_roughness(component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    return roughnesses[eindex];
}

f32 get_metallic(component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    return metallics[eindex];
}

f32 get_alpha_cutoff(component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    return alpha_cutoffs[eindex];
}

void get_base_color(component c, f32 out[4])
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    out[0] = base_color_r[eindex];
    out[1] = base_color_g[eindex];
    out[2] = base_color_b[eindex];
    out[3] = base_color_a[eindex];
}

id::id_type get_albedo_texture(component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    return albedo_textures[eindex];
}

id::id_type get_normal_texture(component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    return normal_textures[eindex];
}

id::id_type get_orm_texture(component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    return orm_textures[eindex];
}

void set_roughness(component c, f32 value)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    roughnesses[eindex] = value;
}

void set_metallic(component c, f32 value)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    metallics[eindex] = value;
}

void set_base_color(component c, const f32 value[4])
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    base_color_r[eindex] = value[0];
    base_color_g[eindex] = value[1];
    base_color_b[eindex] = value[2];
    base_color_a[eindex] = value[3];
}

void set_technique(component c, graphics::ShaderTechnique technique)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    techniques[eindex] = technique;
}

void set_alpha_cutoff(component c, f32 value)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    alpha_cutoffs[eindex] = value;
}

void set_albedo_texture(component c, id::id_type texture_id)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    albedo_textures[eindex] = texture_id;
}

void set_normal_texture(component c, id::id_type texture_id)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    normal_textures[eindex] = texture_id;
}

void set_orm_texture(component c, id::id_type texture_id)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    orm_textures[eindex] = texture_id;
}

// ============================================================================
// material::component member methods (Bridge layer)
// ============================================================================

graphics::ShaderTechnique component::technique() const {
    return get_technique(*this);
}

f32 component::roughness() const {
    return get_roughness(*this);
}

f32 component::metallic() const {
    return get_metallic(*this);
}

f32 component::alpha_cutoff() const {
    return get_alpha_cutoff(*this);
}

void component::base_color(f32 out[4]) const {
    get_base_color(*this, out);
}

void component::set_technique(graphics::ShaderTechnique t) {
    material::set_technique(*this, t);
}

void component::set_roughness(f32 value) {
    material::set_roughness(*this, value);
}

void component::set_metallic(f32 value) {
    material::set_metallic(*this, value);
}

void component::set_base_color(const f32 value[4]) {
    material::set_base_color(*this, value);
}

void component::set_alpha_cutoff(f32 value) {
    material::set_alpha_cutoff(*this, value);
}

id::id_type component::albedo_texture() const {
    return get_albedo_texture(*this);
}

id::id_type component::normal_texture() const {
    return get_normal_texture(*this);
}

id::id_type component::orm_texture() const {
    return get_orm_texture(*this);
}

void component::set_albedo_texture(id::id_type texture_id) {
    material::set_albedo_texture(*this, texture_id);
}

void component::set_normal_texture(id::id_type texture_id) {
    material::set_normal_texture(*this, texture_id);
}

void component::set_orm_texture(id::id_type texture_id) {
    material::set_orm_texture(*this, texture_id);
}

} // namespace primal::material
