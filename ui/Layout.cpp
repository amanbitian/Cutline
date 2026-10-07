#include "ui/Layout.h"

#include "core/util/Json.h"
#include "core/util/JsonParse.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <set>
#include <stdexcept>

namespace cutline::ui {

// ---------------------------------------------------------------- registry ----

void PanelRegistry::Add(PanelDef panel) {
  if (panel.id.empty()) throw std::invalid_argument("A panel needs an id");
  if (Find(panel.id) != nullptr) throw std::invalid_argument("The panel " + panel.id + " is registered twice");
  panels_.push_back(std::move(panel));
}

const PanelDef* PanelRegistry::Find(const std::string& id) const {
  for (const auto& panel : panels_) {
    if (panel.id == id) return &panel;
  }
  return nullptr;
}

const PanelRegistry& BuiltInPanels() {
  static const PanelRegistry registry = [] {
    PanelRegistry r;
    r.Add({"project", "Project", 200, 120});
    r.Add({"effects", "Effects", 200, 120});
    r.Add({"history", "History", 180, 100});
    r.Add({"jobs", "Background Jobs", 200, 100});
    r.Add({"program_monitor", "Program Monitor", 320, 220});
    r.Add({"timeline", "Timeline", 400, 160});
    r.Add({"effect_controls", "Effect Controls", 260, 160});
    r.Add({"scopes", "Scopes", 240, 160});
    r.Add({"audio_mixer", "Audio Mixer", 260, 160});
    r.Add({"audio_essentials", "Essential Sound", 260, 200});
    r.Add({"markers", "Markers", 200, 100});
    r.Add({"multicam", "Multicam", 360, 240});
    r.Add({"exports", "Export Queue", 260, 140});
    r.Add({"transcript", "Transcript", 300, 200});
    r.Add({"captions", "Captions", 300, 200});
    r.Add({"graphics", "Titles", 260, 200});
    r.Add({"graphic_designer", "Graphic Designer", 420, 260});
    return r;
  }();
  return registry;
}

// -------------------------------------------------------------------- nodes ----

DockNode DockNode::MakeTabs(std::vector<std::string> panels, int active) {
  DockNode node;
  node.kind = Kind::Tabs;
  node.panels = std::move(panels);
  node.active = active;
  return node;
}

DockNode DockNode::MakeSplit(Orientation orientation, std::vector<DockNode> children, std::vector<double> weights) {
  DockNode node;
  node.kind = Kind::Split;
  node.orientation = orientation;
  node.children = std::move(children);
  node.weights = std::move(weights);
  return node;
}

namespace {

void Collect(const DockNode& node, std::vector<std::string>& out) {
  if (node.kind == DockNode::Kind::Tabs) {
    out.insert(out.end(), node.panels.begin(), node.panels.end());
  } else {
    for (const auto& child : node.children) Collect(child, out);
  }
}

// The tab group holding `panel`, docked or floating.
DockNode* FindTabs(DockNode& node, const std::string& panel) {
  if (node.kind == DockNode::Kind::Tabs) {
    return std::find(node.panels.begin(), node.panels.end(), panel) != node.panels.end() ? &node : nullptr;
  }
  for (auto& child : node.children) {
    if (auto* found = FindTabs(child, panel)) return found;
  }
  return nullptr;
}

DockNode* FindTabs(Layout& layout, const std::string& panel, bool* floating = nullptr) {
  if (auto* found = FindTabs(layout.root, panel)) {
    if (floating != nullptr) *floating = false;
    return found;
  }
  for (auto& window : layout.floating) {
    if (auto* found = FindTabs(window.root, panel)) {
      if (floating != nullptr) *floating = true;
      return found;
    }
  }
  return nullptr;
}

// The split whose child is `target`, and the child's index; null for the root.
DockNode* ParentOf(DockNode& node, const DockNode* target, int& index) {
  if (node.kind != DockNode::Kind::Split) return nullptr;
  for (std::size_t i = 0; i < node.children.size(); ++i) {
    if (&node.children[i] == target) {
      index = static_cast<int>(i);
      return &node;
    }
  }
  for (auto& child : node.children) {
    if (auto* found = ParentOf(child, target, index)) return found;
  }
  return nullptr;
}

void EqualiseWeights(DockNode& node) {
  node.weights.assign(node.children.size(), node.children.empty() ? 0.0 : 1.0 / static_cast<double>(node.children.size()));
}

void NormaliseWeights(DockNode& node) {
  double sum = 0.0;
  for (const auto weight : node.weights) sum += weight > 0.0 ? weight : 0.0;
  if (node.weights.size() != node.children.size() || sum <= 0.0) {
    EqualiseWeights(node);
    return;
  }
  for (auto& weight : node.weights) weight = (weight > 0.0 ? weight : 0.0) / sum;
}

bool NormaliseNode(DockNode& node) {
  if (node.kind == DockNode::Kind::Tabs) {
    if (node.panels.empty()) {
      node.active = 0;
      return true;
    }
    node.active = std::clamp(node.active, 0, static_cast<int>(node.panels.size()) - 1);
    return false;
  }
  if (node.weights.size() != node.children.size()) EqualiseWeights(node);
  std::vector<DockNode> kept;
  std::vector<double> kept_weights;
  for (std::size_t i = 0; i < node.children.size(); ++i) {
    auto& child = node.children[i];
    if (NormaliseNode(child)) continue;
    // A split directly inside a split of the same direction is flattened into it.
    if (child.kind == DockNode::Kind::Split && child.orientation == node.orientation) {
      for (std::size_t j = 0; j < child.children.size(); ++j) {
        kept.push_back(std::move(child.children[j]));
        kept_weights.push_back(node.weights[i] * child.weights[j]);
      }
    } else {
      kept.push_back(std::move(child));
      kept_weights.push_back(node.weights[i]);
    }
  }
  if (kept.empty()) {
    node = DockNode::MakeTabs({});
    return true;
  }
  if (kept.size() == 1) {
    node = std::move(kept.front());
    return false;
  }
  node.children = std::move(kept);
  node.weights = std::move(kept_weights);
  NormaliseWeights(node);
  return false;
}

std::string NodeJson(const DockNode& node) {
  if (node.kind == DockNode::Kind::Tabs) {
    std::vector<std::string> panels;
    for (const auto& panel : node.panels) panels.push_back("\"" + json::Escape(panel) + "\"");
    return json::Object().AddRaw("tabs", json::Array(panels)).Add("active", static_cast<std::int64_t>(node.active)).Build();
  }
  std::vector<std::string> children, weights;
  for (const auto& child : node.children) children.push_back(NodeJson(child));
  for (const auto weight : node.weights) weights.push_back(json::Number(weight));
  return json::Object()
      .Add("split", node.orientation == Orientation::Horizontal ? "horizontal" : "vertical")
      .AddRaw("children", json::Array(children))
      .AddRaw("weights", json::Array(weights))
      .Build();
}

DockNode NodeFromJson(const json::Value& value, int depth) {
  if (!value.is_object()) throw std::invalid_argument("A layout node must be an object");
  if (depth > 32) throw std::invalid_argument("The layout is nested too deeply");
  if (const auto* tabs = value.Find("tabs")) {
    if (!tabs->is_array()) throw std::invalid_argument("A layout tab group needs a list of panels");
    std::vector<std::string> panels;
    for (const auto& item : tabs->items) {
      if (!item.is_string()) throw std::invalid_argument("A panel id must be text");
      panels.push_back(item.text);
    }
    const auto* active = value.Find("active");
    return DockNode::MakeTabs(std::move(panels), active != nullptr && active->is_number() ? static_cast<int>(active->number) : 0);
  }
  const auto& split = value.String("split");
  if (split != "horizontal" && split != "vertical") throw std::invalid_argument("A layout split is horizontal or vertical");
  const auto& children = value.Require("children");
  if (!children.is_array()) throw std::invalid_argument("A layout split needs a list of children");
  std::vector<DockNode> nodes;
  for (const auto& child : children.items) nodes.push_back(NodeFromJson(child, depth + 1));
  std::vector<double> weights;
  if (const auto* w = value.Find("weights"); w != nullptr && w->is_array()) {
    for (const auto& item : w->items) {
      if (!item.is_number()) throw std::invalid_argument("A layout weight must be a number");
      weights.push_back(item.number);
    }
  }
  return DockNode::MakeSplit(split == "horizontal" ? Orientation::Horizontal : Orientation::Vertical, std::move(nodes), std::move(weights));
}

}  // namespace

// --------------------------------------------------------------- operations ----

bool Has(const Layout& layout, const std::string& panel) {
  const auto panels = PanelsOf(layout);
  return std::find(panels.begin(), panels.end(), panel) != panels.end();
}

std::vector<std::string> PanelsOf(const Layout& layout) {
  std::vector<std::string> panels;
  Collect(layout.root, panels);
  for (const auto& window : layout.floating) Collect(window.root, panels);
  return panels;
}

bool operator==(const Layout& a, const Layout& b) { return ToJson(a) == ToJson(b); }

void Normalize(Layout& layout) {
  (void)NormaliseNode(layout.root);
  for (auto& window : layout.floating) {
    if (window.root.kind != DockNode::Kind::Tabs) {
      // A floating window is a tab group: gather whatever it holds into one.
      std::vector<std::string> panels;
      Collect(window.root, panels);
      window.root = DockNode::MakeTabs(std::move(panels));
    }
    (void)NormaliseNode(window.root);
  }
  layout.floating.erase(std::remove_if(layout.floating.begin(), layout.floating.end(), [](const FloatingWindow& w) { return w.root.panels.empty(); }),
                        layout.floating.end());
}

bool Remove(Layout& layout, const std::string& panel) {
  auto* tabs = FindTabs(layout, panel);
  if (tabs == nullptr) return false;
  const auto position = std::find(tabs->panels.begin(), tabs->panels.end(), panel);
  const auto index = static_cast<int>(position - tabs->panels.begin());
  tabs->panels.erase(position);
  if (tabs->active > index || tabs->active >= static_cast<int>(tabs->panels.size())) --tabs->active;
  Normalize(layout);
  return true;
}

bool Dock(Layout& layout, const std::string& panel, const std::string& target, DropZone zone, double share) {
  if (panel.empty() || panel == target || !Has(layout, target)) return false;
  const Layout backup = layout;
  (void)Remove(layout, panel);
  bool floating = false;
  auto* group = FindTabs(layout, target, &floating);
  if (group == nullptr) {
    layout = backup;
    return false;
  }
  if (zone == DropZone::Center) {
    group->panels.push_back(panel);
    group->active = static_cast<int>(group->panels.size()) - 1;
    Normalize(layout);
    return true;
  }
  if (floating) {
    // A floating window is one group; there is nothing to split beside.
    layout = backup;
    return false;
  }
  share = std::clamp(share, 0.05, 0.95);
  const auto orientation = zone == DropZone::Left || zone == DropZone::Right ? Orientation::Horizontal : Orientation::Vertical;
  const bool before = zone == DropZone::Left || zone == DropZone::Top;
  int index = 0;
  auto* parent = ParentOf(layout.root, group, index);
  auto fresh = DockNode::MakeTabs({panel});
  if (parent != nullptr && parent->orientation == orientation) {
    const auto weight = parent->weights[static_cast<std::size_t>(index)];
    parent->weights[static_cast<std::size_t>(index)] = weight * (1.0 - share);
    const auto at = static_cast<std::ptrdiff_t>(before ? index : index + 1);
    parent->children.insert(parent->children.begin() + at, std::move(fresh));
    parent->weights.insert(parent->weights.begin() + at, weight * share);
  } else {
    DockNode existing = std::move(*group);
    std::vector<DockNode> children;
    std::vector<double> weights;
    if (before) {
      children.push_back(std::move(fresh));
      children.push_back(std::move(existing));
      weights = {share, 1.0 - share};
    } else {
      children.push_back(std::move(existing));
      children.push_back(std::move(fresh));
      weights = {1.0 - share, share};
    }
    *group = DockNode::MakeSplit(orientation, std::move(children), std::move(weights));
  }
  Normalize(layout);
  return true;
}

bool Float(Layout& layout, const std::string& panel, double x, double y, double width, double height) {
  if (panel.empty() || width < 80 || height < 60) return false;
  (void)Remove(layout, panel);
  FloatingWindow window;
  window.x = x;
  window.y = y;
  window.width = width;
  window.height = height;
  window.root = DockNode::MakeTabs({panel});
  layout.floating.push_back(std::move(window));
  Normalize(layout);
  return true;
}

bool Show(Layout& layout, const std::string& panel) {
  if (panel.empty()) return false;
  if (Has(layout, panel)) return Activate(layout, panel);
  DockNode* group = nullptr;
  std::function<void(DockNode&)> first = [&](DockNode& node) {
    if (group != nullptr) return;
    if (node.kind == DockNode::Kind::Tabs) {
      if (!node.panels.empty()) group = &node;
    } else {
      for (auto& child : node.children) first(child);
    }
  };
  first(layout.root);
  if (group == nullptr) group = &layout.root;
  if (group->kind != DockNode::Kind::Tabs) return false;
  group->panels.push_back(panel);
  group->active = static_cast<int>(group->panels.size()) - 1;
  Normalize(layout);
  return true;
}

bool Activate(Layout& layout, const std::string& panel) {
  auto* group = FindTabs(layout, panel);
  if (group == nullptr) return false;
  group->active = static_cast<int>(std::find(group->panels.begin(), group->panels.end(), panel) - group->panels.begin());
  return true;
}

bool MoveSplitter(Layout& layout, const NodePath& path, int index, double delta, double minimum) {
  DockNode* node = &layout.root;
  for (const auto step : path) {
    if (node->kind != DockNode::Kind::Split || step < 0 || step >= static_cast<int>(node->children.size())) return false;
    node = &node->children[static_cast<std::size_t>(step)];
  }
  if (node->kind != DockNode::Kind::Split || index < 0 || index + 1 >= static_cast<int>(node->children.size())) return false;
  auto& before = node->weights[static_cast<std::size_t>(index)];
  auto& after = node->weights[static_cast<std::size_t>(index) + 1];
  if (before + after < 2.0 * minimum) return false;
  const auto limited = std::clamp(delta, -(before - minimum), after - minimum);
  before += limited;
  after -= limited;
  return true;
}

std::string Validate(const Layout& layout, const PanelRegistry& registry) {
  const auto panels = PanelsOf(layout);
  std::set<std::string> seen;
  for (const auto& panel : panels) {
    if (registry.Find(panel) == nullptr) return "The layout names the panel " + panel + ", which does not exist";
    if (!seen.insert(panel).second) return "The panel " + panel + " appears more than once";
  }
  std::function<std::string(const DockNode&)> check = [&](const DockNode& node) -> std::string {
    if (node.kind == DockNode::Kind::Tabs) return {};
    if (node.children.size() < 2) return "A split holds fewer than two children";
    if (node.weights.size() != node.children.size()) return "A split's shares do not match its children";
    double sum = 0.0;
    for (const auto weight : node.weights) {
      if (!(weight > 0.0)) return "A split has a share that is not positive";
      sum += weight;
    }
    if (std::abs(sum - 1.0) > 1e-6) return "A split's shares do not add up to one";
    for (const auto& child : node.children) {
      if (auto problem = check(child); !problem.empty()) return problem;
    }
    return {};
  };
  return check(layout.root);
}

std::string ToJson(const Layout& layout) {
  std::vector<std::string> windows;
  for (const auto& window : layout.floating) {
    windows.push_back(json::Object().Add("x", window.x).Add("y", window.y).Add("width", window.width).Add("height", window.height)
                          .AddRaw("root", NodeJson(window.root)).Build());
  }
  return json::Object().Add("version", static_cast<std::int64_t>(1)).AddRaw("root", NodeJson(layout.root)).AddRaw("floating", json::Array(windows)).Build();
}

Layout LayoutFromJson(const std::string& text) {
  const auto root = json::Parse(text);
  if (!root.is_object()) throw std::invalid_argument("A layout must be a JSON object");
  Layout layout;
  layout.root = NodeFromJson(root.Require("root"), 0);
  if (const auto* floating = root.Find("floating"); floating != nullptr && floating->is_array()) {
    for (const auto& item : floating->items) {
      FloatingWindow window;
      window.x = item.Number("x");
      window.y = item.Number("y");
      window.width = item.Number("width");
      window.height = item.Number("height");
      window.root = NodeFromJson(item.Require("root"), 0);
      layout.floating.push_back(std::move(window));
    }
  }
  Normalize(layout);
  return layout;
}

// ------------------------------------------------------------------ geometry ----

namespace {

constexpr double kTabStrip = 28.0;

struct Size {
  double width{0}, height{0};
};

Size MinSizeOf(const DockNode& node, const PanelRegistry& registry, double splitter) {
  if (node.kind == DockNode::Kind::Tabs) {
    Size size{60, 40};
    for (const auto& panel : node.panels) {
      if (const auto* def = registry.Find(panel)) {
        size.width = std::max(size.width, def->min_width);
        size.height = std::max(size.height, def->min_height);
      }
    }
    size.height += kTabStrip;
    return size;
  }
  Size size;
  const bool horizontal = node.orientation == Orientation::Horizontal;
  for (std::size_t i = 0; i < node.children.size(); ++i) {
    const auto child = MinSizeOf(node.children[i], registry, splitter);
    if (horizontal) {
      size.width += child.width + (i > 0 ? splitter : 0.0);
      size.height = std::max(size.height, child.height);
    } else {
      size.height += child.height + (i > 0 ? splitter : 0.0);
      size.width = std::max(size.width, child.width);
    }
  }
  return size;
}

// Shares of `length` for children with the given weights and minimum sizes: proportional to the weights,
// but no child below its minimum unless the minimums together do not fit, in which case they shrink together.
std::vector<double> Distribute(double length, const std::vector<double>& weights, const std::vector<double>& minimums) {
  const auto n = weights.size();
  std::vector<double> sizes(n, 0.0);
  const auto total_min = std::accumulate(minimums.begin(), minimums.end(), 0.0);
  if (total_min >= length) {
    for (std::size_t i = 0; i < n; ++i) sizes[i] = total_min > 0 ? minimums[i] * length / total_min : length / static_cast<double>(n);
    return sizes;
  }
  std::vector<bool> pinned(n, false);
  for (int pass = 0; pass < static_cast<int>(n) + 1; ++pass) {
    double free_length = length, free_weight = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      if (pinned[i]) free_length -= minimums[i];
      else free_weight += weights[i];
    }
    bool changed = false;
    for (std::size_t i = 0; i < n; ++i) {
      if (pinned[i]) {
        sizes[i] = minimums[i];
        continue;
      }
      sizes[i] = free_weight > 0 ? free_length * weights[i] / free_weight : 0.0;
      if (sizes[i] < minimums[i]) {
        pinned[i] = true;
        changed = true;
      }
    }
    if (!changed) break;
  }
  return sizes;
}

void ArrangeNode(const DockNode& node, const Rect& rect, const PanelRegistry& registry, double splitter, NodePath& path, Arrangement& out) {
  if (node.kind == DockNode::Kind::Tabs) {
    TabGroupPlacement group;
    group.path = path;
    group.rect = rect;
    group.tabs = node.panels;
    group.active = node.active;
    out.groups.push_back(std::move(group));
    return;
  }
  const bool horizontal = node.orientation == Orientation::Horizontal;
  const auto n = node.children.size();
  const double available = (horizontal ? rect.width : rect.height) - splitter * static_cast<double>(n - 1);
  std::vector<double> minimums;
  for (const auto& child : node.children) {
    const auto size = MinSizeOf(child, registry, splitter);
    minimums.push_back(horizontal ? size.width : size.height);
  }
  const auto sizes = Distribute(std::max(available, 0.0), node.weights, minimums);
  double cursor = horizontal ? rect.x : rect.y;
  for (std::size_t i = 0; i < n; ++i) {
    Rect child_rect = horizontal ? Rect{cursor, rect.y, sizes[i], rect.height} : Rect{rect.x, cursor, rect.width, sizes[i]};
    path.push_back(static_cast<int>(i));
    ArrangeNode(node.children[i], child_rect, registry, splitter, path, out);
    path.pop_back();
    cursor += sizes[i];
    if (i + 1 < n) {
      SplitterPlacement bar;
      bar.path = path;
      bar.index = static_cast<int>(i);
      bar.orientation = node.orientation;
      bar.extent = std::max(available, 1.0);
      bar.rect = horizontal ? Rect{cursor, rect.y, splitter, rect.height} : Rect{rect.x, cursor, rect.width, splitter};
      out.splitters.push_back(std::move(bar));
      cursor += splitter;
    }
  }
}

}  // namespace

