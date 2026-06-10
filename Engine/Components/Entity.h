#pragma once

#include "ComponentsCommon.h"
#include "ComponentTraits.h"


namespace primal {

	namespace game_entity {

		// Create an entity with no components. Use Add<T>(init_info) to add components.
		entity create();
		// Backward-compatible create: entity_info is translated to create() + Add calls.
		struct entity_info;
		entity create(entity_info info);
		void remove(entity_id id);
		bool is_alive(entity_id id);
		u32 entity_count();

		// Component mask accessors
		component_mask get_component_mask(entity_id id);
		void set_component_bit(entity_id id, u8 bit);
		void clear_component_bit(entity_id id, u8 bit);

	}

	// Legacy entity_info for backward compatibility.
	// Prefer using create() + Add<T>() in new code.
#define INIT_INFO(component) namespace component {struct init_info;}
	INIT_INFO(transform);
	INIT_INFO(script);
	INIT_INFO(mesh);
	INIT_INFO(particle);
	INIT_INFO(cluster);
#undef INIT_INFO

	// Forward declaration for Geometry component (nested namespace)
	namespace geometry { namespace component { struct init_info; } }

	namespace game_entity {
		struct entity_info
		{
			transform::init_info* transform{ nullptr };
			script::init_info* script{ nullptr };
			mesh::init_info* mesh{ nullptr };
			particle::init_info* particle{ nullptr };
			cluster::init_info* cluster{ nullptr };
			geometry::component::init_info* geometry{ nullptr };
		};
	}
}
