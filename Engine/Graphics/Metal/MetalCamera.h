#pragma once

#include "MetalCommonHeaders.h"

namespace primal::graphics::metal::camera
{
    class metal_camera
	{
	public:
		explicit metal_camera(camera_init_info info);

		void update();
		void up(math::v3 up);
		constexpr void field_of_view(f32 fov);
		constexpr void aspect_ratio(f32 aspect_ratio);
		constexpr void view_width(f32 width);
		constexpr void view_height(f32 height);
		constexpr void near_z(f32 near_z);
		constexpr void far_z(f32 far_z);

		[[nodiscard]] math::m4x4 view() const									        		{ return _view; }
		[[nodiscard]] math::m4x4 projection() const							        			{ return _projection; }
		[[nodiscard]] math::m4x4 inverse_projection() const					        			{ return _inverse_projection; }
		[[nodiscard]] math::m4x4 view_projection() const						        		{ return _view_projection; }
		[[nodiscard]] math::m4x4 inverse_view_projection() const				        		{ return _inverse_view_projection; }
		[[nodiscard]] math::v3 position() const								        			{ return _position; }
		[[nodiscard]] math::v3 direction() const								        		{ return _direction; }
		[[nodiscard]] math::v3 up() const									            		{ return _up; }
		[[nodiscard]] constexpr f32 near_z() const												{ return _near_z; }
		[[nodiscard]] constexpr f32 far_z() const												{ return _far_z; }
		[[nodiscard]] constexpr f32 field_of_view() const										{ return _field_of_view; }
		[[nodiscard]] constexpr f32 aspect_ratio() const										{ return _aspect_ratio; }
		[[nodiscard]] constexpr f32 view_width() const											{ return _view_width; }
		[[nodiscard]] constexpr f32 view_height() const											{ return _view_height; }
		[[nodiscard]] constexpr graphics::camera::type projection_type() const					{ return _projection_type; }
		[[nodiscard]] constexpr id::id_type entity_id() const									{ return _entity_id; }

	private:
		math::m4x4							        _view;
		math::m4x4							        _projection;
		math::m4x4							        _inverse_projection;
		math::m4x4							        _view_projection;
		math::m4x4							        _inverse_view_projection;
		math::v3							        _position{};
		math::v3							        _direction{};
		math::v3							        _up;
		f32											_near_z;
		f32											_far_z;
		union {
			f32										_field_of_view;				// The field of view for perspective camera
			f32										_view_width;				// View width in pixels for orthographic camera
		};
		union {
			f32										_aspect_ratio;				// Width/height aspect ratio for perspective camera
			f32										_view_height;				// View height in pixels for orthographic camera
		};
		graphics::camera::type						_projection_type;
		id::id_type									_entity_id;
		bool										_is_dirty;
	};

    graphics::camera create(camera_init_info info);
	void remove(camera_id id);
	void set_parameter(camera_id id, camera_parameter::parameter parameter, const void *const data, u32 data_size);
	void get_parameter(camera_id id, camera_parameter::parameter parameter, void *const data, u32 data_size);
	[[nodiscard]] metal_camera& get(camera_id id);
}