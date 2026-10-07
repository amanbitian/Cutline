#include "render/RenderGraphBuilder.h"

#include "effects/MaskDocument.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace cutline::render::graph {
namespace {

void Field(std::ostringstream& out, const std::string& name, const std::string& value) {
  out << name.size() << ':' << name << '=' << value.size() << ':' << value << ';';
}

template <typename T>
void Number(std::ostringstream& out, const std::string& name, T value) {
  std::ostringstream text;
  text << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
  Field(out, name, text.str());
}

[[nodiscard]] NodeKind KindFor(const std::string& type) {
  if (type == "motion" || type == "crop" || type == "lens_correction" || type == "mesh_warp" ||
      type == "rolling_shutter" || type == "wave_warp" || type == "bulge") return NodeKind::Transform;
  if (type == "opacity") return NodeKind::Opacity;
  if (type == "lut") return NodeKind::Lut;
  if (type == "blur" || type == "gaussian_blur" || type == "directional_blur") return NodeKind::Blur;
  if (type == "grade" || type == "color_adjust" || type == "curves" || type == "hue_curves" ||
      type == "hsl_secondary" || type == "channel_mixer" || type == "black_and_white" || type == "tint") {
    return NodeKind::Color;
  }
  if (type == "blend_mode") return NodeKind::Blend;
  return NodeKind::Filter;
}

[[nodiscard]] std::string EffectParameters(const timeline::SampledEffect& effect) {
  std::ostringstream out;
  Field(out, "id", effect.id);
  Field(out, "type", effect.effect_type);
  Field(out, "preset", effect.preset_name);
  Field(out, "asset", effect.inline_asset);
  Number(out, "order", effect.order);
  Number(out, "intrinsic", effect.intrinsic ? 1 : 0);
  for (const auto& parameter : effect.parameters) {
    Field(out, "parameter", parameter.name);
    Number(out, "dimension", parameter.value.dimension);
    for (int component = 0; component < parameter.value.dimension; ++component) {
      Number(out, "component", parameter.value.components[static_cast<std::size_t>(component)]);
    }
  }
  for (const auto& mask : effect.masks) {
    Field(out, "mask-id", mask.id);
    Number(out, "mask-order", mask.order);
    Field(out, "mask-document", effects::mask::ToJson(mask.document));
  }
  return out.str();
}

[[nodiscard]] std::string SourceParameters(const timeline::SourceRequest& source, const BuildOptions& options) {
  std::ostringstream out;
  Field(out, "clip", source.clip_id);
  Field(out, "track", source.track_id);
  Field(out, "source", source.source_id);
  if (const auto version = options.source_versions.find(source.source_id); version != options.source_versions.end()) {
    Field(out, "source-version", version->second);
  }
  Number(out, "kind", static_cast<int>(source.source_kind));
  Number(out, "time-numerator", source.source_time.numerator());
  Number(out, "time-denominator", source.source_time.denominator());
  Number(out, "source-rate", source.source_rate);
  Number(out, "reversed", source.reversed ? 1 : 0);
  Field(out, "quality", options.quality_key);
  return out.str();
}

[[nodiscard]] std::string AddEffects(RenderGraph& graph, std::string input,
                                     const std::vector<timeline::SampledEffect>& effects,
                                     const Region& region, const std::string& prefix) {
  for (std::size_t index = 0; index < effects.size(); ++index) {
    const auto& effect = effects[index];
    const auto id = prefix + "/effect/" + std::to_string(index);
    graph.nodes.push_back({id, KindFor(effect.effect_type), {std::move(input)}, EffectParameters(effect), region});
    input = id;
  }
  return input;
}

}  // namespace

RenderGraph Build(const timeline::PlaybackPlan& plan, const BuildOptions& options) {
  RenderGraph graph;
  const Region full{0, 0, static_cast<int>(plan.width), static_cast<int>(plan.height)};
  struct Layer final {
    std::int64_t order{};
    std::string id;
    std::string clip_id;
  };
  std::vector<Layer> layers;
  std::unordered_map<std::string, std::string> clip_outputs;

  for (std::size_t index = 0; index < plan.video.size(); ++index) {
    const auto& source = plan.video[index];
    const auto prefix = "layer/" + std::to_string(index);
    const auto source_id = prefix + "/source";
    graph.nodes.push_back({source_id, NodeKind::Source, {}, SourceParameters(source, options), full});
    auto output = AddEffects(graph, source_id, source.effects, full, prefix);
    clip_outputs[source.clip_id] = output;
    layers.push_back({source.track_order, output, source.clip_id});
  }

  std::unordered_set<std::string> transitioned;
  for (std::size_t index = 0; index < plan.transitions.size(); ++index) {
    const auto& transition = plan.transitions[index];
    std::vector<std::string> inputs;
    for (const auto* clip : {&transition.from_clip_id, &transition.to_clip_id}) {
      if (!clip->has_value()) continue;
      if (const auto found = clip_outputs.find(**clip); found != clip_outputs.end()) inputs.push_back(found->second);
      transitioned.insert(**clip);
    }
    std::ostringstream parameters;
    Field(parameters, "id", transition.id);
    Field(parameters, "kind", transition.kind);
    Number(parameters, "progress", transition.progress);
    const auto prefix = "transition/" + std::to_string(index);
    const auto transition_id = prefix + "/mix";
    graph.nodes.push_back({transition_id, NodeKind::Transition, std::move(inputs), parameters.str(), full});
    const auto output = AddEffects(graph, transition_id, transition.effects, full, prefix);
    layers.push_back({transition.track_order, output, {}});
  }
  layers.erase(std::remove_if(layers.begin(), layers.end(), [&](const Layer& layer) {
                 return !layer.clip_id.empty() && transitioned.count(layer.clip_id) != 0;
               }), layers.end());
  std::stable_sort(layers.begin(), layers.end(), [](const Layer& left, const Layer& right) {
    return left.order < right.order;
  });

  std::string composite;
  if (layers.empty()) {
    composite = "empty/source";
    graph.nodes.push_back({composite, NodeKind::Source, {}, "transparent", full});
  } else {
    composite = layers.front().id;
    for (std::size_t index = 1; index < layers.size(); ++index) {
      const auto blend = "composite/" + std::to_string(index);
      graph.nodes.push_back({blend, NodeKind::Blend, {std::move(composite), layers[index].id}, "source-over", full});
      composite = blend;
    }
  }
  composite = AddEffects(graph, composite, plan.sequence_effects, full, "sequence");

  std::ostringstream output_parameters;
  Number(output_parameters, "render-version", plan.render_version);
  Field(output_parameters, "working-space", plan.working_color_space);
  Field(output_parameters, "display-space", plan.display_color_space);
  Field(output_parameters, "quality", options.quality_key);
  Number(output_parameters, "caption-count", plan.captions.size());
  graph.nodes.push_back({"output", NodeKind::Output, {std::move(composite)}, output_parameters.str(), full});
  graph.outputs.push_back("output");
  return graph;
}

}  // namespace cutline::render::graph