Arrangement Arrange(const Layout& layout, const Rect& bounds, const PanelRegistry& registry, double splitter_thickness) {
  Arrangement arrangement;
  NodePath path;
  ArrangeNode(layout.root, bounds, registry, splitter_thickness, path, arrangement);
  for (std::size_t i = 0; i < layout.floating.size(); ++i) {
    const auto& window = layout.floating[i];
    TabGroupPlacement group;
    group.floating_index = static_cast<int>(i);
    group.rect = {window.x, window.y, window.width, window.height};
    group.tabs = window.root.panels;
    group.active = window.root.active;
    arrangement.groups.push_back(std::move(group));
  }
  return arrangement;
}

// ---------------------------------------------------------------- workspaces ----

namespace {

DockNode T(std::vector<std::string> panels, int active = 0) { return DockNode::MakeTabs(std::move(panels), active); }
DockNode H(std::vector<DockNode> children, std::vector<double> weights) { return DockNode::MakeSplit(Orientation::Horizontal, std::move(children), std::move(weights)); }
DockNode V(std::vector<DockNode> children, std::vector<double> weights) { return DockNode::MakeSplit(Orientation::Vertical, std::move(children), std::move(weights)); }

Layout Make(DockNode root) {
  Layout layout;
  layout.root = std::move(root);
  Normalize(layout);
  return layout;
}

}  // namespace

