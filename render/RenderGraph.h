#pragma once

// Backend-neutral immutable render graph. CPU, D3D12, Metal and Vulkan
// executors consume the same compiled nodes and content addresses.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cutline::render::graph {

enum class NodeKind {
  Source,
  Transform,
  Opacity,
  Color,
  Lut,
  Blur,
  Filter,
  Mask,
  Blend,
  Transition,
  Output,
};

struct Region final {
  int x{};
  int y{};
  int width{};
  int height{};

  [[nodiscard]] bool empty() const noexcept { return width <= 0 || height <= 0; }
};

struct Node final {
  std::string id;
  NodeKind kind{NodeKind::Source};
  std::vector<std::string> inputs;
  // Canonical serialized parameters, including sampled values and asset
  // versions. The graph never interprets backend-specific shader state.
  std::string parameters;
  Region region;
  bool enabled{true};
};

struct RenderGraph final {
  std::vector<Node> nodes;
  std::vector<std::string> outputs;
};

struct CompiledNode final {
  NodeKind kind{NodeKind::Source};
  std::vector<std::string> source_ids;
  // Ordered operations represented by this executable node. A fused node must
  // retain every operation and parameter; a dependency hash alone is a cache
  // address, not enough information for a backend to execute it.
  std::vector<Node> operations;
  std::vector<std::string> input_hashes;
  std::string dependency_hash;
  Region region;
};

struct CompileStatistics final {
  std::size_t input_nodes{};
  std::size_t live_nodes{};
  std::size_t dead_nodes{};
  std::size_t fused_nodes{};
};

struct CompiledGraph final {
  std::vector<CompiledNode> nodes;
  std::vector<std::string> output_hashes;
  CompileStatistics statistics;
  // Original node id -> content address. Unchanged branches keep their address
  // across graph revisions and can reuse a CPU texture or GPU surface.
  std::map<std::string, std::string> hashes;
};

[[nodiscard]] CompiledGraph Compile(const RenderGraph& graph);
[[nodiscard]] const char* ToString(NodeKind kind);

}  // namespace cutline::render::graph
