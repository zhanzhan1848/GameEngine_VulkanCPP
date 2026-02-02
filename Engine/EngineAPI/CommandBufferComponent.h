#pragma once
#include "GameEntity.h"

namespace primal::command_buffer {
    DEFINE_TYPED_ID(command_buffer_id);
    
    class component final {
    public:
        constexpr explicit component(command_buffer_id id) : id_(id) {}
        constexpr component() : id_(id::invalid_id) {}
        constexpr command_buffer_id get_id() const { return id_; }
        constexpr bool is_valid() const { return id::is_valid(id_); }
        
        static constexpr component from_id(id::id_type id) { return component{ command_buffer_id{id} }; }

    private:
        command_buffer_id id_;
    };
}