Layout EditingLayout() {
  return Make(V({H({T({"project", "effects", "history"}), T({"program_monitor"}), T({"effect_controls", "jobs", "exports"})}, {0.22, 0.52, 0.26}),
                 T({"timeline"})},
                {0.56, 0.44}));
}

std::vector<std::pair<std::string, Layout>> BuiltInWorkspaces() {
  std::vector<std::pair<std::string, Layout>> workspaces;
  workspaces.emplace_back("Editing", EditingLayout());
  workspaces.emplace_back("Assembly", Make(H({T({"project", "history"}), V({T({"program_monitor"}), T({"timeline"})}, {0.5, 0.5})}, {0.42, 0.58})));
  workspaces.emplace_back("Color", Make(V({H({T({"program_monitor"}), T({"scopes"}), T({"effect_controls"})}, {0.38, 0.27, 0.35}), T({"timeline"})}, {0.68, 0.32})));
  workspaces.emplace_back("Audio", Make(V({H({T({"project"}), T({"program_monitor"}), T({"audio_essentials"}), T({"audio_mixer"})}, {0.16, 0.28, 0.2, 0.36}), T({"timeline"})}, {0.5, 0.5})));
  workspaces.emplace_back("Multicam", Make(V({H({T({"project", "history"}), T({"multicam"}), T({"program_monitor"})}, {0.2, 0.5, 0.3}), T({"timeline"})}, {0.62, 0.38})));
  workspaces.emplace_back("Text", Make(V({H({T({"project", "history"}), T({"transcript", "captions"}), T({"program_monitor"})}, {0.2, 0.4, 0.4}), T({"timeline"})}, {0.62, 0.38})));
  workspaces.emplace_back("Graphics", Make(V({H({T({"project"}), T({"graphic_designer"}), V({T({"program_monitor"}), T({"graphics"})}, {0.5, 0.5})}, {0.16, 0.48, 0.36}), T({"timeline"})}, {0.66, 0.34})));
  workspaces.emplace_back("Effects", Make(V({H({T({"effects", "project"}), T({"program_monitor"}), T({"effect_controls"})}, {0.25, 0.4, 0.35}), T({"timeline", "markers"})}, {0.58, 0.42})));
  return workspaces;
}

