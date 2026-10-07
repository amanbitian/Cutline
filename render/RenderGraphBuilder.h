#pragma once

#include "render/RenderGraph.h"
#include "timeline/TimelineCompiler.h"

#include <map>
#include <string>

namespace cutline::render::graph {

struct BuildOptions final {
  // Media fingerprints or monotonically increasing source generations. The
  // project model intentionally does not put backend/cache data in a plan.
  std::map<std::string, std::string> source_versions;
  // Preview/full, resolution scale and temporal-quality choices belong here.
  std::string quality_key{"final"};
};

// Lowers one sampled playback plan into immutable dependency nodes. This does
// no media I/O and can run off the UI thread.
[[nodiscard]] RenderGraph Build(const timeline::PlaybackPlan& plan, const BuildOptions& options = {});

}  // namespace cutline::render::graph
