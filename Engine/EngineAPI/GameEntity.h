#pragma once

#include "Components/ComponentsCommon.h"
#include "Components/ComponentTraits.h"
#include "TransformComponent.h"
#include "ScriptComponent.h"
#include "MeshComponent.h"
#include "ParticleComponent.h"
#include "Engine/Utilities/Hash.h"

namespace primal {

	namespace game_entity {

		DEFINE_TYPED_ID(entity_id);

		// Forward declarations from Entity.cpp
		component_mask get_component_mask(entity_id id);
		void set_component_bit(entity_id id, u8 bit);
		void clear_component_bit(entity_id id, u8 bit);

	} // namespace game_entity

	// Forward declarations for component accessors used by entity::Get<T> / Remove<T>
	namespace script {
		class component;
		component get_component_for_entity(game_entity::entity_id eid);
		void remove_for_entity(game_entity::entity_id eid);
	}

	namespace particle {
		class component;
		component get_component_for_entity(game_entity::entity_id eid);
		void remove_for_entity(game_entity::entity_id eid);
	}

	namespace game_entity {

		class entity {
		public:
			constexpr explicit entity(entity_id id) : _id{ id } {}
			constexpr entity() : _id{ id::invalid_id } {}
			[[nodiscard]] constexpr entity_id get_id() const { return _id; }
			[[nodiscard]] constexpr bool is_valid() const { return id::is_valid(_id); }

			// Legacy accessors (backward compatible)
			[[nodiscard]] transform::component transform() const;
			[[nodiscard]] script::component script() const;
			[[nodiscard]] mesh::component mesh() const;
			[[nodiscard]] particle::component particle() const;

			[[nodiscard]] math::v4 rotation() const { return transform().rotation(); }
			[[nodiscard]] math::v3 orientation() const { return transform().orientation(); }
			[[nodiscard]] math::v3 position() const { return transform().position(); }
			[[nodiscard]] math::v3 scale() const { return transform().scale(); }

			// Template component API
			template<typename T>
			[[nodiscard]] bool Has() const {
				return (get_component_mask(_id) & bit_mask(component_bit_of<T>())) != 0;
			}

			template<typename T>
			[[nodiscard]] auto Get() const; // defined in GameEntity_impl.h

			// Add/Remove — definitions in GameEntity_impl.h
			template<typename T>
			void Add(const typename component_init_info<T>::type& info);

			template<typename T>
			void Remove();

		private:
			entity_id _id;
		};

	} // namespace game_entity

	namespace script {
		class entity_script : public game_entity::entity
		{
		public:
			virtual ~entity_script() = default;
			virtual void begin_play() {}
			virtual void update(float) {}
		protected:
			constexpr explicit entity_script(game_entity::entity entity)
				: game_entity::entity{ entity.get_id()} {}

			void set_rotation(math::v4 rotation_quaternion) const { set_rotation(this, rotation_quaternion); }
			void set_orientation(math::v3 orientation_vector) const { set_orientation(this, orientation_vector); }
			void set_position(math::v3 position) const { set_position(this, position); }
			void set_scale(math::v3 scale) const { set_scale(this, scale); }

			static void set_rotation(const game_entity::entity *const entity, math::v4 rotation_quaternion);
			static void set_orientation(const game_entity::entity *const entity, math::v3 orientation_vector);
			static void set_position(const game_entity::entity *const entity, math::v3 position);
			static void set_scale(const game_entity::entity *const entity, math::v3 scale);
		};

		class my_player_character : public entity_script
		{
		public:
			void update([[maybe_unused]] float dt) override
			{
				// do player character update
			}
		};

		namespace detail {
			using script_ptr = std::unique_ptr<entity_script>;
			using script_creator = script_ptr(*)(game_entity::entity entity);
			// using string_hash = std::hash<std::string>;
            struct string_hash {
                size_t operator()(const std::string& s) const {
                    uint32_t hashOut;
                    primal::utl::MurmurHash3_x86_32(s.data(), (int)s.length(), 0x9e3779b9, &hashOut);
                    return hashOut;
                }
            };

			u8 register_script(size_t, script_creator);
			script_creator get_script_creator(size_t tag);
#ifdef USE_WITH_EDITOR
			u8 add_script_name(const char* name);
#define REGISTER_SCRIPT(TYPE)													\
		namespace {																\
		const u8 _reg_##TYPE													\
		{ primal::script::detail::register_script(								\
				primal::script::detail::string_hash()(#TYPE),					\
				&primal::script::detail::create_script<TYPE>) };				\
		const u8 _name_##TYPE													\
		{ primal::script::detail::add_script_name(#TYPE) };						\
		}
#else
#define REGISTER_SCRIPT(TYPE)													\
		namespace {																\
		const u8 _reg_##TYPE													\
		{ primal::script::detail::register_script(								\
				primal::script::detail::string_hash()(#TYPE),					\
				&primal::script::detail::create_script<TYPE>) };				\
		}
#endif

			template<class script_class>
			script_ptr create_script(game_entity::entity entity)
			{
				assert(entity.is_valid());
				return std::make_unique<script_class>(entity);
			}
		} // namespace detail
	} // namespace script
} // namespace primal
