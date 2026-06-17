#include "Graphics/MaterialGraph/MaterialGraph.h"
#include <queue>
#include <algorithm>

namespace primal::graphics::material_graph {

u32 MaterialGraph::AddNode(std::unique_ptr<MaterialNode> node) {
    u32 id = static_cast<u32>(nodes_.size());
    nodes_.push_back(std::move(node));
    return id;
}

void MaterialGraph::RemoveNode(u32 node_id) {
    if (node_id >= nodes_.size()) return;
    nodes_.erase(nodes_.begin() + node_id);
    connections_.erase(
        std::remove_if(connections_.begin(), connections_.end(),
            [node_id](const Connection& c) {
                return c.from_node == node_id || c.to_node == node_id;
            }),
        connections_.end());
    for (auto& c : connections_) {
        if (c.from_node > node_id) c.from_node--;
        if (c.to_node > node_id) c.to_node--;
    }
}

void MaterialGraph::Connect(u32 from_node, u32 from_pin, u32 to_node, u32 to_pin) {
    connections_.push_back({from_node, from_pin, to_node, to_pin});
}

void MaterialGraph::Disconnect(u32 from_node, u32 from_pin, u32 to_node, u32 to_pin) {
    connections_.erase(
        std::remove_if(connections_.begin(), connections_.end(),
            [&](const Connection& c) {
                return c.from_node == from_node && c.from_pin == from_pin &&
                       c.to_node == to_node && c.to_pin == to_pin;
            }),
        connections_.end());
}

void MaterialGraph::Clear() {
    nodes_.clear();
    connections_.clear();
    errors_.clear();
}

std::vector<u32> MaterialGraph::TopologicalSort() const {
    u32 n = static_cast<u32>(nodes_.size());
    std::vector<u32> in_degree(n, 0);

    for (const auto& conn : connections_) {
        in_degree[conn.to_node]++;
    }

    std::queue<u32> queue;
    for (u32 i = 0; i < n; ++i) {
        if (in_degree[i] == 0) queue.push(i);
    }

    std::vector<u32> order;
    order.reserve(n);
    while (!queue.empty()) {
        u32 node = queue.front();
        queue.pop();
        order.push_back(node);

        for (const auto& conn : connections_) {
            if (conn.from_node == node) {
                in_degree[conn.to_node]--;
                if (in_degree[conn.to_node] == 0) {
                    queue.push(conn.to_node);
                }
            }
        }
    }

    return order;
}

void MaterialGraph::Execute() {
    errors_.clear();
    auto order = TopologicalSort();
    for (u32 node_id : order) {
        for (const auto& conn : connections_) {
            if (conn.to_node == node_id) {
                MaterialNode* src = nodes_[conn.from_node].get();
                MaterialNode* dst = nodes_[conn.to_node].get();
                if (src && dst &&
                    conn.from_pin < src->outputs.size() &&
                    conn.to_pin < dst->inputs.size()) {
                    dst->inputs[conn.to_pin].data = src->outputs[conn.from_pin].data;
                }
            }
        }
        nodes_[node_id]->Execute();

        auto& node = nodes_[node_id];
        bool has_output = false;
        for (auto& pin : node->outputs) {
            if (pin.data) { has_output = true; break; }
        }
        if (!node->outputs.empty() && !has_output) {
            errors_.push_back({std::string("Node ") + node->TypeName() + " produced no output", node_id});
        }
    }
}

void MaterialGraph::ClearErrors() {
    errors_.clear();
}

} // namespace primal::graphics::material_graph