WorkspaceSet::WorkspaceSet() : built_in_(BuiltInWorkspaces()) {
  current_ = built_in_.front().first;
  layout_ = built_in_.front().second;
}

std::vector<std::string> WorkspaceSet::Names() const {
  std::vector<std::string> names;
  for (const auto& [name, layout] : built_in_) names.push_back(name);
  for (const auto& [name, layout] : user_) names.push_back(name);
  return names;
}

const Layout* WorkspaceSet::Saved(const std::string& name) const {
  for (const auto& [saved, layout] : user_) {
    if (saved == name) return &layout;
  }
  for (const auto& [saved, layout] : built_in_) {
    if (saved == name) return &layout;
  }
  return nullptr;
}

bool WorkspaceSet::IsBuiltIn(const std::string& name) const {
  return std::any_of(built_in_.begin(), built_in_.end(), [&](const auto& entry) { return entry.first == name; });
}

bool WorkspaceSet::modified() const {
  const auto* saved = Saved(current_);
  return saved == nullptr || !(*saved == layout_);
}

bool WorkspaceSet::Switch(const std::string& name) {
  const auto* saved = Saved(name);
  if (saved == nullptr) return false;
  current_ = name;
  layout_ = *saved;
  return true;
}

void WorkspaceSet::SaveCurrent() {
  if (IsBuiltIn(current_)) {
    // A built-in is kept as it shipped; a changed one is saved under its own name as a user copy that wins.
    for (auto& [name, layout] : user_) {
      if (name == current_) {
        layout = layout_;
        return;
      }
    }
    user_.emplace_back(current_, layout_);
    return;
  }
  for (auto& [name, layout] : user_) {
    if (name == current_) layout = layout_;
  }
}

