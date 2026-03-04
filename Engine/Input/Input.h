#pragma once
#include "CommonHeaders.h"
#include "EngineAPI/Input.h"

namespace primal::input
{
	void bind(input_source source);
	void unbind(input_source::type type, input_code::code code);
	void unbind(u64 binding);
	void set(input_source::type type, input_code::code code, math::v3 value);
    void get(input_source::type type, input_code::code code, input_value& value);
    void get(u64 binding, input_value& value);
}