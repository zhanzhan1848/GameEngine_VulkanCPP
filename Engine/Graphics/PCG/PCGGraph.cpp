#include "Graphics/PCG/PCGGraph.h"
#include <queue>
#include <algorithm>

namespace primal::graphics::pcg {

u32 PCGGraph::AddNode(std::unique_ptr<PCGNode> node) {
    u32 id = static_cast<u32>(nodes_.size());
    nodes_.push_back(std::move(node));
    return id;
}

void PCGGraph::Connect(u32 from_node, u32 from_pin, u32 to_node, u32 to_pin) {
    connections_.push_back({from_node, from_pin, to_node, to_pin});
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

} // namespace primal::graphics::pcg
