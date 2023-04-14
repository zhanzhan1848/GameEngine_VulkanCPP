#pragma once
#include "VulkanCommonHeaders.h"
#include <atomic>

namespace primal::graphics::vulkan
{

	namespace texture
	{
		class vulkan_texture_2d
		{
		public:

			vulkan_texture_2d() : _entity_id{ ++_count } {}

			explicit vulkan_texture_2d(std::string path);

			DISABLE_COPY_AND_MOVE(vulkan_texture_2d);

			~vulkan_texture_2d();

			void loadTexture(std::string path);

			[[nodiscard]] constexpr id::id_type getTextureID() const { return _entity_id; }
			[[nodiscard]] constexpr vulkan_texture& getTexture() { return _texture; }

		private:
			vulkan_texture								_texture;
			id::id_type									_entity_id;
			static std::atomic<u32>						_count;
		};
	}
}