bool WorkspaceSet::SaveAs(const std::string& name) {
  if (name.empty() || IsBuiltIn(name)) return false;
  for (auto& [saved, layout] : user_) {
    if (saved == name) {
      layout = layout_;
      current_ = name;
      return true;
    }
  }
  user_.emplace_back(name, layout_);
  current_ = name;
  return true;
}

bool WorkspaceSet::Delete(const std::string& name) {
  const auto found = std::find_if(user_.begin(), user_.end(), [&](const auto& entry) { return entry.first == name; });
  if (found == user_.end()) return false;
  user_.erase(found);
  if (current_ == name) {
    // A built-in that was shadowed by a user copy comes back as it shipped.
    if (!Switch(name)) (void)Switch(built_in_.front().first);
  }
  return true;
}

void WorkspaceSet::ResetCurrent() {
  if (IsBuiltIn(current_)) {
    // Back to the original, discarding any saved user copy of it too.
    user_.erase(std::remove_if(user_.begin(), user_.end(), [&](const auto& entry) { return entry.first == current_; }), user_.end());
  }
  (void)Switch(current_);
}

std::string WorkspaceSet::ToJson() const {
  std::vector<std::string> saved;
  for (const auto& [name, layout] : user_) {
    saved.push_back(json::Object().Add("name", name).AddRaw("layout", ::cutline::ui::ToJson(layout)).Build());
  }
  return json::Object()
      .Add("version", static_cast<std::int64_t>(1))
      .Add("current", current_)
      .AddRaw("live", ::cutline::ui::ToJson(layout_))
      .AddRaw("workspaces", json::Array(saved))
      .Build();
}

