#pragma once

#include "ComponentsCommon.h"


namespace primal {

#define INIT_INFO(component) namespace component {struct init_info;}
	INIT_INFO(transform);
	INIT_INFO(script);
	INIT_INFO(mesh);
	INIT_INFO(particle);
#undef INIT_INFO

	namespace game_entity {
		struct entity_info
		{
			transform::init_info* transform{ nullptr };
			script::init_info* script{ nullptr };
			mesh::init_info* mesh{ nullptr };
			particle::init_info* particle{ nullptr };
		};

		entity create(entity_info info);
		void remove(entity_id id);
		bool is_alive(entity_id id);

	}
}