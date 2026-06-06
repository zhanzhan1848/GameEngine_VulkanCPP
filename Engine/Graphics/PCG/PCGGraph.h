#pragma once

#include "Graphics/PCG/PCGNode.h"
#include <vector>
#include <memory>

namespace primal::graphics::pcg {

// Directed acyclic graph of PCG nodes. Supports topological execution with
// automatic data propagation between connected pins.
//
// Usage:
//   PCGGraph graph;
//
//   // Create and configure nodes
//   auto noise = std::make_unique<NoiseFieldNode>();
//   noise->frequency = 0.05f;
//   u32 noise_id = graph.AddNode(std::move(noise));
//
//   auto scatter = std::make_unique<FieldScatterNode>();
//   scatter->target_count = 1000;
//   u32 scatter_id = graph.AddNode(std::move(scatter));
//
//   // Wire: noise output 0 → scatter input 0 (density field)
//   graph.Connect(noise_id, 0, scatter_id, 0);
//
//   // Execute in topological order
//   graph.Execute();
//
//   // Read results
//   auto* points = graph.GetOutputPoints(scatter_id);
//
// Connect semantics:
//   Connect(from_node, from_pin, to_node, to_pin)
//   Data flows from from_node's output[from_pin] to to_node's input[to_pin].
//   During Execute(), upstream output data is copied to downstream input pins
//   before each node's Execute() is called, in topological order.
class PCGGraph {
public:
    // Add a node to the graph. Returns the node's ID (index).
    u32 AddNode(std::unique_ptr<PCGNode> node);

    // Create a directed connection between two nodes' pins.
    void Connect(u32 from_node, u32 from_pin, u32 to_node, u32 to_pin);

    // Execute all nodes in topological order. Before each node's Execute(),
    // upstream output data is propagated to its input pins.
    void Execute();

    // Retrieve the output PointSet from a specific node.
    // Returns the first output pin containing a PointSet, or nullptr if none.
    PCGPointSet* GetOutputPoints(u32 node_id);

private:
    struct Connection {
        u32 from_node;
        u32 from_pin;
        u32 to_node;
        u32 to_pin;
    };

    std::vector<std::unique_ptr<PCGNode>> nodes_;
    std::vector<Connection> connections_;

    std::vector<u32> TopologicalSort() const;
};

} // namespace primal::graphics::pcg
