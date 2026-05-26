#pragma once

#include "ComponentsCommon.h"
#include "Particles/ParticleTypes.h"

namespace primal::game_entity { class entity; }

namespace primal::particle {
    DEFINE_TYPED_ID(particle_id);
}

#if _DEBUG
// In debug mode, particle_id is a struct (id_base subclass) and needs std::hash.
// In release mode, particle_id = unsigned int and std::hash already exists.
namespace std {
    template<>
    struct hash<primal::particle::particle_id> {
        size_t operator()(primal::particle::particle_id id) const noexcept {
            return static_cast<size_t>(static_cast<primal::id::id_type>(id));
        }
    };
}
#endif

#ifndef DISABLE_PARTICLE_SYSTEM

namespace primal::particle {

struct init_info {
    particles::emitter_config config;
    u32 material_id{ u32_invalid_id };
    bool auto_activate{ true };
};

struct component_flags {
    enum flags : u32 {
        spawn_rate = 0x01,
        position = 0x02,
        active = 0x04,
        burst = 0x08,
        all = spawn_rate | position | active | burst
    };
};

struct component_cache {
    f32 spawn_rate;
    math::v3 position_offset;
    particle_id id;
    u32 flags;
};

class component final {
public:
    constexpr explicit component(particle_id id) : _id{ id } {}
    constexpr component() : _id{ id::invalid_id } {}
    constexpr particle_id get_id() const { return _id; }
    constexpr bool is_valid() const { return id::is_valid(_id); }

    void set_active(bool active);
    bool is_active() const;
    void burst(u32 count);
    void set_spawn_rate(f32 rate);
    f32 get_spawn_rate() const;
    u32 get_active_particle_count() const;
    particles::emitter_id get_emitter_id() const;

private:
    particle_id _id;
};

component create(init_info info, game_entity::entity entity);
void remove(component c);

void update(const component_cache* cache, u32 count);

particles::emitter_id get_emitter_id(const component& c);
bool is_active(const component& c);
void set_active(component& c, bool active);
void burst(component& c, u32 count);

f32 get_spawn_rate(const component& c);
void set_spawn_rate(component& c, f32 rate);

u32 get_active_particle_count(const component& c);

} // namespace primal::particle

#else // DISABLE_PARTICLE_SYSTEM

namespace primal::particle {

struct init_info {};
struct component_cache {};

class component {
public:
    constexpr explicit component(particle_id) {}
    constexpr component() = default;
    constexpr bool is_valid() const { return false; }
};

inline component create(init_info, game_entity::entity) { return component{}; }
inline void remove(component) {}
inline void update(const component_cache*, u32) {}

} // namespace primal::particle

#endif // !DISABLE_PARTICLE_SYSTEM
