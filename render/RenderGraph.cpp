#include "render/RenderGraph.h"

#include "core/util/Sha256.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace cutline::render::graph {
namespace {

[[nodiscard]] bool UnaryFusible(NodeKind kind) {
  return kind == NodeKind::Transform || kind == NodeKind::Opacity || kind == NodeKind::Color ||
         kind == NodeKind::Lut || kind == NodeKind::Mask;
}

[[nodiscard]] std::string Hash(const Node& node, const std::vector<std::string>& inputs) {
  std::string content = "render-graph-v1\n" + std::string(ToString(node.kind)) + "\n" + node.parameters + "\n";
  content += std::to_string(node.region.x) + "," + std::to_string(node.region.y) + "," +
             std::to_string(node.region.width) + "," + std::to_string(node.region.height) + "\n";
  for (const auto& input : inputs) content += input + "\n";
  return util::Sha256::Of(content).substr(0, 32);
}

}  // namespace

const char* ToString(NodeKind kind) {
  switch (kind) {
    case NodeKind::Source: return "source";
    case NodeKind::Transform: return "transform";
    case NodeKind::Opacity: return "opacity";
    case NodeKind::Color: return "color";
    case NodeKind::Lut: return "lut";
    case NodeKind::Blur: return "blur";
    case NodeKind::Filter: return "filter";
    case NodeKind::Mask: return "mask";
    case NodeKind::Blend: return "blend";
    case NodeKind::Transition: return "transition";
    case NodeKind::Output: return "output";
  }
  return "unknown";
}

CompiledGraph Compile(const RenderGraph& graph) {
  CompiledGraph compiled;
  compiled.statistics.input_nodes = graph.nodes.size();
  if (graph.outputs.empty()) throw std::invalid_argument("A render graph needs an output");

  std::unordered_map<std::string, const Node*> by_id;
  for (const auto& node : graph.nodes) {
    if (node.id.empty() || !by_id.emplace(node.id, &node).second) {
      throw std::invalid_argument("Render graph node identities must be non-empty and unique");
    }
  }

  std::unordered_set<std::string> live;
  std::unordered_set<std::string> visiting;
  std::vector<const Node*> order;
  const auto visit = [&](const auto& self, const std::string& id) -> void {
    const auto found = by_id.find(id);
    if (found == by_id.end()) throw std::invalid_argument("Render graph references an unknown node: " + id);
    if (live.count(id) != 0) return;
    if (!visiting.insert(id).second) throw std::invalid_argument("Render graph contains a cycle at: " + id);
    for (const auto& input : found->second->inputs) self(self, input);
    visiting.erase(id);
    live.insert(id);
    order.push_back(found->second);
  };
  for (const auto& output : graph.outputs) visit(visit, output);
  compiled.statistics.live_nodes = live.size();
  compiled.statistics.dead_nodes = graph.nodes.size() - live.size();

  std::unordered_map<std::string, std::size_t> consumers;
  for (const auto* node : order) for (const auto& input : node->inputs) ++consumers[input];

  std::unordered_map<std::string, std::size_t> compiled_slot;
  for (const auto* node : order) {
    if (!node->enabled && node->inputs.size() == 1) {
      compiled.hashes[node->id] = compiled.hashes.at(node->inputs.front());
      compiled_slot[node->id] = compiled_slot.at(node->inputs.front());
      continue;
    }
    std::vector<std::string> input_hashes;
    input_hashes.reserve(node->inputs.size());
    for (const auto& input : node->inputs) input_hashes.push_back(compiled.hashes.at(input));
    const auto dependency_hash = Hash(*node, input_hashes);

    bool fused = false;
    if (UnaryFusible(node->kind) && node->inputs.size() == 1 && consumers[node->inputs.front()] == 1) {
      const auto input_slot = compiled_slot.at(node->inputs.front());
      auto& previous = compiled.nodes[input_slot];
      if (UnaryFusible(previous.kind)) {
        previous.source_ids.push_back(node->id);
        previous.operations.push_back(*node);
        previous.dependency_hash = dependency_hash;
        previous.region = node->region.empty() ? previous.region : node->region;
        compiled_slot[node->id] = input_slot;
        ++compiled.statistics.fused_nodes;
        fused = true;
      }
    }
    if (!fused) {
      compiled_slot[node->id] = compiled.nodes.size();
      compiled.nodes.push_back({node->kind, {node->id}, {*node}, std::move(input_hashes), dependency_hash, node->region});
    }
    compiled.hashes[node->id] = dependency_hash;
  }
  for (const auto& output : graph.outputs) compiled.output_hashes.push_back(compiled.hashes.at(output));
  return compiled;
}

}  // namespace cutline::render::graph
