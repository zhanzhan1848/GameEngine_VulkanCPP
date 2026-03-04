#pragma once

#include "Components/ComponentsCommon.h"

namespace primal::pipeline {

    DEFINE_TYPED_ID(pipeline_id);

    class component final
    {
    public:
        constexpr explicit component(pipeline_id id) : _id{ id } {}
        constexpr component() : _id{ id::invalid_id } {}
        [[nodiscard]] constexpr pipeline_id get_id() const { return _id; }
        [[nodiscard]] constexpr bool is_valid() const { return id::is_valid(_id); }

    private:
        pipeline_id _id;
    };
}
