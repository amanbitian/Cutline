#pragma once

// The arrangement of panels in the window, and the named arrangements (workspaces) a person switches
// between.
//
// A layout is a tree. A split lays its children side by side or one above another, each taking a share
// of the space; a tab group holds panels stacked as tabs with one showing. Floating windows hold a tab
// group of their own. Docking a panel onto the edge of another splits that panel's area; docking onto its
// middle adds a tab. After every change the tree is normalised, so it never holds an empty group, a split
// of one child, or a split directly inside a split of the same direction.
//
// All of it is plain data with no windowing system in it: the same layout gives the same rectangles
// whatever draws them, and it saves and loads as JSON, which is how workspaces persist.

#include <optional>
#include <string>
#include <vector>

namespace cutline::ui {

enum class Orientation { Horizontal, Vertical };  // Horizontal: children side by side
enum class DropZone { Left, Right, Top, Bottom, Center };

struct PanelDef final {
  std::string id;
  std::string title;
  double min_width{120};
  double min_height{80};
};

class PanelRegistry final {
 public:
  void Add(PanelDef panel);
  [[nodiscard]] const PanelDef* Find(const std::string& id) const;
  [[nodiscard]] const std::vector<PanelDef>& All() const { return panels_; }

 private:
  std::vector<PanelDef> panels_;
};

[[nodiscard]] const PanelRegistry& BuiltInPanels();

struct DockNode final {
  enum class Kind { Split, Tabs };
  Kind kind{Kind::Tabs};
  Orientation orientation{Orientation::Horizontal};
  // Split: the children and the share of the space each takes (positive, summing to 1).
  std::vector<DockNode> children;
  std::vector<double> weights;
  // Tabs: the panels, and which one shows.
  std::vector<std::string> panels;
  int active{0};

  [[nodiscard]] static DockNode MakeTabs(std::vector<std::string> panels, int active = 0);
  [[nodiscard]] static DockNode MakeSplit(Orientation orientation, std::vector<DockNode> children, std::vector<double> weights);
};

struct FloatingWindow final {
  double x{100}, y{100}, width{480}, height{360};
  DockNode root;  // a tab group
};

struct Layout final {
  DockNode root;  // an empty tab group when nothing is docked
  std::vector<FloatingWindow> floating;
};

// Positions in the tree: indices from the root through each split's children.
using NodePath = std::vector<int>;

[[nodiscard]] bool Has(const Layout& layout, const std::string& panel);
[[nodiscard]] std::vector<std::string> PanelsOf(const Layout& layout);
[[nodiscard]] bool operator==(const Layout& a, const Layout& b);

// Taking a panel out of wherever it is, and putting it somewhere. Each returns false (changing
// nothing) when it cannot: an unknown panel, a target that is not in the layout, a panel dropped on itself.
bool Remove(Layout& layout, const std::string& panel);
bool Dock(Layout& layout, const std::string& panel, const std::string& target, DropZone zone, double share = 0.3);
bool Float(Layout& layout, const std::string& panel, double x, double y, double width, double height);
// Shows the panel as a tab of the first docked group if it is not in the layout yet; activates it if it is.
bool Show(Layout& layout, const std::string& panel);
bool Activate(Layout& layout, const std::string& panel);
// Moves the boundary after child `index` of the split at `path` by `delta` (a fraction of the split),
// never taking either neighbour below `minimum` of the split.
bool MoveSplitter(Layout& layout, const NodePath& path, int index, double delta, double minimum = 0.05);
// Restores the invariants (see the top of this file). Called by every operation above.
void Normalize(Layout& layout);
// Empty when the layout is sound: every panel known and in exactly one place, every share positive.
[[nodiscard]] std::string Validate(const Layout& layout, const PanelRegistry& registry);

[[nodiscard]] std::string ToJson(const Layout& layout);
// Throws std::invalid_argument for text that is not a layout.
[[nodiscard]] Layout LayoutFromJson(const std::string& json);

// ------------------------------------------------------------------ geometry ----

struct Rect final {
  double x{0}, y{0}, width{0}, height{0};
  [[nodiscard]] bool Contains(double px, double py) const { return px >= x && py >= y && px < x + width && py < y + height; }
};

struct TabGroupPlacement final {
  NodePath path;           // empty for a group that is the whole root; floating groups use floating_index
  int floating_index{-1};  // -1 when docked
  Rect rect;               // the group including its tab strip
  std::vector<std::string> tabs;
  int active{0};
};

struct SplitterPlacement final {
  NodePath path;
  int index{0};  // the boundary after this child
  Orientation orientation{Orientation::Horizontal};
  Rect rect;  // a thin strip the pointer can grab
  // The length the split divides among its children along its axis, so a drag in pixels can become a share.
  double extent{0};
};

struct Arrangement final {
  std::vector<TabGroupPlacement> groups;
  std::vector<SplitterPlacement> splitters;
};

// Lays the docked tree out in `bounds`. Each child gets at least the minimum size of the panels in it where
// the space allows; where it does not, everything shrinks together.
[[nodiscard]] Arrangement Arrange(const Layout& layout, const Rect& bounds, const PanelRegistry& registry, double splitter_thickness = 4.0);

// ---------------------------------------------------------------- workspaces ----

[[nodiscard]] Layout EditingLayout();
[[nodiscard]] std::vector<std::pair<std::string, Layout>> BuiltInWorkspaces();

class WorkspaceSet final {
 public:
  WorkspaceSet();

  [[nodiscard]] std::vector<std::string> Names() const;
  [[nodiscard]] const std::string& current() const { return current_; }
  [[nodiscard]] const Layout& layout() const { return layout_; }
  // The live layout, which a drag-and-drop edits. Differs from the saved workspace once changed.
  [[nodiscard]] Layout& edit() { return layout_; }
  [[nodiscard]] bool modified() const;
  [[nodiscard]] bool IsBuiltIn(const std::string& name) const;

  // Switching discards unsaved changes to the layout you leave, as other editors do; Save first to keep them.
  bool Switch(const std::string& name);
  void SaveCurrent();
  // Keeps the live layout under a new (or existing user) name and makes it current. False for a built-in's name or an empty one.
  bool SaveAs(const std::string& name);
  bool Delete(const std::string& name);
  // Puts the current workspace back to its saved (or, for a built-in, its original) arrangement.
  void ResetCurrent();

  [[nodiscard]] std::string ToJson() const;
  // Unknown or broken workspaces are skipped and described in `warnings`.
  void FromJson(const std::string& json, std::vector<std::string>* warnings = nullptr);

 private:
  [[nodiscard]] const Layout* Saved(const std::string& name) const;

  std::vector<std::pair<std::string, Layout>> built_in_;
  std::vector<std::pair<std::string, Layout>> user_;
  std::string current_;
  Layout layout_;
};

}  // namespace cutline::ui
