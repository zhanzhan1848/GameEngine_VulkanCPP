#pragma once

#include "Graphics/MaterialGraph/MaterialNode.h"
#include <vector>
#include <memory>
#include <string>

namespace primal::graphics::material_graph {

struct MaterialExecError {
    std::string message;
    u32 node_id;
};

class MaterialGraph {
public:
    u32 AddNode(std::unique_ptr<MaterialNode> node);
    void RemoveNode(u32 node_id);
    void Connect(u32 from_node, u32 from_pin, u32 to_node, u32 to_pin);
    void Disconnect(u32 from_node, u32 from_pin, u32 to_node, u32 to_pin);
    void Clear();
    void Execute();

    const std::vector<std::unique_ptr<MaterialNode>>& GetNodes() const { return nodes_; }

    struct Connection {
        u32 from_node;
        u32 from_pin;
        u32 to_node;
        u32 to_pin;
    };

    const std::vector<Connection>& GetConnections() const { return connections_; }

    const std::vector<MaterialExecError>& GetErrors() const { return errors_; }
    void ClearErrors();

private:
    std::vector<std::unique_ptr<MaterialNode>> nodes_;
    std::vector<Connection> connections_;
    std::vector<MaterialExecError> errors_;

    std::vector<u32> TopologicalSort() const;
};

} // namespace primal::graphics::material_graph
