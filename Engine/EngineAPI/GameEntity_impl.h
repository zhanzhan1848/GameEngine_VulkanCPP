#pragma once
// Template implementations for entity::Add<T> and entity::Remove<T>.
// Include this header in .cpp files that need to dynamically add/remove components.

#include "GameEntity.h"
#include "Components/Transform.h"
#include "Components/Script.h"
#include "Components/Mesh.h"
#include "Components/Particle.h"
#include "Components/Cluster.h"
#include "Components/Geometry.h"
#include "Components/Material.h"
#include "Components/Light.h"
#include "Components/Camera.h"

namespace primal {

// component_init_info<T> specializations: map tag types to init_info types
template<> struct component_init_info<component::Transform>     { using type = transform::init_info; };
template<> struct component_init_info<component::Script>        { using type = script::init_info; };
template<> struct component_init_info<component::Mesh>          { using type = mesh::init_info; };
template<> struct component_init_info<component::Particle>      { using type = particle::init_info; };
template<> struct component_init_info<component::Cluster>       { using type = cluster::init_info; };
template<> struct component_init_info<component::Geometry>      { using type = geometry::component::init_info; };
template<> struct component_init_info<component::Material>      { using type = material::init_info; };
template<> struct component_init_info<component::Light>         { using type = light::init_info; };
template<> struct component_init_info<component::Camera>        { using type = camera::init_info; };

namespace game_entity {

template<typename T>
[[nodiscard]] auto entity::Get() const {
    if constexpr (std::is_same_v<T, component::Transform>) {
        return primal::transform::component{ primal::transform::transform_id{_id} };
    } else if constexpr (std::is_same_v<T, component::Script>) {
        return primal::script::get_component_for_entity(_id);
    } else if constexpr (std::is_same_v<T, component::Mesh>) {
        return primal::mesh::component{ primal::mesh::mesh_id{_id} };
    } else if constexpr (std::is_same_v<T, component::Particle>) {
        return primal::particle::get_component_for_entity(_id);
    } else if constexpr (std::is_same_v<T, component::Geometry>) {
        return primal::geometry::component::get(_id);
    } else if constexpr (std::is_same_v<T, component::Material>) {
        return primal::material::component{ primal::material::material_component_id{_id} };
    } else if constexpr (std::is_same_v<T, component::Light>) {
        return primal::light::component{ primal::light::light_component_id{_id} };
    } else if constexpr (std::is_same_v<T, component::Camera>) {
        return primal::camera::component{ primal::camera::camera_component_id{_id} };
    }
}

template<typename T>
void entity::Add(const typename component_init_info<T>::type& info) {
    if constexpr (std::is_same_v<T, component::Transform>) {
        auto c = transform::create(info, *this);
        assert(c.is_valid());
        set_component_bit(_id, static_cast<u8>(component_bit::Transform));
    } else if constexpr (std::is_same_v<T, component::Script>) {
        auto c = script::create(info, *this);
        assert(c.is_valid());
        set_component_bit(_id, static_cast<u8>(component_bit::Script));
    } else if constexpr (std::is_same_v<T, component::Mesh>) {
        auto c = mesh::create(info, *this);
        assert(c.is_valid());
        set_component_bit(_id, static_cast<u8>(component_bit::Mesh));
    } else if constexpr (std::is_same_v<T, component::Particle>) {
        auto c = particle::create(info, *this);
        set_component_bit(_id, static_cast<u8>(component_bit::Particle));
    } else if constexpr (std::is_same_v<T, component::Cluster>) {
        auto c = cluster::create(info, *this);
        assert(c != id::invalid_id);
        set_component_bit(_id, static_cast<u8>(component_bit::Cluster));
    } else if constexpr (std::is_same_v<T, component::Geometry>) {
        auto c = geometry::component::create(info, *this);
        assert(c.is_valid());
        set_component_bit(_id, static_cast<u8>(component_bit::Geometry));
    } else if constexpr (std::is_same_v<T, component::Material>) {
        auto c = material::create(info, *this);
        assert(c.is_valid());
        set_component_bit(_id, static_cast<u8>(component_bit::Material));
    } else if constexpr (std::is_same_v<T, component::Light>) {
        auto c = light::create(info, *this);
        assert(c.is_valid());
        set_component_bit(_id, static_cast<u8>(component_bit::Light));
    } else if constexpr (std::is_same_v<T, component::Camera>) {
        auto c = camera::create(info, *this);
        assert(c.is_valid());
        set_component_bit(_id, static_cast<u8>(component_bit::Camera));
    }
}

template<typename T>
void entity::Remove() {
    if constexpr (std::is_same_v<T, component::Transform>) {
        transform::component tc{ transform::transform_id{_id} };
        transform::remove(tc);
        clear_component_bit(_id, static_cast<u8>(component_bit::Transform));
    } else if constexpr (std::is_same_v<T, component::Script>) {
        script::remove_for_entity(_id);
        clear_component_bit(_id, static_cast<u8>(component_bit::Script));
    } else if constexpr (std::is_same_v<T, component::Mesh>) {
        mesh::component mc{ mesh::mesh_id{_id} };
        mesh::remove(mc);
        clear_component_bit(_id, static_cast<u8>(component_bit::Mesh));
    } else if constexpr (std::is_same_v<T, component::Particle>) {
        particle::remove_for_entity(_id);
        clear_component_bit(_id, static_cast<u8>(component_bit::Particle));
    } else if constexpr (std::is_same_v<T, component::Cluster>) {
        cluster::component cc{ _id };
        cluster::remove(cc);
        clear_component_bit(_id, static_cast<u8>(component_bit::Cluster));
    } else if constexpr (std::is_same_v<T, component::Geometry>) {
        geometry::component::remove(_id);
        clear_component_bit(_id, static_cast<u8>(component_bit::Geometry));
    } else if constexpr (std::is_same_v<T, component::Material>) {
        material::component mc{ material::material_component_id{_id} };
        material::remove(mc);
        clear_component_bit(_id, static_cast<u8>(component_bit::Material));
    } else if constexpr (std::is_same_v<T, component::Light>) {
        light::component lc{ light::light_component_id{_id} };
        light::remove(lc);
        clear_component_bit(_id, static_cast<u8>(component_bit::Light));
    } else if constexpr (std::is_same_v<T, component::Camera>) {
        camera::component cc{ camera::camera_component_id{_id} };
        camera::remove(cc);
        clear_component_bit(_id, static_cast<u8>(component_bit::Camera));
    }
}

} // namespace game_entity
} // namespace primal
