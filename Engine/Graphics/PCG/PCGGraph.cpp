#include "Graphics/PCG/PCGGraph.h"
#include <queue>
#include <algorithm>

namespace primal::graphics::pcg {

u32 PCGGraph::AddNode(std::unique_ptr<PCGNode> node) {
    u32 id = static_cast<u32>(nodes_.size());
    nodes_.push_back(std::move(node));
    return id;
}

void PCGGraph::RemoveNode(u32 node_id) {
    if (node_id >= nodes_.size()) return;

    nodes_.erase(nodes_.begin() + node_id);

    // Remove connections referencing the deleted node, and decrement indices > node_id.
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

void PCGGraph::Connect(u32 from_node, u32 from_pin, u32 to_node, u32 to_pin) {
    connections_.push_back({from_node, from_pin, to_node, to_pin});
}

void PCGGraph::Disconnect(u32 from_node, u32 from_pin, u32 to_node, u32 to_pin) {
    connections_.erase(
        std::remove_if(connections_.begin(), connections_.end(),
            [&](const Connection& c) {
                return c.from_node == from_node && c.from_pin == from_pin &&
                       c.to_node == to_node && c.to_pin == to_pin;
            }),
        connections_.end());
}

void PCGGraph::Clear() {
    nodes_.clear();
    connections_.clear();
    mesh_slots_.clear();
    errors_.clear();
}

std::vector<u32> PCGGraph::TopologicalSort() const {
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

void PCGGraph::Execute() {
    errors_.clear();
    auto order = TopologicalSort();
    for (u32 node_id : order) {
        // Propagate upstream outputs to this node's inputs before executing
        for (const auto& conn : connections_) {
            if (conn.to_node == node_id) {
                PCGNode* src = nodes_[conn.from_node].get();
                PCGNode* dst = nodes_[conn.to_node].get();
                if (src && dst &&
                    conn.from_pin < src->outputs.size() &&
                    conn.to_pin < dst->inputs.size()) {
                    dst->inputs[conn.to_pin].data = src->outputs[conn.from_pin].data;
                }
            }
        }
        nodes_[node_id]->Execute();

        // Check if node produced output where expected
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

PCGPointSet* PCGGraph::GetOutputPoints(u32 node_id) {
    if (node_id >= nodes_.size()) return nullptr;
    for (auto& pin : nodes_[node_id]->outputs) {
        if (pin.data && pin.data->type == PCGDataType::PointSet) {
            return static_cast<PCGPointSet*>(pin.data);
        }
    }
    return nullptr;
}

u32 PCGGraph::AddMeshSlot(const char* path, const char* name) {
    u32 idx = static_cast<u32>(mesh_slots_.size());
    mesh_slots_.push_back({std::string(path), std::string(name)});
    return idx;
}

void PCGGraph::RemoveMeshSlot(u32 index) {
    if (index >= mesh_slots_.size()) return;
    mesh_slots_.erase(mesh_slots_.begin() + index);
}

void PCGGraph::SetMeshSlot(u32 index, const char* path, const char* name) {
    if (index >= mesh_slots_.size()) return;
    mesh_slots_[index] = {std::string(path), std::string(name)};
}

u32 PCGGraph::GetMeshSlotCount() const {
    return static_cast<u32>(mesh_slots_.size());
}

const PCGMeshSlot& PCGGraph::GetMeshSlot(u32 index) const {
    return mesh_slots_[index];
}

void PCGGraph::ClearErrors() {
    errors_.clear();
}

} // namespace primal::graphics::pcg
