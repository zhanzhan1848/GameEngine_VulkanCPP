#pragma once

#include "Graphics/PCG/PCGNode.h"
#include <vector>
#include <memory>
#include <string>

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

// Mesh slot in the graph-level mesh library.
// Each slot maps an index to an asset path and display name.
// MeshAssignNode weights[i] references slot i.
//
// The mesh library lives at the graph level (not per-node) so multiple
// MeshAssignNodes share the same mesh pool. The editor displays a single
// mesh list panel where users drag in model files.
struct PCGMeshSlot {
    std::string path;  // Asset path, e.g. "Content/Props/Tree.model"
    std::string name;  // Display name, e.g. "Oak Tree"
};

// Execution error recorded during PCGGraph::Execute().
// Errors are collected when a node has output pins but produces no data.
struct PCGExecError {
    std::string message;    // Human-readable error description
    u32 node_id;            // ID of the failing node, u32(-1) if unknown
};

class PCGGraph {
public:
    // Add a node to the graph. Returns the node's ID (index).
    u32 AddNode(std::unique_ptr<PCGNode> node);

    // Remove a node by ID. All connections to/from this node are removed.
    // Connections referencing nodes with ID > node_id have their indices
    // decremented to stay valid. O(n) where n = number of connections.
    void RemoveNode(u32 node_id);

    // Create a directed connection between two nodes' pins.
    // Data flows: from_node's output[from_pin] → to_node's input[to_pin].
    void Connect(u32 from_node, u32 from_pin, u32 to_node, u32 to_pin);

    // Remove a specific connection. No-op if the connection doesn't exist.
    void Disconnect(u32 from_node, u32 from_pin, u32 to_node, u32 to_pin);

    // Remove all nodes, connections, mesh slots, and errors.
    void Clear();

    // Execute all nodes in topological order. Before each node's Execute(),
    // upstream output data is propagated to its input pins. After execution,
    // any node that has output pins but produced no data is recorded as an error.
    void Execute();

    // Retrieve the output PointSet from a specific node.
    // Returns the first output pin containing a PointSet, or nullptr if none.
    PCGPointSet* GetOutputPoints(u32 node_id);

    // Const accessors for serialization.
    const std::vector<std::unique_ptr<PCGNode>>& GetNodes() const { return nodes_; }

    struct Connection {
        u32 from_node;
        u32 from_pin;
        u32 to_node;
        u32 to_pin;
    };

    const std::vector<Connection>& GetConnections() const { return connections_; }

    // --- Mesh Library ---
    // Graph-level mesh registry. MeshAssignNode weights index into these slots.
    // The editor uses this to show a unified mesh list panel.

    // Add a mesh slot. Returns the slot's index.
    u32 AddMeshSlot(const char* path, const char* name);

    // Remove a mesh slot by index. Indices above shift down.
    void RemoveMeshSlot(u32 index);

    // Update a mesh slot's path and name.
    void SetMeshSlot(u32 index, const char* path, const char* name);

    // Returns the number of registered mesh slots.
    u32 GetMeshSlotCount() const;

    // Returns a mesh slot by index. No bounds checking — caller must ensure
    // index < GetMeshSlotCount().
    const PCGMeshSlot& GetMeshSlot(u32 index) const;

    // --- Error Reporting ---
    // Errors are accumulated during Execute() and persist until explicitly cleared.

    const std::vector<PCGExecError>& GetErrors() const { return errors_; }
    void ClearErrors();

private:
    std::vector<std::unique_ptr<PCGNode>> nodes_;
    std::vector<Connection> connections_;
    std::vector<PCGMeshSlot> mesh_slots_;
    std::vector<PCGExecError> errors_;

    // Kahn's BFS topological sort. Returns node IDs in execution order.
    std::vector<u32> TopologicalSort() const;
};

} // namespace primal::graphics::pcg