void WorkspaceSet::FromJson(const std::string& text, std::vector<std::string>* warnings) {
  const auto note = [&](const std::string& message) {
    if (warnings != nullptr) warnings->push_back(message);
  };
  const auto root = json::Parse(text);
  user_.clear();
  if (const auto* list = root.Find("workspaces"); list != nullptr && list->is_array()) {
    for (const auto& item : list->items) {
      try {
        auto layout = LayoutFromJson(item.Require("layout").raw);
        const auto problem = Validate(layout, BuiltInPanels());
        if (!problem.empty()) {
          note("The workspace " + item.String("name") + " was skipped: " + problem);
          continue;
        }
        user_.emplace_back(item.String("name"), std::move(layout));
      } catch (const std::exception& error) {
        note(std::string("A saved workspace was skipped: ") + error.what());
      }
    }
  }
  current_ = built_in_.front().first;
  layout_ = built_in_.front().second;
  if (const auto* current = root.Find("current"); current != nullptr && current->is_string() && Switch(current->text)) {
    if (const auto* live = root.Find("live"); live != nullptr && live->is_object()) {
      try {
        auto layout = LayoutFromJson(live->raw);
        if (Validate(layout, BuiltInPanels()).empty()) layout_ = std::move(layout);
        else note("The saved window arrangement was not valid and the workspace was restored instead");
      } catch (const std::exception& error) {
        note(std::string("The saved window arrangement could not be read: ") + error.what());
      }
    }
  }
}

}  // namespace cutline::ui
