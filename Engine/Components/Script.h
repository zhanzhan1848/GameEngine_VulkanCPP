#pragma once
#include "ComponentsCommon.h"

namespace primal::script {

	struct init_info
	{
		detail::script_creator script_creator;
	};

	component create(init_info info, game_entity::entity entity);
	void remove(component c);
	component get_component_for_entity(game_entity::entity_id eid);
	void remove_for_entity(game_entity::entity_id eid);
	void update(f32 dt);
}