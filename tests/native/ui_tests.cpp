// The logic behind the application window: shortcuts, panel layout and workspaces, preferences, and (below)
// the program monitor and the timeline's view and editing rules. None of it needs a window; the Qt layer is a
// thin skin over these.

#include "tests/native/TestHarness.h"

#include "core/project/ProjectStore.h"
#include "timeline/SequenceLoader.h"
#include "timeline/TimelineCompiler.h"
#include "effects/EffectRegistry.h"
#include "ui/EditPlanner.h"
#include "ui/Inspector.h"
#include "ui/Jobs.h"
#include "ui/LutLibrary.h"
#include "ui/Transport.h"
#include "ui/Layout.h"
#include "ui/LookPack.h"
#include "render/CubeLut.h"
#include "ui/MaskEditor.h"
#include "ui/Multicam.h"
#include "ui/Ramp.h"
#include "media/Providers.h"
#include "media/SyntheticSource.h"
#include "timeline/Multicam.h"
#include "ui/Monitor.h"
#include "ui/TimelineView.h"
#include "ui/Preferences.h"
#include "ui/Shortcuts.h"
#include "ui/TextEdit.h"
#include "ui/Transitions.h"
#include "ui/GraphicsDesigner.h"
#include "render/Graphics.h"
#include "render/TextRaster.h"
#include "ui/AudioWorkflow.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <filesystem>
#include <fstream>
#include <set>

using namespace cutline::ui;

namespace {

void Expect(bool condition, const std::string& message, int line) {
  if (!condition) cutline::testing::Fail("expectation", __FILE__, line, message);
}

std::string Owner(const Keymap& keymap, const char* chord) {
  const auto found = keymap.Lookup(*ParseShortcut(chord));
  return found.has_value() ? *found : std::string("(none)");
}

}  // namespace

// ------------------------------------------------------------------- shortcuts ----

CUTLINE_TEST(ShortcutsParseInAnyCaseAndOrderAndPrintCanonically) {
  const auto a = ParseShortcut("shift+ctrl+z");
  CHECK(a.has_value());
  CHECK(a->ctrl && a->shift && !a->alt && a->key == "Z");
  CHECK_EQ(ToString(*a), std::string("Ctrl+Shift+Z"));
  CHECK_EQ(ToString(*ParseShortcut("alt+f4")), std::string("Alt+F4"));
  CHECK_EQ(ToString(*ParseShortcut("SPACE")), std::string("Space"));
  CHECK_EQ(ToString(*ParseShortcut("Ctrl + Comma")), std::string("Ctrl+Comma"));
  for (const auto* bad : {"", "Ctrl+", "Ctrl+Shift", "Hyper+A", "F25", "F0", "Ctrl+Ab", "++", "Ctrl++"}) {
    CHECK(!ParseShortcut(bad).has_value());
  }
  // Every default of every built-in command is a chord that parses back to itself.
  for (const auto& command : BuiltInCommands().All()) {
    for (const auto& shortcut : command.defaults) {
      const auto round = ParseShortcut(ToString(shortcut));
      CHECK(round.has_value() && *round == shortcut);
    }
  }
}

CUTLINE_TEST(TheBuiltInCommandsAreUniqueHaveNoSharedDefaultAndCanBeSearched) {
  const auto& registry = BuiltInCommands();
  std::set<std::string> ids;
  std::set<Shortcut> chords;
  for (const auto& command : registry.All()) {
    CHECK(ids.insert(command.id).second);
    CHECK(!command.label.empty() && !command.category.empty());
    for (const auto& shortcut : command.defaults) CHECK(chords.insert(shortcut).second);   // no chord is the default of two commands
  }
  CHECK(registry.All().size() > 80);
  CHECK(registry.Find("transport.play_pause") != nullptr);
  CHECK(registry.Find("nothing") == nullptr);
  CHECK(registry.Categories().size() >= 7);
  // The editing keys people expect.
  Keymap keymap(registry);
  CHECK_EQ(Owner(keymap, "Space"), std::string("transport.play_pause"));
  CHECK_EQ(Owner(keymap, "J"), std::string("transport.play_reverse"));
  CHECK_EQ(Owner(keymap, "K"), std::string("transport.stop"));
  CHECK_EQ(Owner(keymap, "L"), std::string("transport.play_forward"));
  CHECK_EQ(Owner(keymap, "Ctrl+K"), std::string("timeline.add_edit"));
  CHECK_EQ(Owner(keymap, "Ctrl+Z"), std::string("edit.undo"));
  CHECK_EQ(Owner(keymap, "Ctrl+Y"), std::string("edit.redo"));
  CHECK_EQ(Owner(keymap, "Comma"), std::string("timeline.insert"));
  CHECK_EQ(Owner(keymap, "Period"), std::string("timeline.overwrite"));
  CHECK(!keymap.Lookup(*ParseShortcut("Ctrl+Alt+Shift+Q")).has_value());
  // Search: the label first, then anything else that mentions every word.
  const auto found = registry.Search("add edit");
  CHECK(!found.empty() && found.front()->id == "timeline.add_edit");
  CHECK(registry.Search("zzzzzz").empty());
  CHECK_EQ(registry.Search("").size(), registry.All().size());
  // A registry refuses duplicates and bad defaults.
  CommandRegistry fresh;
  fresh.Add({"a", "A", "Cat", "", {}});
  CHECK_THROWS(fresh.Add({"a", "Again", "Cat", "", {}}));
  CHECK_THROWS(fresh.Add({"", "No id", "Cat", "", {}}));
  CHECK_THROWS(fresh.Add({"b", "B", "Cat", "", {Shortcut{false, false, false, "nonsense"}}}));
}

CUTLINE_TEST(BindingAChordTakesItFromItsOwnerAndOnlyTheDifferencesAreSaved) {
  const auto& registry = BuiltInCommands();
  Keymap keymap(registry);
  CHECK(keymap.IsDefault());
  CHECK_EQ(Keymap(registry).ToJson(), keymap.ToJson());
  // Giving J's chord to Add Edit takes it from the shuttle.
  const auto displaced = keymap.Bind("timeline.add_edit", *ParseShortcut("J"));
  CHECK_EQ(displaced.size(), std::size_t{1});
  CHECK_EQ(displaced[0], std::string("transport.play_reverse"));
  CHECK_EQ(Owner(keymap, "J"), std::string("timeline.add_edit"));
  CHECK(keymap.ShortcutsFor("transport.play_reverse").empty());
  CHECK_EQ(keymap.ShortcutsFor("timeline.add_edit").size(), std::size_t{2});   // its own Ctrl+K and the new one
  CHECK(!keymap.IsDefault());
  // Binding what a command already has changes nothing.
  CHECK(keymap.Bind("timeline.add_edit", *ParseShortcut("J")).empty());
  CHECK_EQ(keymap.ShortcutsFor("timeline.add_edit").size(), std::size_t{2});
  CHECK_THROWS(keymap.Bind("no.such.command", *ParseShortcut("Q")));
  CHECK_THROWS(keymap.Bind("edit.undo", Shortcut{}));

  // Saved: only the two commands that differ.
  const auto saved = keymap.ToJson();
  Keymap loaded(registry);
  std::vector<std::string> warnings;
  loaded.FromJson(saved, &warnings);
  CHECK(warnings.empty());
  CHECK_EQ(Owner(loaded, "J"), std::string("timeline.add_edit"));
  CHECK(loaded.ShortcutsFor("transport.play_reverse").empty());
  CHECK_EQ(loaded.ToJson(), saved);
  CHECK(saved.find("transport.play_pause") == std::string::npos);   // untouched commands are not written

  // A swap in one file works whatever order the entries are in.
  Keymap swapped(registry);
  swapped.FromJson("{\"changes\":[{\"command\":\"transport.play_reverse\",\"shortcuts\":[\"L\"]},{\"command\":\"transport.play_forward\",\"shortcuts\":[\"J\"]}]}");
  CHECK_EQ(Owner(swapped, "L"), std::string("transport.play_reverse"));
  CHECK_EQ(Owner(swapped, "J"), std::string("transport.play_forward"));

  // Unbind, clear, reset.
  loaded.Unbind("timeline.add_edit", *ParseShortcut("J"));
  CHECK(!loaded.Lookup(*ParseShortcut("J")).has_value());
  loaded.Clear("timeline.add_edit");
  CHECK(loaded.ShortcutsFor("timeline.add_edit").empty());
  loaded.Reset("timeline.add_edit");
  CHECK_EQ(loaded.ShortcutsFor("timeline.add_edit").size(), std::size_t{1});
  loaded.Reset("transport.play_reverse");
  loaded.ResetAll();
  CHECK(loaded.IsDefault());

  // A file from another build: unknown commands and bad chords are skipped and said so, the rest applies.
  Keymap tolerant(registry);
  std::vector<std::string> notes;
  tolerant.FromJson("{\"changes\":[{\"command\":\"future.thing\",\"shortcuts\":[\"Q\"]},{\"command\":\"edit.undo\",\"shortcuts\":[\"Ctrl+Z\",\"Banana\"]},{\"nonsense\":1}]}", &notes);
  CHECK_EQ(notes.size(), std::size_t{3});
  CHECK_EQ(Owner(tolerant, "Ctrl+Z"), std::string("edit.undo"));
}

// ---------------------------------------------------------------------- layout ----

namespace {

std::set<std::string> Names(const Layout& layout) {
  const auto panels = PanelsOf(layout);
  return {panels.begin(), panels.end()};
}

}  // namespace

CUTLINE_TEST(EveryBuiltInWorkspaceIsAValidLayoutAndArrangesWithinTheWindow) {
  const auto& registry = BuiltInPanels();
  for (const auto& [name, layout] : BuiltInWorkspaces()) {
    const auto problem = Validate(layout, registry);
    Expect(problem.empty(), name + ": " + problem, __LINE__);
    CHECK(Has(layout, "timeline") && Has(layout, "program_monitor"));
    const Rect window{0, 0, 1920, 1080};
    const auto arrangement = Arrange(layout, window, registry);
    double area = 0.0;
    for (const auto& group : arrangement.groups) {
      // Inside the window, and no smaller than its panels need.
      CHECK(group.rect.x >= -1e-9 && group.rect.y >= -1e-9 && group.rect.x + group.rect.width <= 1920 + 1e-6 && group.rect.y + group.rect.height <= 1080 + 1e-6);
      for (const auto& tab : group.tabs) {
        const auto* def = registry.Find(tab);
        CHECK(def != nullptr);
        if (def != nullptr) Expect(group.rect.width >= def->min_width - 1e-6 && group.rect.height >= def->min_height - 1e-6, name + ": " + tab + " is too small", __LINE__);
      }
      area += group.rect.width * group.rect.height;
    }
    // The groups and the splitters between them fill the window.
    double bars = 0.0;
    for (const auto& bar : arrangement.splitters) bars += bar.rect.width * bar.rect.height;
    CHECK(std::abs(area + bars - 1920.0 * 1080.0) < 1.0);
    // Round trips through JSON unchanged.
    CHECK(LayoutFromJson(ToJson(layout)) == layout);
  }
  CHECK_EQ(BuiltInWorkspaces().size(), std::size_t{8});
}

CUTLINE_TEST(DockingAPanelSplitsTabsOrFloatsAndEveryOperationLeavesATidyTree) {
  const auto& registry = BuiltInPanels();
  auto layout = EditingLayout();
  const auto before = Names(layout);

  // Onto the edge of the monitor: a split beside it, the monitor keeping 70% of what it had.
  CHECK(Dock(layout, "scopes", "program_monitor", DropZone::Right, 0.3));
  CHECK(Validate(layout, registry).empty());
  CHECK(Has(layout, "scopes"));
  auto arranged = Arrange(layout, {0, 0, 2000, 1000}, registry);
  Rect monitor{}, scopes{};
  for (const auto& group : arranged.groups) {
    for (const auto& tab : group.tabs) {
      if (tab == "program_monitor") monitor = group.rect;
      if (tab == "scopes") scopes = group.rect;
    }
  }
  CHECK(scopes.x >= monitor.x + monitor.width);                     // to the right of it
  CHECK(std::abs(scopes.y - monitor.y) < 1e-6 && std::abs(scopes.height - monitor.height) < 1e-6);
  CHECK(std::abs(scopes.width / (scopes.width + monitor.width) - 0.3) < 0.02);

  // Into the middle of another panel: a tab, and it shows.
  CHECK(Dock(layout, "markers", "timeline", DropZone::Center));
  const auto timeline_group = Arrange(layout, {0, 0, 2000, 1000}, registry);
  bool tabbed = false;
  for (const auto& group : timeline_group.groups) {
    if (std::find(group.tabs.begin(), group.tabs.end(), "timeline") != group.tabs.end()) {
      tabbed = group.tabs.size() == 2 && group.tabs[static_cast<std::size_t>(group.active)] == "markers";
    }
  }
  CHECK(tabbed);

  // Moving a panel out of a group that then has one panel leaves a plain group, and out of the last panel of a group removes the group.
  CHECK(Dock(layout, "project", "timeline", DropZone::Top));
  CHECK(Validate(layout, registry).empty());
  CHECK(Remove(layout, "markers"));
  CHECK(!Has(layout, "markers"));
  CHECK(Validate(layout, registry).empty());

  // Floating, and back.
  CHECK(Float(layout, "history", 200, 150, 500, 400));
  CHECK_EQ(layout.floating.size(), std::size_t{1});
  CHECK(Has(layout, "history") && Validate(layout, registry).empty());
  CHECK(!Dock(layout, "effects", "history", DropZone::Left));   // nothing to split beside in a floating window
  CHECK(Has(layout, "effects"));                                // and the failed attempt left it where it was
  CHECK(Dock(layout, "history", "effect_controls", DropZone::Center));
  CHECK(layout.floating.empty());                               // the empty window went away

  // What cannot be done is refused and changes nothing.
  const auto snapshot = ToJson(layout);
  CHECK(!Dock(layout, "timeline", "timeline", DropZone::Left));
  CHECK(!Dock(layout, "timeline", "no_such_panel", DropZone::Left));
  CHECK(!Remove(layout, "no_such_panel"));
  CHECK(!Float(layout, "timeline", 0, 0, 10, 10));
  CHECK_EQ(ToJson(layout), snapshot);

  // Every panel is still in exactly one place.
  const auto after = Names(layout);
  for (const auto& panel : before) CHECK(after.count(panel) == 1);
  CHECK_EQ(PanelsOf(layout).size(), after.size());

  // Show brings back what was closed, as a tab of the first group; showing what is there activates it.
  CHECK(Remove(layout, "scopes"));
  CHECK(Show(layout, "scopes"));
  CHECK(Has(layout, "scopes"));
  CHECK(Show(layout, "scopes") && Activate(layout, "scopes"));
  CHECK(!Activate(layout, "nonexistent"));
}

CUTLINE_TEST(DraggingASplitterMovesTheBoundaryWithinLimitsAndTheLayoutKeepsItsShape) {
  auto layout = EditingLayout();
  const auto& registry = BuiltInPanels();
  const auto first = Arrange(layout, {0, 0, 2000, 1000}, registry);
  CHECK(!first.splitters.empty());
  // The vertical boundary between the top row and the timeline is the root's first splitter.
  const SplitterPlacement* root_bar = nullptr;
  for (const auto& bar : first.splitters) {
    if (bar.path.empty()) root_bar = &bar;
  }
  CHECK(root_bar != nullptr);
  if (root_bar == nullptr) return;
  CHECK(root_bar->orientation == Orientation::Vertical);
  const auto before = root_bar->rect.y;
  CHECK(MoveSplitter(layout, root_bar->path, root_bar->index, 0.1));
  const auto second = Arrange(layout, {0, 0, 2000, 1000}, registry);
  for (const auto& bar : second.splitters) {
    if (bar.path.empty()) CHECK(std::abs((bar.rect.y - before) - 100.0) < 8.0);
  }
  // It cannot be pushed past a neighbour.
  CHECK(MoveSplitter(layout, {}, 0, 5.0));
  CHECK(Validate(layout, registry).empty());
  const auto squeezed = Arrange(layout, {0, 0, 2000, 1000}, registry);
  for (const auto& group : squeezed.groups) CHECK(group.rect.height > 0.0);
  CHECK(!MoveSplitter(layout, {7, 7}, 0, 0.1));   // no such place
  CHECK(!MoveSplitter(layout, {}, 5, 0.1));

  // A window too small for the panels still lays out: everything shrinks together instead of overlapping.
  const auto tiny = Arrange(EditingLayout(), {0, 0, 500, 300}, registry);
  double covered = 0.0;
  for (const auto& group : tiny.groups) covered += group.rect.width * group.rect.height;
  CHECK(covered <= 500.0 * 300.0 + 1.0);
}

CUTLINE_TEST(LayoutsRoundTripAsJsonAndBadOnesAreRefusedOrRepaired) {
  auto layout = EditingLayout();
  CHECK(Dock(layout, "scopes", "timeline", DropZone::Bottom));
  CHECK(Float(layout, "markers", 10, 20, 300, 200));
  CHECK(LayoutFromJson(ToJson(layout)) == layout);
  CHECK_THROWS(LayoutFromJson("not json"));
  CHECK_THROWS(LayoutFromJson("[]"));
  CHECK_THROWS(LayoutFromJson("{\"root\":{\"split\":\"diagonal\",\"children\":[]}}"));
  CHECK_THROWS(LayoutFromJson("{\"root\":{\"tabs\":[1]}}"));
  // Shares that are missing or nonsense are repaired to equal ones rather than refused.
  const auto repaired = LayoutFromJson("{\"root\":{\"split\":\"horizontal\",\"children\":[{\"tabs\":[\"timeline\"]},{\"tabs\":[\"project\"]}],\"weights\":[0,-1]}}");
  CHECK(Validate(repaired, BuiltInPanels()).empty());
  CHECK(std::abs(repaired.root.weights[0] - 0.5) < 1e-9);
  // A layout naming a panel nobody has, or one twice, does not validate.
  CHECK(!Validate(LayoutFromJson("{\"root\":{\"tabs\":[\"timeline\",\"ghost\"]}}"), BuiltInPanels()).empty());
  CHECK(!Validate(LayoutFromJson("{\"root\":{\"split\":\"horizontal\",\"children\":[{\"tabs\":[\"timeline\"]},{\"tabs\":[\"timeline\"]}]}}"), BuiltInPanels()).empty());
  // Empty and single-child splits are tidied away when read.
  const auto tidy = LayoutFromJson("{\"root\":{\"split\":\"vertical\",\"children\":[{\"tabs\":[]},{\"split\":\"horizontal\",\"children\":[{\"tabs\":[\"timeline\"]}]}]}}");
  CHECK(tidy.root.kind == DockNode::Kind::Tabs && tidy.root.panels.size() == 1);
}

CUTLINE_TEST(WorkspacesSwitchSaveResetAndComeBackAfterARestart) {
  WorkspaceSet set;
  CHECK_EQ(set.current(), std::string("Editing"));
  CHECK(!set.modified());
  const auto built_in_count = BuiltInWorkspaces().size();
  CHECK_EQ(set.Names().size(), built_in_count);
  // Edit the live layout: it is modified; switching away discards it; resetting puts it back.
  CHECK(Dock(set.edit(), "scopes", "program_monitor", DropZone::Right));
  CHECK(set.modified());
  CHECK(set.layout() == set.layout() && Has(set.layout(), "scopes"));
  set.ResetCurrent();
  CHECK(!set.modified() && !Has(set.layout(), "scopes"));
  CHECK(Dock(set.edit(), "scopes", "program_monitor", DropZone::Right));
  CHECK(set.Switch("Color"));
  CHECK(set.current() == "Color" && !set.modified());
  CHECK(set.Switch("Editing") && !Has(set.layout(), "scopes"));
  CHECK(!set.Switch("Nonexistent"));

  // Save as a name of your own; a built-in's name cannot be taken.
  CHECK(Dock(set.edit(), "markers", "timeline", DropZone::Center));
  CHECK(!set.SaveAs("Editing"));
  CHECK(!set.SaveAs(""));
  CHECK(set.SaveAs("Mine"));
  CHECK_EQ(set.current(), std::string("Mine"));
  CHECK(!set.modified() && set.IsBuiltIn("Editing") && !set.IsBuiltIn("Mine"));
  CHECK_EQ(set.Names().size(), built_in_count + 1);
  CHECK(Dock(set.edit(), "history", "project", DropZone::Center));
  CHECK(set.modified());
  set.SaveCurrent();
  CHECK(!set.modified());

  // Saved, and read back by a new set, with the live arrangement as it was left.
  CHECK(Float(set.edit(), "jobs", 100, 100, 400, 300));
  const auto saved = set.ToJson();
  WorkspaceSet reloaded;
  std::vector<std::string> warnings;
  reloaded.FromJson(saved, &warnings);
  CHECK(warnings.empty());
  CHECK_EQ(reloaded.current(), std::string("Mine"));
  CHECK(reloaded.layout() == set.layout());
  CHECK(reloaded.modified());
  CHECK(reloaded.Switch("Mine") && !reloaded.modified());
  CHECK(Has(reloaded.layout(), "markers"));

  // A saved built-in that was changed shadows the original until it is reset, which restores the original.
  CHECK(reloaded.Switch("Audio"));
  CHECK(Remove(reloaded.edit(), "audio_mixer"));
  reloaded.SaveCurrent();
  CHECK(reloaded.Switch("Editing") && reloaded.Switch("Audio") && !Has(reloaded.layout(), "audio_mixer"));
  reloaded.ResetCurrent();
  CHECK(Has(reloaded.layout(), "audio_mixer"));

  // Deleting a user workspace; the built-ins stay.
  CHECK(reloaded.Delete("Mine"));
  CHECK(!reloaded.Delete("Mine"));
  CHECK(!reloaded.Delete("Editing"));
  CHECK_EQ(reloaded.Names().size(), built_in_count);

  // A damaged saved file does not stop anything: the broken part is skipped and said so.
  WorkspaceSet tolerant;
  std::vector<std::string> notes;
  tolerant.FromJson("{\"current\":\"Ghost\",\"workspaces\":[{\"name\":\"Bad\",\"layout\":{\"root\":{\"tabs\":[\"ghost\"]}}},{\"name\":\"Worse\",\"layout\":5}]}", &notes);
  CHECK_EQ(notes.size(), std::size_t{2});
  CHECK_EQ(tolerant.current(), std::string("Editing"));
}

// ----------------------------------------------------------------- preferences ----

CUTLINE_TEST(PreferencesAreDeclaredTypedValidatedObservedAndOnlyDifferencesAreSaved) {
  Preferences prefs;
  CHECK(prefs.GetBool("timeline.snap"));
  CHECK_EQ(prefs.GetInt("timeline.snap_distance_px"), std::int64_t{8});
  CHECK(std::abs(prefs.GetReal("monitor.action_safe_percent") - 93.0) < 1e-12);
  CHECK_EQ(prefs.GetText("playback.default_quality"), std::string("auto"));
  CHECK_EQ(prefs.ToJson().find("timeline.snap"), std::string::npos);   // defaults are not written

  std::vector<std::pair<std::string, PreferenceValue>> seen;
  const auto id = prefs.Observe([&](const std::string& key, const PreferenceValue& value) { seen.emplace_back(key, value); });
  prefs.Set("timeline.snap", false);
  prefs.Set("timeline.snap", false);                          // no change, no notification
  prefs.Set("timeline.snap_distance_px", std::int64_t{12});
  prefs.Set("monitor.title_safe_percent", std::int64_t{85});  // a whole number for a real preference
  CHECK_EQ(seen.size(), std::size_t{3});
  CHECK(std::abs(prefs.GetReal("monitor.title_safe_percent") - 85.0) < 1e-12);
  prefs.Unobserve(id);
  prefs.Set("timeline.snap", true);
  CHECK_EQ(seen.size(), std::size_t{3});

  // Refused: unknown keys, wrong kinds, out of range, not a choice, not finite. Nothing changes.
  CHECK_THROWS(prefs.Set("no.such.key", true));
  CHECK_THROWS(prefs.Set("timeline.snap", std::string("yes")));
  CHECK_THROWS(prefs.Set("timeline.snap_distance_px", std::int64_t{99}));
  CHECK_THROWS(prefs.Set("timeline.snap_distance_px", std::int64_t{0}));
  CHECK_THROWS(prefs.Set("appearance.theme", std::string("neon")));
  CHECK_THROWS(prefs.Set("appearance.ui_scale", std::nan("")));
  CHECK_THROWS(prefs.Set("appearance.ui_scale", 5.0));
  CHECK_EQ(prefs.GetInt("timeline.snap_distance_px"), std::int64_t{12});
  CHECK_THROWS((void)prefs.Get("no.such.key"));

  // The settings page can be built from the declaration alone.
  CHECK(prefs.schema().Categories().size() >= 6);
  for (const auto& def : prefs.schema().All()) CHECK(!def.label.empty() && !def.category.empty());

  // Round trip.
  prefs.Set("appearance.theme", std::string("light"));
  const auto text = prefs.ToJson();
  Preferences loaded;
  std::vector<std::string> warnings;
  loaded.FromJson(text, &warnings);
  CHECK(warnings.empty());
  CHECK_EQ(loaded.GetText("appearance.theme"), std::string("light"));
  CHECK_EQ(loaded.GetInt("timeline.snap_distance_px"), std::int64_t{12});
  CHECK(loaded.IsDefault("general.confirm_quit") && !loaded.IsDefault("appearance.theme"));
  loaded.Reset("appearance.theme");
  CHECK(loaded.IsDefault("appearance.theme"));
  loaded.ResetAll();
  CHECK_EQ(loaded.ToJson().find("timeline"), std::string::npos);

  // A file from elsewhere: bad values fall back to their defaults and are said so; unknown keys are skipped.
  Preferences tolerant;
  std::vector<std::string> notes;
  tolerant.FromJson("{\"values\":{\"timeline.snap_distance_px\":500,\"appearance.theme\":\"neon\",\"timeline.snap\":3,\"future.key\":1,\"playback.frame_cache_mb\":512.5,\"timeline.video_track_height\":80}}", &notes);
  CHECK_EQ(notes.size(), std::size_t{5});
  CHECK_EQ(tolerant.GetInt("timeline.snap_distance_px"), std::int64_t{8});
  CHECK_EQ(tolerant.GetInt("timeline.video_track_height"), std::int64_t{80});
}

CUTLINE_TEST(PreferencesAreSavedAtomicallyToAFileAndADamagedFileFallsBackToDefaults) {
  const auto directory = std::filesystem::temp_directory_path() / "cutline-test-prefs";
  std::filesystem::remove_all(directory);
  const auto path = (directory / "nested" / "preferences.json").string();
  Preferences prefs;
  CHECK(!prefs.Load(path));                       // no file yet
  prefs.Set("timeline.snap_distance_px", std::int64_t{20});
  prefs.Save(path);
  CHECK(std::filesystem::exists(path));
  CHECK(!std::filesystem::exists(path + ".tmp"));
  Preferences again;
  CHECK(again.Load(path));
  CHECK_EQ(again.GetInt("timeline.snap_distance_px"), std::int64_t{20});
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "{ this is not json";
  }
  Preferences broken;
  broken.Set("timeline.snap_distance_px", std::int64_t{3});
  std::vector<std::string> warnings;
  CHECK(broken.Load(path, &warnings));
  CHECK_EQ(broken.GetInt("timeline.snap_distance_px"), std::int64_t{8});
  CHECK_EQ(warnings.size(), std::size_t{1});
  std::filesystem::remove_all(directory);
}

// -------------------------------------------------------- timeline editing rules ----

namespace {

namespace cmd = cutline::commands;
using cutline::time::RationalTime;
using cutline::timeline::Sequence;

RationalTime S(std::int64_t seconds) { return {seconds, 1}; }
RationalTime Fr(std::int64_t frames) { return RationalTime::FromFrames(frames, cutline::time::kFrameRate25); }

// A project with two video tracks, two audio tracks and a sequence at 25 fps, edited only through plans.
class Project final {
 public:
  Project() : store_(":memory:") {
    store_.Initialize();
    Run(cmd::CommandType::CreateProject, cmd::CreateProjectPayload{"UI"});
    for (const auto* id : {"m1", "m2"}) {
      cmd::ImportMediaPayload media;
      media.id = id;
      media.display_name = id;
      media.original_path = std::string("/") + id;
      media.fingerprint = std::string("fp-") + id;
      media.duration = S(60);
      Run(cmd::CommandType::ImportMedia, media);
    }
    cmd::CreateSequencePayload sequence;
    sequence.id = "seq";
    sequence.settings.name = "Main";
    sequence.settings.frame_rate = {25, 1};
    sequence.settings.width = 1920;
    sequence.settings.height = 1080;
    sequence.settings.sample_rate = 48000;
    Run(cmd::CommandType::CreateSequence, sequence);
    Run(cmd::CommandType::AddVideoTrack, cmd::AddTrackPayload{"v1", "seq", 0, "stereo", "V1"});
    Run(cmd::CommandType::AddVideoTrack, cmd::AddTrackPayload{"v2", "seq", 1, "stereo", "V2"});
    Run(cmd::CommandType::AddAudioTrack, cmd::AddTrackPayload{"a1", "seq", 0, "stereo", "A1"});
    Run(cmd::CommandType::AddAudioTrack, cmd::AddTrackPayload{"a2", "seq", 1, "stereo", "A2"});
  }

  cutline::commands::CommandEnvelope Envelope(cmd::CommandType type, cmd::CommandPayload payload) {
    cmd::CommandEnvelope command;
    command.command_id = "cmd-" + std::to_string(++counter_);
    command.project_id = "project-ui";
    command.author_id = "tester";
    command.base_revision = store_.CurrentRevision();
    command.timestamp_utc = "2026-10-06T00:00:00Z";
    command.type = type;
    command.payload = std::move(payload);
    command.idempotency_key = "key-" + std::to_string(counter_);
    return command;
  }

  void Run(cmd::CommandType type, cmd::CommandPayload payload) {
    auto command = Envelope(type, std::move(payload));
    (void)store_.Execute(command);
  }

  void Add(const std::string& id, const std::string& track, std::int64_t start, std::int64_t in, std::int64_t out, const std::string& group = "") {
    cmd::InsertClipPayload clip;
    clip.id = id;
    clip.track_id = track;
    clip.media_id = "m1";
    clip.source_in = S(in);
    clip.source_out = S(out);
    clip.timeline_start = S(start);
    clip.linked_group = group;
    clip.name = id;
    Run(cmd::CommandType::InsertClip, clip);
  }

  Sequence Snapshot() const { return cutline::timeline::LoadSequenceGraph(store_, "seq").sequences.front(); }

  cutline::ui::EditContext Context(const Sequence& sequence) {
    cutline::ui::EditContext ctx;
    ctx.sequence = &sequence;
    ctx.new_id = [this](const std::string& prefix) { return prefix + "-n" + std::to_string(++ids_); };
    ctx.media_duration = [](const std::string&) -> std::optional<RationalTime> { return RationalTime(60, 1); };
    return ctx;
  }

  // Runs a plan as one undoable step. False (and nothing changed) if the plan was refused or failed.
  bool Apply(const cutline::ui::EditPlan& plan) {
    if (!plan.ok) return false;
    cutline::ui::EnvelopeFactory factory;
    factory.project_id = "project-ui";
    factory.author_id = "tester";
    factory.timestamp_utc = "2026-10-06T00:00:00Z";
    factory.key_prefix = "plan" + std::to_string(++plans_);
    factory.base_revision = store_.CurrentRevision();
    try {
      (void)store_.ExecuteGroup(cutline::ui::ToEnvelopes(plan, factory, 1), plan.label);
    } catch (const std::exception& error) {
      last_error = error.what();
      return false;
    }
    return true;
  }

  int NextId() { return ++ids_; }
  cutline::project::ProjectStore& store() { return store_; }
  std::string last_error;

 private:
  cutline::project::ProjectStore store_;
  int counter_{0};
  int ids_{0};
  int plans_{0};
};

const cutline::timeline::Clip* Find(const Sequence& sequence, const std::string& id) {
  for (const auto& track : sequence.tracks) {
    for (const auto& clip : track.clips) {
      if (clip.id == id) return &clip;
    }
  }
  return nullptr;
}

std::string TrackOf(const Sequence& sequence, const std::string& id) {
  for (const auto& track : sequence.tracks) {
    for (const auto& clip : track.clips) {
      if (clip.id == id) return track.id;
    }
  }
  return "(none)";
}

bool StartsAt(const Sequence& sequence, const std::string& id, const RationalTime& at) {
  const auto* clip = Find(sequence, id);
  return clip != nullptr && clip->timeline_start.Compare(at) == 0;
}
bool EndsAt(const Sequence& sequence, const std::string& id, const RationalTime& at) {
  const auto* clip = Find(sequence, id);
  return clip != nullptr && clip->end().Compare(at) == 0;
}
bool Source(const Sequence& sequence, const std::string& id, const RationalTime& in, const RationalTime& out) {
  const auto* clip = Find(sequence, id);
  return clip != nullptr && clip->source_in.Compare(in) == 0 && clip->source_out.Compare(out) == 0;
}
std::size_t Count(const Sequence& sequence, const std::string& track) {
  const auto* t = sequence.FindTrack(track);
  return t == nullptr ? 0 : t->clips.size();
}

}  // namespace

CUTLINE_TEST(MovingClipsOverwritesRefusesOrInsertsAsAskedAndIsOneUndoStep) {
  Project project;
  project.Add("a", "v1", 0, 0, 5);
  project.Add("b", "v1", 5, 10, 15);
  project.Add("c", "v1", 20, 20, 25);
  auto snap = project.Snapshot();
  const auto steps = project.store().AppliedStepCount();

  // Into free space.
  auto plan = cutline::ui::PlanMove(project.Context(snap), {"c"}, "c", S(-3), 0, cutline::ui::OverlapMode::Refuse);
  CHECK(plan.ok);
  CHECK(project.Apply(plan));
  snap = project.Snapshot();
  CHECK(StartsAt(snap, "c", S(17)));
  CHECK_EQ(project.store().AppliedStepCount(), steps + 1);

  // Onto a clip: refused in strict mode, overwriting in overwrite mode (b's head is cut away).
  auto refused = cutline::ui::PlanMove(project.Context(snap), {"c"}, "c", S(-10), 0, cutline::ui::OverlapMode::Refuse);
  CHECK(!refused.ok && !refused.refusal.empty());
  plan = cutline::ui::PlanMove(project.Context(snap), {"c"}, "c", S(-10), 0, cutline::ui::OverlapMode::Overwrite);
  CHECK(plan.ok);
  CHECK(project.Apply(plan));
  snap = project.Snapshot();
  CHECK(StartsAt(snap, "c", S(7)) && EndsAt(snap, "c", S(12)));
  CHECK(EndsAt(snap, "b", S(7)));                              // b was shortened at its end, where c now begins
  CHECK(Source(snap, "b", S(10), S(12)));
  CHECK(StartsAt(snap, "a", S(0)));
  CHECK_NO_THROW(project.store().ValidateDatabase());
  // Undo takes the whole gesture back, including the trim of b.
  (void)project.store().Undo("tester", "2026-10-06T00:00:01Z");
  snap = project.Snapshot();
  CHECK(StartsAt(snap, "c", S(17)) && EndsAt(snap, "b", S(10)));

  // Stopped at the start of the sequence.
  plan = cutline::ui::PlanMove(project.Context(snap), {"a"}, "a", S(-4), 0, cutline::ui::OverlapMode::Refuse);
  CHECK(!plan.ok);   // a is already at zero: nothing to do
  plan = cutline::ui::PlanMove(project.Context(snap), {"b"}, "b", S(-7), 0, cutline::ui::OverlapMode::Overwrite);
  CHECK(plan.ok && !plan.notes.empty());
  CHECK(project.Apply(plan));
  snap = project.Snapshot();
  CHECK(StartsAt(snap, "b", S(0)));

  // Insert: the moved clip goes in at 2 s and what was there is cut and pushed along.
  Project insert;
  insert.Add("x", "v1", 0, 0, 10);
  insert.Add("y", "v1", 10, 20, 24);
  insert.Add("z", "v1", 30, 30, 35);
  auto insert_snap = insert.Snapshot();
  plan = cutline::ui::PlanMove(insert.Context(insert_snap), {"z"}, "z", S(-28), 0, cutline::ui::OverlapMode::Insert);
  CHECK(plan.ok);
  CHECK(insert.Apply(plan));
  insert_snap = insert.Snapshot();
  CHECK(StartsAt(insert_snap, "z", S(2)) && EndsAt(insert_snap, "z", S(7)));
  CHECK(EndsAt(insert_snap, "x", S(2)));                       // x was cut at 2
  CHECK_EQ(Count(insert_snap, "v1"), std::size_t{4});          // x, z, the right half of x, y
  CHECK(StartsAt(insert_snap, "y", S(15)));                    // y pushed along by the 5 s that came in
  CHECK_NO_THROW(insert.store().ValidateDatabase());
  (void)insert.store().Undo("tester", "2026-10-06T00:00:01Z");
  insert_snap = insert.Snapshot();
  CHECK(StartsAt(insert_snap, "z", S(30)) && StartsAt(insert_snap, "y", S(10)) && Count(insert_snap, "v1") == 3);
}

CUTLINE_TEST(MovingLinkedClipsMovesTheirPartnersAndTracksMoveByRows) {
  Project project;
  project.Add("v", "v1", 4, 0, 6, "g");
  project.Add("a", "a1", 4, 0, 6, "g");
  project.Add("other", "v2", 20, 0, 3);
  auto snap = project.Snapshot();
  auto ctx = project.Context(snap);
  // Moving the picture takes the sound with it.
  auto plan = cutline::ui::PlanMove(ctx, {"v"}, "v", S(3), 0, cutline::ui::OverlapMode::Refuse);
  CHECK(plan.ok && plan.commands.size() == 2);
  CHECK(project.Apply(plan));
  snap = project.Snapshot();
  CHECK(StartsAt(snap, "v", S(7)) && StartsAt(snap, "a", S(7)));
  // With linking off in the context, only the one clip moves.
  ctx = project.Context(snap);
  ctx.linked = false;
  plan = cutline::ui::PlanMove(ctx, {"v"}, "v", S(1), 0, cutline::ui::OverlapMode::Refuse);
  CHECK(plan.ok && plan.commands.size() == 1);
  CHECK(project.Apply(plan));
  snap = project.Snapshot();
  CHECK(StartsAt(snap, "v", S(8)) && StartsAt(snap, "a", S(7)));
  // Up a row: v1 to v2 (the audio partner stays on its own track).
  ctx = project.Context(snap);
  plan = cutline::ui::PlanMove(ctx, {"v"}, "v", S(0), 1, cutline::ui::OverlapMode::Refuse);
  CHECK(plan.ok);
  CHECK(project.Apply(plan));
  snap = project.Snapshot();
  CHECK_EQ(TrackOf(snap, "v"), std::string("v2"));
  CHECK_EQ(TrackOf(snap, "a"), std::string("a1"));
  // There is no row above the top one, and a locked track refuses.
  CHECK(!cutline::ui::PlanMove(project.Context(snap), {"v"}, "v", S(0), 1, cutline::ui::OverlapMode::Refuse).ok);
  project.Run(cmd::CommandType::SetTrackState, cmd::SetTrackStatePayload{"v1", true, false, false, 0.0, 0.0, "V1"});
  snap = project.Snapshot();
  const auto locked = cutline::ui::PlanMove(project.Context(snap), {"v"}, "v", S(0), -1, cutline::ui::OverlapMode::Refuse);
  CHECK(!locked.ok && locked.refusal.find("locked") != std::string::npos);
}

CUTLINE_TEST(TrimmingMovesOneEdgeWithinTheMediaAndNeighboursAndARippleClosesUpEverywhere) {
  Project project;
  project.Add("a", "v1", 0, 10, 15);
  project.Add("b", "v1", 8, 20, 30);
  project.Add("c", "v1", 20, 40, 45);
  project.Add("under", "v2", 25, 0, 4);
  project.Add("sound", "a1", 20, 40, 45);
  auto snap = project.Snapshot();

  // Tail of a, shorter; then longer, stopped by the media's end? (a's media runs to 60) and by b at 8.
  auto plan = cutline::ui::PlanTrim(project.Context(snap), "a", cutline::ui::TrimEdge::Tail, S(3), cutline::ui::TrimMode::Normal);
  CHECK(plan.ok && project.Apply(plan));
  snap = project.Snapshot();
  CHECK(EndsAt(snap, "a", S(3)) && Source(snap, "a", S(10), S(13)));
  plan = cutline::ui::PlanTrim(project.Context(snap), "a", cutline::ui::TrimEdge::Tail, S(20), cutline::ui::TrimMode::Normal);
  CHECK(plan.ok && !plan.notes.empty());
  CHECK(project.Apply(plan));
  snap = project.Snapshot();
  CHECK(EndsAt(snap, "a", S(8)));                                       // stopped by b

  // Head of b: shorter by 2 s (source in moves with it), then back past where it was: stopped by a.
  plan = cutline::ui::PlanTrim(project.Context(snap), "b", cutline::ui::TrimEdge::Head, S(10), cutline::ui::TrimMode::Normal);
  CHECK(plan.ok && project.Apply(plan));
  snap = project.Snapshot();
  CHECK(StartsAt(snap, "b", S(10)) && Source(snap, "b", S(22), S(30)));
  plan = cutline::ui::PlanTrim(project.Context(snap), "b", cutline::ui::TrimEdge::Head, S(0), cutline::ui::TrimMode::Normal);
  CHECK(plan.ok && !plan.notes.empty());
  CHECK(project.Apply(plan));
  snap = project.Snapshot();
  CHECK(StartsAt(snap, "b", S(8)));
  // Never below a frame, and refused when nothing changes.
  plan = cutline::ui::PlanTrim(project.Context(snap), "b", cutline::ui::TrimEdge::Tail, S(8), cutline::ui::TrimMode::Normal);
  CHECK(plan.ok);
  CHECK(project.Apply(plan));
  snap = project.Snapshot();
  CHECK(EndsAt(snap, "b", S(8).Add(Fr(1))));
  CHECK(!cutline::ui::PlanTrim(project.Context(snap), "nothing", cutline::ui::TrimEdge::Tail, S(8), cutline::ui::TrimMode::Normal).ok);

  // Back to a known state for the ripple.
  Project ripple;
  ripple.Add("a", "v1", 0, 10, 20);
  ripple.Add("b", "v1", 10, 30, 40);
  ripple.Add("c", "v1", 20, 40, 45);
  ripple.Add("later", "v2", 22, 0, 4);
  ripple.Add("locked", "a2", 30, 0, 4);
  ripple.Run(cmd::CommandType::SetTrackState, cmd::SetTrackStatePayload{"a2", true, false, false, 0.0, 0.0, "A2"});
  auto rsnap = ripple.Snapshot();
  // Ripple the tail of a from 10 to 6: b, c and the clip on v2 move 4 s earlier; the locked track stays.
  plan = cutline::ui::PlanTrim(ripple.Context(rsnap), "a", cutline::ui::TrimEdge::Tail, S(6), cutline::ui::TrimMode::Ripple);
  CHECK(plan.ok);
  CHECK(ripple.Apply(plan));
  rsnap = ripple.Snapshot();
  CHECK(EndsAt(rsnap, "a", S(6)) && StartsAt(rsnap, "b", S(6)) && StartsAt(rsnap, "c", S(16)) && StartsAt(rsnap, "later", S(18)));
  CHECK(StartsAt(rsnap, "locked", S(30)));
  CHECK_NO_THROW(ripple.store().ValidateDatabase());
  // Growing: the tail out by 3, and everything after moves 3 later.
  plan = cutline::ui::PlanTrim(ripple.Context(rsnap), "a", cutline::ui::TrimEdge::Tail, S(9), cutline::ui::TrimMode::Ripple);
  CHECK(plan.ok && ripple.Apply(plan));
  rsnap = ripple.Snapshot();
  CHECK(EndsAt(rsnap, "a", S(9)) && StartsAt(rsnap, "b", S(9)) && StartsAt(rsnap, "c", S(19)) && StartsAt(rsnap, "later", S(21)));
  // Head ripple on b: its start stays, its source in moves, the end comes in and what follows closes up.
  plan = cutline::ui::PlanTrim(ripple.Context(rsnap), "b", cutline::ui::TrimEdge::Head, S(11), cutline::ui::TrimMode::Ripple);
  CHECK(plan.ok && ripple.Apply(plan));
  rsnap = ripple.Snapshot();
  CHECK(StartsAt(rsnap, "b", S(9)) && EndsAt(rsnap, "b", S(17)) && Source(rsnap, "b", S(32), S(40)));
  CHECK(StartsAt(rsnap, "c", S(17)) && StartsAt(rsnap, "later", S(19)));
  CHECK_NO_THROW(ripple.store().ValidateDatabase());
  (void)ripple.store().Undo("tester", "2026-10-06T00:00:01Z");
  rsnap = ripple.Snapshot();
  CHECK(StartsAt(rsnap, "b", S(9)) && EndsAt(rsnap, "b", S(19)) && StartsAt(rsnap, "c", S(19)));

  // A ripple that would close up past a clip on another track is stopped where that clip is.
  Project blocked;
  blocked.Add("a", "v1", 0, 0, 10);
  blocked.Add("b", "v1", 10, 0, 10);
  blocked.Add("wide", "v2", 5, 0, 12);     // spans the join at 10 and ends at 17
  blocked.Add("after", "v2", 20, 0, 3);
  auto bsnap = blocked.Snapshot();
  plan = cutline::ui::PlanTrim(blocked.Context(bsnap), "a", cutline::ui::TrimEdge::Tail, S(2), cutline::ui::TrimMode::Ripple);
  CHECK(plan.ok);
  CHECK(!plan.notes.empty());
  CHECK(blocked.Apply(plan));
  bsnap = blocked.Snapshot();
  CHECK(StartsAt(bsnap, "after", S(17)));    // 'after' came up against the end of 'wide'
  CHECK(EndsAt(bsnap, "a", S(7)));           // so a could only come in by 3 s, not the 8 asked for
  CHECK_NO_THROW(blocked.store().ValidateDatabase());
}

CUTLINE_TEST(LinkedClipsAreTrimmedTogetherAndRollSlipAndSlideDoWhatTheyAreFor) {
  Project project;
  project.Add("v", "v1", 0, 10, 20, "g");
  project.Add("a", "a1", 0, 10, 20, "g");
  project.Add("next", "v1", 10, 30, 40);
  auto snap = project.Snapshot();
  auto plan = cutline::ui::PlanTrim(project.Context(snap), "v", cutline::ui::TrimEdge::Tail, S(7), cutline::ui::TrimMode::Normal);
  CHECK(plan.ok && project.Apply(plan));
  snap = project.Snapshot();
  CHECK(EndsAt(snap, "v", S(7)) && EndsAt(snap, "a", S(7)));

  // Roll: the cut between two touching clips moves, one growing as the other shrinks.
  Project roll;
  roll.Add("p", "v1", 0, 10, 20);
  roll.Add("q", "v1", 10, 30, 40);
  auto rsnap = roll.Snapshot();
  plan = cutline::ui::PlanRoll(roll.Context(rsnap), "p", "q", S(13));
  CHECK(plan.ok && roll.Apply(plan));
  rsnap = roll.Snapshot();
  CHECK(EndsAt(rsnap, "p", S(13)) && Source(rsnap, "p", S(10), S(23)));
  CHECK(StartsAt(rsnap, "q", S(13)) && EndsAt(rsnap, "q", S(20)) && Source(rsnap, "q", S(33), S(40)));
  plan = cutline::ui::PlanRoll(roll.Context(rsnap), "p", "q", S(5));
  CHECK(plan.ok && roll.Apply(plan));
  rsnap = roll.Snapshot();
  CHECK(EndsAt(rsnap, "p", S(5)) && StartsAt(rsnap, "q", S(5)) && Source(rsnap, "q", S(25), S(40)));
  CHECK(!cutline::ui::PlanRoll(roll.Context(rsnap), "q", "p", S(7)).ok);       // not in order / not touching that way
  // Stopped where the media starts: q's media would go below zero if rolled far enough left.
  Project media_edge;
  media_edge.Add("p", "v1", 0, 0, 10);
  media_edge.Add("q", "v1", 10, 3, 9);
  auto msnap = media_edge.Snapshot();
  plan = cutline::ui::PlanRoll(media_edge.Context(msnap), "p", "q", S(2));
  CHECK(plan.ok && !plan.notes.empty());
  CHECK(media_edge.Apply(plan));
  msnap = media_edge.Snapshot();
  CHECK(StartsAt(msnap, "q", S(7)) && Source(msnap, "q", S(0), S(9)));

  // Slip: same place on the timeline, different part of the media; clamped at the media's start.
  Project slip;
  slip.Add("s", "v1", 5, 10, 15);
  auto ssnap = slip.Snapshot();
  plan = cutline::ui::PlanSlip(slip.Context(ssnap), "s", S(3));
  CHECK(plan.ok && slip.Apply(plan));
  ssnap = slip.Snapshot();
  CHECK(StartsAt(ssnap, "s", S(5)) && EndsAt(ssnap, "s", S(10)) && Source(ssnap, "s", S(13), S(18)));
  plan = cutline::ui::PlanSlip(slip.Context(ssnap), "s", S(-50));
  CHECK(plan.ok && !plan.notes.empty());
  CHECK(slip.Apply(plan));
  ssnap = slip.Snapshot();
  CHECK(Source(ssnap, "s", S(0), S(5)));
  CHECK(!cutline::ui::PlanSlip(slip.Context(ssnap), "s", S(-1)).ok);     // already at the start

  // Slide: the clip moves between neighbours that touch it.
  Project slide;
  slide.Add("l", "v1", 0, 0, 10);
  slide.Add("m", "v1", 10, 20, 25);
  slide.Add("r", "v1", 15, 30, 40);
  auto dsnap = slide.Snapshot();
  plan = cutline::ui::PlanSlide(slide.Context(dsnap), "m", S(2));
  CHECK(plan.ok && slide.Apply(plan));
  dsnap = slide.Snapshot();
  CHECK(StartsAt(dsnap, "m", S(12)) && EndsAt(dsnap, "m", S(17)) && Source(dsnap, "m", S(20), S(25)));
  CHECK(EndsAt(dsnap, "l", S(12)) && Source(dsnap, "l", S(0), S(12)));
  CHECK(StartsAt(dsnap, "r", S(17)) && Source(dsnap, "r", S(32), S(40)));
  plan = cutline::ui::PlanSlide(slide.Context(dsnap), "m", S(-4));
  CHECK(plan.ok && slide.Apply(plan));
  dsnap = slide.Snapshot();
  CHECK(StartsAt(dsnap, "m", S(8)) && EndsAt(dsnap, "l", S(8)) && StartsAt(dsnap, "r", S(13)));
  CHECK_NO_THROW(slide.store().ValidateDatabase());
  CHECK(!cutline::ui::PlanSlide(slide.Context(dsnap), "l", S(1)).ok);   // nothing before it
}

CUTLINE_TEST(CuttingDeletingLiftingAndExtractingKeepTheTimelineConsistent) {
  Project project;
  project.Add("v", "v1", 0, 0, 10, "g");
  project.Add("a", "a1", 0, 0, 10, "g");
  project.Add("w", "v2", 2, 0, 4);
  project.Add("n", "v1", 10, 20, 30);
  auto snap = project.Snapshot();

  // Add edit: cuts every clip across the time, linked ones together, once each.
  auto plan = cutline::ui::PlanSplit(project.Context(snap), S(3));
  CHECK(plan.ok && plan.commands.size() == 2);   // the linked pair is one cut, and w is the second
  CHECK(project.Apply(plan));
  snap = project.Snapshot();
  CHECK_EQ(Count(snap, "v1"), std::size_t{3});
  CHECK_EQ(Count(snap, "a1"), std::size_t{2});
  CHECK_EQ(Count(snap, "v2"), std::size_t{2});
  CHECK(!cutline::ui::PlanSplit(project.Context(snap), S(100)).ok);
  // Restricted to a track.
  plan = cutline::ui::PlanSplit(project.Context(snap), S(5), {"v1"});
  CHECK(plan.ok && project.Apply(plan));
  snap = project.Snapshot();
  CHECK_EQ(Count(snap, "v1"), std::size_t{4});

  // Delete, with and without closing the gap.
  plan = cutline::ui::PlanDelete(project.Context(snap), {"n"}, false);
  CHECK(plan.ok && project.Apply(plan));
  snap = project.Snapshot();
  CHECK_EQ(Count(snap, "v1"), std::size_t{3});
  Project ripple;
  ripple.Add("a", "v1", 0, 0, 5);
  ripple.Add("b", "v1", 5, 5, 10);
  ripple.Add("c", "v1", 10, 10, 15);
  auto rsnap = ripple.Snapshot();
  plan = cutline::ui::PlanDelete(ripple.Context(rsnap), {"a", "b"}, true);
  CHECK(plan.ok && ripple.Apply(plan));
  rsnap = ripple.Snapshot();
  CHECK_EQ(Count(rsnap, "v1"), std::size_t{1});
  CHECK(StartsAt(rsnap, "c", S(0)));
  CHECK(!cutline::ui::PlanDelete(ripple.Context(rsnap), {}, false).ok);
  CHECK(!cutline::ui::PlanDelete(ripple.Context(rsnap), {"ghost"}, false).ok);

  // Lift and extract a range: a clip across both edges is cut, a clip inside is removed, one across an edge is trimmed.
  Project range;
  range.Add("whole", "v1", 0, 0, 20);
  range.Add("inside", "v2", 6, 0, 2);
  range.Add("edge", "v2", 9, 0, 4);
  range.Add("after", "v1", 25, 0, 5);
  auto gsnap = range.Snapshot();
  plan = cutline::ui::PlanLift(range.Context(gsnap), S(5), S(10), {});
  CHECK(plan.ok && range.Apply(plan));
  gsnap = range.Snapshot();
  CHECK(EndsAt(gsnap, "whole", S(5)));
  CHECK_EQ(Count(gsnap, "v1"), std::size_t{3});
  CHECK(Find(gsnap, "inside") == nullptr);
  CHECK(StartsAt(gsnap, "edge", S(10)) && Source(gsnap, "edge", S(1), S(4)));
  CHECK(StartsAt(gsnap, "after", S(25)));
  CHECK_NO_THROW(range.store().ValidateDatabase());
  (void)range.store().Undo("tester", "2026-10-06T00:00:01Z");
  gsnap = range.Snapshot();
  CHECK(EndsAt(gsnap, "whole", S(20)) && Find(gsnap, "inside") != nullptr);

  plan = cutline::ui::PlanExtract(range.Context(gsnap), S(5), S(10), {});
  CHECK(plan.ok && range.Apply(plan));
  gsnap = range.Snapshot();
  CHECK(StartsAt(gsnap, "after", S(20)));
  CHECK_EQ(Count(gsnap, "v1"), std::size_t{3});
  CHECK(!cutline::ui::PlanLift(range.Context(gsnap), S(10), S(10), {}).ok);
  CHECK(!cutline::ui::PlanLift(range.Context(gsnap), S(100), S(110), {}).ok);
}

CUTLINE_TEST(InsertingAndOverwritingASourcePlacesLinkedPictureAndSoundAndPastingCarriesEffects) {
  Project project;
  project.Add("x", "v1", 0, 0, 10);
  project.Add("y", "a1", 0, 0, 10);
  project.Add("late", "v1", 20, 0, 5);
  auto snap = project.Snapshot();
  cutline::ui::SourceRange source{"m2", "Interview", S(5), S(9), true, true};

  // Overwrite at 4 s: what is under 4..8 is cut away on both tracks.
  auto plan = cutline::ui::PlanInsertEdit(project.Context(snap), source, S(4), "v1", "a1", cutline::ui::OverlapMode::Overwrite);
  CHECK(plan.ok && project.Apply(plan));
  snap = project.Snapshot();
  CHECK(EndsAt(snap, "x", S(4)) && EndsAt(snap, "y", S(4)));
  CHECK_EQ(Count(snap, "v1"), std::size_t{4});     // x, the new clip, what is left of x after it, and late
  CHECK_EQ(Count(snap, "a1"), std::size_t{3});
  // The new clips are linked to each other and not to anything else.
  std::string video_group, audio_group;
  for (const auto& clip : snap.FindTrack("v1")->clips) {
    if (clip.name == "Interview") video_group = clip.linked_group;
  }
  for (const auto& clip : snap.FindTrack("a1")->clips) {
    if (clip.name == "Interview") audio_group = clip.linked_group;
  }
  CHECK(!video_group.empty() && video_group == audio_group);

  // Insert at 2 s on an empty region of a different pair of tracks: everything on every track from there moves 4 s later.
  plan = cutline::ui::PlanInsertEdit(project.Context(snap), source, S(2), "v1", "a1", cutline::ui::OverlapMode::Insert);
  CHECK(plan.ok && project.Apply(plan));
  snap = project.Snapshot();
  CHECK(StartsAt(snap, "late", S(24)));
  CHECK(EndsAt(snap, "x", S(2)));
  CHECK_NO_THROW(project.store().ValidateDatabase());
  (void)project.store().Undo("tester", "2026-10-06T00:00:01Z");
  snap = project.Snapshot();
  CHECK(StartsAt(snap, "late", S(20)));

  // Picture only, or sound only.
  source.has_audio = false;
  plan = cutline::ui::PlanInsertEdit(project.Context(snap), source, S(40), "v1", "a1", cutline::ui::OverlapMode::Overwrite);
  CHECK(plan.ok && plan.commands.size() == 1);
  CHECK(!cutline::ui::PlanInsertEdit(project.Context(snap), source, S(40), "", "a1", cutline::ui::OverlapMode::Overwrite).ok);
  // Refused: past the media, locked track, strict overlap.
  source.in = S(58);
  source.out = S(70);
  CHECK(!cutline::ui::PlanInsertEdit(project.Context(snap), source, S(40), "v1", "", cutline::ui::OverlapMode::Overwrite).ok);
  source.in = S(0);
  source.out = S(5);
  CHECK(!cutline::ui::PlanInsertEdit(project.Context(snap), source, S(1), "v1", "", cutline::ui::OverlapMode::Refuse).ok);
  CHECK(!cutline::ui::PlanInsertEdit(project.Context(snap), source, S(1), "nonexistent", "", cutline::ui::OverlapMode::Overwrite).ok);

  // Copy and paste: a clip with an animated effect lands elsewhere with the effect and its keyframes.
  Project paste;
  paste.Add("src", "v1", 0, 0, 5);
  cmd::AddEffectPayload effect;
  effect.id = "fx1";
  effect.owner_kind = cutline::model::EffectOwner::Clip;
  effect.owner_id = "src";
  effect.effect_type = "opacity";
  effect.parameters = {{"fx1:value", "value", cutline::anim::Value::Scalar(0.5)}};
  paste.Run(cmd::CommandType::AddEffect, effect);
  cmd::SetKeyframePayload key;
  key.parameter_id = "fx1:value";
  key.keyframe = {S(1), cutline::anim::Value::Scalar(1.0), cutline::anim::Interpolation::Linear, {}, {}};
  paste.Run(cmd::CommandType::SetKeyframe, key);
  auto psnap = paste.Snapshot();
  const auto spec = cutline::ui::SpecOf(psnap, "src", S(0));
  CHECK(spec.has_value());
  auto placed = *spec;
  placed.track_id = "v2";
  plan = cutline::ui::PlanPlace(paste.Context(psnap), {placed}, S(30), cutline::ui::OverlapMode::Overwrite, "Paste");
  CHECK(plan.ok && paste.Apply(plan));
  psnap = paste.Snapshot();
  CHECK_EQ(Count(psnap, "v2"), std::size_t{1});
  const auto& copy = psnap.FindTrack("v2")->clips.front();
  CHECK(copy.timeline_start.Compare(S(30)) == 0);
  CHECK_EQ(copy.effects.size(), std::size_t{1});
  CHECK_EQ(copy.effects[0].parameters[0].value.keyframes().size(), std::size_t{1});
  CHECK(std::abs(copy.effects[0].parameters[0].value.keyframes()[0].value.scalar() - 1.0) < 1e-9);
  CHECK(copy.effects[0].parameters[0].value.keyframes()[0].time.Compare(S(1)) == 0);
}

CUTLINE_TEST(ThreePointEditsResolveTheMissingPointAndRefuseWhatCannotBe) {
  using cutline::ui::ResolveThreePoint;
  using cutline::ui::ThreePointInput;
  ThreePointInput in;
  in.playhead = S(10);
  in.source_duration = S(60);
  // Source in and out, at the playhead.
  in.source_in = S(5);
  in.source_out = S(9);
  auto r = ResolveThreePoint(in);
  CHECK(r.ok && r.at.Compare(S(10)) == 0 && r.source_in.Compare(S(5)) == 0 && r.source_out.Compare(S(9)) == 0);
  // Source in, sequence in and out: the source out follows.
  in.source_out.reset();
  in.sequence_in = S(20);
  in.sequence_out = S(26);
  r = ResolveThreePoint(in);
  CHECK(r.ok && r.at.Compare(S(20)) == 0 && r.source_out.Compare(S(11)) == 0);
  // Source out and sequence out only: both follow.
  in.source_in.reset();
  in.source_out = S(8);
  in.sequence_in.reset();
  r = ResolveThreePoint(in);
  CHECK(r.ok && r.source_in.Compare(S(0)) == 0 && r.at.Compare(S(18)) == 0);
  // Source range and sequence out, no sequence in: placed so that it ends at the out.
  in.source_in = S(10);
  in.source_out = S(14);
  r = ResolveThreePoint(in);
  CHECK(r.ok && r.at.Compare(S(22)) == 0);
  // Four points: the source is trimmed to the space.
  in.sequence_in = S(20);
  in.sequence_out = S(23);
  r = ResolveThreePoint(in);
  CHECK(r.ok && r.source_out.Compare(S(13)) == 0 && r.at.Compare(S(20)) == 0);
  // One source point and nothing else: to the end of the media; nothing at all: the whole source.
  ThreePointInput lone;
  lone.playhead = S(3);
  lone.source_duration = S(60);
  lone.source_in = S(50);
  r = ResolveThreePoint(lone);
  CHECK(r.ok && r.source_out.Compare(S(60)) == 0 && r.at.Compare(S(3)) == 0);
  lone.source_in.reset();
  r = ResolveThreePoint(lone);
  CHECK(r.ok && r.source_in.Compare(S(0)) == 0 && r.source_out.Compare(S(60)) == 0);
  // Refused: reversed marks, outside the media, before the start.
  ThreePointInput bad = lone;
  bad.source_in = S(9);
  bad.source_out = S(4);
  CHECK(!ResolveThreePoint(bad).ok);
  bad.source_in = S(50);
  bad.source_out = S(80);
  CHECK(!ResolveThreePoint(bad).ok);
  bad = lone;
  bad.source_in = S(0);
  bad.source_out = S(30);
  bad.sequence_out = S(10);
  CHECK(!ResolveThreePoint(bad).ok);   // 30 s of source ending at 10 s would start before the sequence
  bad = lone;
  bad.sequence_in = S(8);
  bad.sequence_out = S(4);
  CHECK(!ResolveThreePoint(bad).ok);
}

CUTLINE_TEST(EnablingLinkingAndUnlinkingClipsArePlansToo) {
  Project project;
  project.Add("a", "v1", 0, 0, 5);
  project.Add("b", "a1", 0, 0, 5);
  project.Add("c", "v1", 5, 0, 5);
  auto snap = project.Snapshot();
  auto plan = cutline::ui::PlanLink(project.Context(snap), {"a", "b"});
  CHECK(plan.ok && project.Apply(plan));
  snap = project.Snapshot();
  CHECK(!Find(snap, "a")->linked_group.empty() && Find(snap, "a")->linked_group == Find(snap, "b")->linked_group);
  CHECK(!cutline::ui::PlanLink(project.Context(snap), {"a"}).ok);
  plan = cutline::ui::PlanSetEnabled(project.Context(snap), {"a"}, false);
  CHECK(plan.ok && plan.commands.size() == 2);      // the linked partner too
  CHECK(project.Apply(plan));
  snap = project.Snapshot();
  CHECK(!Find(snap, "a")->enabled && !Find(snap, "b")->enabled && Find(snap, "c")->enabled);
  CHECK(!cutline::ui::PlanSetEnabled(project.Context(snap), {"a"}, false).ok);   // already off
  plan = cutline::ui::PlanUnlink(project.Context(snap), {"a"});
  CHECK(plan.ok && project.Apply(plan));
  snap = project.Snapshot();
  CHECK(Find(snap, "a")->linked_group.empty());
  CHECK(!cutline::ui::PlanUnlink(project.Context(snap), {"a"}).ok);
}

// ------------------------------------------------------------ timeline view ----

CUTLINE_TEST(TheViewportMapsTimeToPixelsZoomsAboutAPointAndFollowsThePlayhead) {
  cutline::ui::TimelineViewport view;
  view.pixels_per_second = 100;
  view.width_px = 1000;
  CHECK(std::abs(view.SecondsToX(3.0) - 300.0) < 1e-9);
  CHECK(std::abs(view.XToSeconds(300.0) - 3.0) < 1e-9);
  // To the nearest frame, never before zero.
  CHECK(view.XToTime(301.0, cutline::time::kFrameRate25).Compare(RationalTime::FromFrames(75, cutline::time::kFrameRate25)) == 0);
  CHECK(view.XToTime(-50.0, cutline::time::kFrameRate25).Compare(S(0)) == 0);
  // Zoom keeps the time under the pointer where it is.
  view.scroll_seconds = 2.0;
  const auto before = view.XToSeconds(400.0);
  view.ZoomAt(400.0, 2.0);
  CHECK(std::abs(view.XToSeconds(400.0) - before) < 1e-9);
  CHECK(std::abs(view.pixels_per_second - 200.0) < 1e-9);
  view.ZoomAt(0.0, 1e9);
  CHECK(view.pixels_per_second <= cutline::ui::TimelineViewport::kMaxPixelsPerSecond);
  view.ZoomAt(0.0, 1e-12);
  CHECK(view.pixels_per_second >= cutline::ui::TimelineViewport::kMinPixelsPerSecond);
  // Fit.
  view.FitRange(10.0, 70.0);
  CHECK(view.SecondsToX(10.0) > 0 && view.SecondsToX(70.0) < view.width_px);
  CHECK(view.SecondsToX(10.0) < 40 && view.SecondsToX(70.0) > view.width_px - 40);
  // Scrolling is bounded at zero; EnsureVisible and follow-playhead page as needed.
  view.scroll_seconds = 5.0;
  view.ScrollBySeconds(-100.0);
  CHECK(view.scroll_seconds == 0.0);
  view.pixels_per_second = 100;
  view.scroll_seconds = 0;
  CHECK(!view.EnsureVisible(5.0));
  CHECK(view.EnsureVisible(30.0));
  CHECK(view.XToSeconds(0) <= 30.0 && view.EndSeconds() >= 30.0);
  view.scroll_seconds = 0;
  CHECK(!view.FollowPlayhead(3.0));
  CHECK(view.FollowPlayhead(12.0) && view.scroll_seconds > 11.0);
  CHECK(!view.FollowPlayhead(view.scroll_seconds + 1.0));
}

CUTLINE_TEST(TheRulerChoosesReadableStepsAndLabelsThemAsTimecode) {
  cutline::ui::TimelineViewport view;
  view.width_px = 1000;
  const auto rate = cutline::time::kFrameRate25;
  for (const double zoom : {0.05, 0.5, 5.0, 50.0, 500.0, 5000.0}) {
    view.pixels_per_second = zoom;
    view.scroll_seconds = 0;
    const auto ticks = cutline::ui::BuildRuler(view, rate, false);
    CHECK(!ticks.empty());
    double last_major = -1e9;
    int majors = 0;
    for (const auto& tick : ticks) {
      if (!tick.major) continue;
      ++majors;
      CHECK(!tick.label.empty());
      if (last_major > -1e8) Expect(tick.x - last_major >= 89.0, "major ticks " + std::to_string(tick.x - last_major) + " px apart at zoom " + std::to_string(zoom), __LINE__);
      last_major = tick.x;
    }
    CHECK(majors >= 1);
    // Minor ticks, where there are any, are not crowded.
    double last = -1e9;
    for (const auto& tick : ticks) {
      if (last > -1e8) CHECK(tick.x - last >= 9.0);
      last = tick.x;
    }
  }
  view.pixels_per_second = 100;
  view.scroll_seconds = 0;
  const auto ticks = cutline::ui::BuildRuler(view, rate, false);
  CHECK_EQ(ticks.front().label, std::string("00:00:00:00"));
  bool found_ten = false;
  for (const auto& tick : ticks) {
    if (tick.major && std::abs(tick.seconds - 10.0) < 1e-9) found_ten = true;   // at 100 px/s the major step is 1 s, so 10 s is a major tick
  }
  CHECK(found_ten);
  // At frame zoom the labels step by frames.
  view.pixels_per_second = 2000;
  const auto fine = cutline::ui::BuildRuler(view, rate, false);
  CHECK(fine.size() > 5 && fine[1].seconds > 0 && fine[1].seconds < 0.1);
  // Scrolled: ticks begin before the left edge's time and cover the right edge.
  view.pixels_per_second = 100;
  view.scroll_seconds = 62.5;
  const auto scrolled = cutline::ui::BuildRuler(view, rate, false);
  CHECK(scrolled.front().seconds <= 62.5 + 1.0 && scrolled.back().seconds >= view.EndSeconds() - 1.0);
}

CUTLINE_TEST(TheTimelineLayoutStacksTracksLikeAnEditorAndHitTestingFindsEdgesAndEditPoints) {
  Project project;
  project.Add("a", "v1", 0, 0, 5);
  project.Add("b", "v1", 5, 0, 5);
  project.Add("c", "v2", 2, 0, 3, "g");
  project.Add("d", "a1", 2, 0, 3, "g");
  auto snap = project.Snapshot();
  cutline::ui::TimelineViewport view;
  view.pixels_per_second = 100;
  view.width_px = 1000;
  cutline::ui::LayoutOptions options;
  const auto layout = cutline::ui::BuildTimelineLayout(snap, view, options);
  // v2 is above v1, then audio a1 then a2.
  CHECK_EQ(layout.tracks.size(), std::size_t{4});
  CHECK_EQ(layout.tracks[0].id, std::string("v2"));
  CHECK_EQ(layout.tracks[1].id, std::string("v1"));
  CHECK_EQ(layout.tracks[2].id, std::string("a1"));
  CHECK_EQ(layout.tracks[3].id, std::string("a2"));
  CHECK(layout.tracks[0].y < layout.tracks[1].y && layout.tracks[1].y < layout.tracks[2].y);
  CHECK(std::abs(layout.tracks[1].height - options.video_track_height) < 1e-9 && std::abs(layout.tracks[2].height - options.audio_track_height) < 1e-9);
  CHECK_EQ(layout.clips.size(), std::size_t{4});
  CHECK(std::abs(layout.sequence_seconds - 10.0) < 1e-9);
  const cutline::ui::ClipBox* box_c = nullptr;
  for (const auto& box : layout.clips) {
    if (box.clip_id == "c") box_c = &box;
  }
  CHECK(box_c != nullptr);
  if (box_c == nullptr) return;
  CHECK(std::abs(box_c->x - 200.0) < 1e-9 && std::abs(box_c->width - 300.0) < 1e-9 && box_c->linked);
  CHECK(box_c->y >= layout.tracks[0].y && box_c->y + box_c->height <= layout.tracks[0].y + layout.tracks[0].height);
  // Clips outside the visible span are left out.
  view.scroll_seconds = 6.0;
  CHECK_EQ(cutline::ui::BuildTimelineLayout(snap, view, options).clips.size(), std::size_t{1});
  view.scroll_seconds = 0;

  const auto hit = [&](double x, double y) { return cutline::ui::HitTest(layout, view, options, x, y); };
  const auto v1_y = layout.tracks[1].y + layout.tracks[1].height / 2.0;
  CHECK(hit(100.0, -5.0).kind == cutline::ui::HitKind::Ruler);
  CHECK(hit(100.0, v1_y).kind == cutline::ui::HitKind::ClipBody && hit(100.0, v1_y).clip_id == "a");
  CHECK(hit(2.0, v1_y).kind == cutline::ui::HitKind::ClipHead && hit(2.0, v1_y).clip_id == "a");
  CHECK(hit(700.0, v1_y).kind == cutline::ui::HitKind::ClipBody && hit(700.0, v1_y).clip_id == "b");
  const auto at_join = hit(502.0, v1_y);                     // within the zone around the cut at 5 s
  CHECK(at_join.kind == cutline::ui::HitKind::EditPoint && at_join.clip_id == "b" && at_join.other_clip_id == "a");
  CHECK(hit(498.0, v1_y).kind == cutline::ui::HitKind::EditPoint);
  CHECK(hit(1200.0, v1_y).kind == cutline::ui::HitKind::TrackEmpty && hit(1200.0, v1_y).track_id == "v1");
  CHECK(hit(996.0, v1_y).kind == cutline::ui::HitKind::ClipTail && hit(996.0, v1_y).clip_id == "b");
  CHECK(hit(300.0, layout.tracks[3].y + 5).kind == cutline::ui::HitKind::TrackEmpty);
  CHECK(hit(300.0, layout.content_height + 100).kind == cutline::ui::HitKind::None);
  CHECK(hit(491.0, v1_y).kind == cutline::ui::HitKind::ClipBody);        // 9 px from the cut: past the zone around it
  CHECK(hit(494.5, v1_y).kind == cutline::ui::HitKind::EditPoint);
  // A very short clip still has a body to grab: the edge zones shrink with it.
  Project tiny;
  tiny.Add("t", "v1", 1, 0, 1);
  auto tsnap = tiny.Snapshot();
  cutline::ui::TimelineViewport zoomed_out;
  zoomed_out.pixels_per_second = 6.0;
  zoomed_out.width_px = 500;
  const auto tlayout = cutline::ui::BuildTimelineLayout(tsnap, zoomed_out, options);
  const auto ty = tlayout.tracks[1].y + 5.0;
  const auto middle = cutline::ui::HitTest(tlayout, zoomed_out, options, 6.0 + 3.0, ty);
  CHECK(middle.kind == cutline::ui::HitKind::ClipBody);
}

CUTLINE_TEST(TimelineLayoutVirtualizesHundredsOfTracksAndLongClipLists) {
  cutline::timeline::Sequence sequence;
  sequence.id = "large";
  sequence.frame_rate = cutline::time::kFrameRate25;
  sequence.width = 1920;
  sequence.height = 1080;
  for (int track_index = 0; track_index < 120; ++track_index) {
    cutline::timeline::Track track;
    track.id = "v" + std::to_string(track_index);
    track.kind = cutline::model::TrackKind::Video;
    track.order = track_index;
    for (int clip_index = 0; clip_index < 1000; ++clip_index) {
      cutline::timeline::Clip clip;
      clip.id = track.id + "-" + std::to_string(clip_index);
      clip.source_kind = cutline::model::SourceKind::Media;
      clip.source_id = "media";
      clip.source_in = S(0);
      clip.source_out = S(1);
      clip.timeline_start = S(clip_index);
      clip.start_ticks = clip.timeline_start.ToTicks();
      clip.end_ticks = clip.end().ToTicks();
      track.clips.push_back(std::move(clip));
    }
    sequence.tracks.push_back(std::move(track));
  }
  cutline::ui::TimelineViewport viewport;
  viewport.scroll_seconds = 500.0;
  viewport.pixels_per_second = 100.0;
  viewport.width_px = 500.0;
  viewport.vertical_scroll_px = 58.0 * 50.0;
  viewport.height_px = 120.0;
  const auto layout = cutline::ui::BuildTimelineLayout(sequence, viewport);
  CHECK(layout.tracks.size() <= std::size_t{4});
  CHECK(layout.clips.size() <= std::size_t{28});
  CHECK(layout.content_height > 6000.0);
}

CUTLINE_TEST(SelectionLinksClipsRectanglesAndTracksAndSnappingPrefersWhatMattersMost) {
  Project project;
  project.Add("v", "v1", 0, 0, 5, "g");
  project.Add("a", "a1", 0, 0, 5, "g");
  project.Add("n", "v1", 5, 0, 5);
  project.Add("m", "v1", 12, 0, 3);
  auto snap = project.Snapshot();
  cutline::ui::Selection selection;
  selection.Select(snap, "v", cutline::ui::Selection::Mode::Replace, true);
  CHECK(selection.Contains("v") && selection.Contains("a") && !selection.Contains("n"));
  selection.Select(snap, "n", cutline::ui::Selection::Mode::Add, true);
  CHECK_EQ(selection.clips().size(), std::size_t{3});
  selection.Select(snap, "v", cutline::ui::Selection::Mode::Toggle, true);   // toggles the pair off
  CHECK(!selection.Contains("v") && !selection.Contains("a") && selection.Contains("n"));
  selection.Select(snap, "v", cutline::ui::Selection::Mode::Replace, false);
  CHECK_EQ(selection.clips().size(), std::size_t{1});
  selection.SelectAll(snap);
  CHECK_EQ(selection.clips().size(), std::size_t{4});
  selection.SelectTrackForward(snap, "v1", S(6), cutline::ui::Selection::Mode::Replace);
  CHECK(selection.Contains("n") && selection.Contains("m") && !selection.Contains("v"));
  cutline::ui::TimelineViewport view;
  view.pixels_per_second = 100;
  view.width_px = 2000;
  const auto layout = cutline::ui::BuildTimelineLayout(snap, view, {});
  selection.SelectRect(layout, 480.0, 0.0, 700.0, 500.0, cutline::ui::Selection::Mode::Replace, &snap);
  CHECK(selection.Contains("n") && selection.Contains("v") && selection.Contains("a") && !selection.Contains("m"));   // the rectangle touches n and v's end, and v brings a
  (void)project.Apply(cutline::ui::PlanDelete(project.Context(snap), {"n"}, false));
  snap = project.Snapshot();
  selection.Prune(snap);
  CHECK(!selection.Contains("n"));

  // Snapping.
  cutline::ui::SnapSources sources;
  sources.playhead = S(8);
  sources.mark_in = S(9);
  sources.markers = {RationalTime(21, 2)};
  const auto points = cutline::ui::CollectSnapPoints(snap, sources, {"m"});
  for (std::size_t i = 1; i < points.size(); ++i) CHECK(points[i - 1].time.ToTicks() <= points[i].time.ToTicks());
  for (const auto& point : points) CHECK(point.source != "m");
  view.pixels_per_second = 100;
  auto snapped = cutline::ui::Snap(RationalTime(404, 100), points, view, 8.0);       // 4.04 s: within 8 px of the end of v (5 s)? no (96 px away)
  CHECK(!snapped.snapped && snapped.time.Compare(RationalTime(404, 100)) == 0);
  snapped = cutline::ui::Snap(RationalTime(496, 100), points, view, 8.0);
  CHECK(snapped.snapped && snapped.time.Compare(S(5)) == 0);
  snapped = cutline::ui::Snap(RationalTime(803, 100), points, view, 8.0);            // near the playhead at 8 s
  CHECK(snapped.snapped && snapped.point.kind == cutline::ui::SnapKind::Playhead);
  snapped = cutline::ui::Snap(RationalTime(803, 100), points, view, 0.0);
  CHECK(!snapped.snapped);
  // Zoomed far out the same distance in pixels is a long way in time, so more things are close.
  view.pixels_per_second = 5;
  snapped = cutline::ui::Snap(RationalTime(404, 100), points, view, 8.0);
  CHECK(snapped.snapped);
  // A moving span snaps by whichever end is nearer a point.
  view.pixels_per_second = 100;
  auto span = cutline::ui::SnapSpan(RationalTime(501, 100), S(3), points, view, 8.0);      // start 5.01 s: near n's start at 5
  CHECK(span.snapped && !span.end_snapped && span.start.Compare(S(5)) == 0);
  span = cutline::ui::SnapSpan(RationalTime(497, 100), S(3), points, view, 8.0);                // start 4.97, end 7.97: the start is 3 px from 5 s, the end 3 px from 8 s; the playhead (priority) wins ties
  CHECK(span.snapped);
  span = cutline::ui::SnapSpan(S(6).Add(RationalTime(1, 2)), S(1), points, view, 8.0);          // 6.5..7.5: nothing near
  CHECK(!span.snapped);
}

// ---------------------------------------------------------------------- monitor ----

CUTLINE_TEST(TheMonitorPicksARenderSizeFitsThePictureAndDrawsSafeMargins) {
  using namespace cutline::ui;
  const SizePx hd{1920, 1080};
  CHECK(RenderSizeFor(hd, MonitorQuality::Full, {100, 100}) == hd);
  CHECK(RenderSizeFor(hd, MonitorQuality::Half, {100, 100}) == (SizePx{960, 540}));
  CHECK(RenderSizeFor(hd, MonitorQuality::Quarter, {100, 100}) == (SizePx{480, 270}));
  CHECK(RenderSizeFor(hd, MonitorQuality::Eighth, {100, 100}) == (SizePx{240, 136}));   // even sizes
  // Auto: the largest reduction that still has as many pixels as are shown.
  CHECK(RenderSizeFor(hd, MonitorQuality::Auto, {1920, 1080}) == hd);
  CHECK(RenderSizeFor(hd, MonitorQuality::Auto, {900, 500}) == (SizePx{960, 540}));
  CHECK(RenderSizeFor(hd, MonitorQuality::Auto, {400, 220}) == (SizePx{480, 270}));
  CHECK(RenderSizeFor(hd, MonitorQuality::Auto, {200, 100}) == (SizePx{240, 136}));
  CHECK(RenderSizeFor(hd, MonitorQuality::Auto, {5000, 3000}) == hd);          // never above the sequence
  CHECK(RenderSizeFor(hd, MonitorQuality::Auto, {0, 0}) == hd);
  CHECK(RenderSizeFor({64, 36}, MonitorQuality::Eighth, {1, 1}).width >= 16);
  CHECK(ParseMonitorQuality("quarter").value() == MonitorQuality::Quarter && !ParseMonitorQuality("huge").has_value());
  for (const auto q : {MonitorQuality::Full, MonitorQuality::Half, MonitorQuality::Quarter, MonitorQuality::Eighth, MonitorQuality::Auto}) {
    CHECK(ParseMonitorQuality(ToString(q)).value() == q);
  }

  // Fit centres the picture and keeps its shape; pixel aspect widens it.
  MonitorView view;
  auto rect = FrameRect(hd, 1.0, {1000, 1000}, view);
  CHECK(std::abs(rect.width - 1000.0) < 1e-9 && std::abs(rect.height - 562.5) < 1e-9 && std::abs(rect.y - 218.75) < 1e-9);
  rect = FrameRect({720, 480}, 40.0 / 33.0, {1000, 1000}, view);
  CHECK(std::abs(rect.width / rect.height - (720.0 * 40.0 / 33.0) / 480.0) < 1e-9);
  view.zoom = ZoomMode::Actual;
  rect = FrameRect(hd, 1.0, {1000, 1000}, view);
  CHECK(std::abs(rect.width - 1920.0) < 1e-9 && std::abs(rect.x - (1000.0 - 1920.0) / 2.0) < 1e-9);
  // Panned, but never so far that the picture is gone.
  view.pan_x = 100000.0;
  rect = FrameRect(hd, 1.0, {1000, 1000}, view);
  CHECK(rect.x < 1000.0 && rect.x + rect.width > 0.0);
  view.pan_x = -100000.0;
  rect = FrameRect(hd, 1.0, {1000, 1000}, view);
  CHECK(rect.x + rect.width > 0.0);
  view.zoom = ZoomMode::Fit;
  view.pan_x = 100000.0;
  rect = FrameRect(hd, 1.0, {1000, 1000}, view);
  CHECK(std::abs(rect.x) < 1e-9);                               // a fitted picture does not pan
  // A window point back to a picture pixel, and nothing outside it.
  view.pan_x = 0;
  const auto centre = PictureAt(hd, 1.0, {1000, 1000}, view, 500.0, 500.0);
  CHECK(centre.has_value() && std::abs(centre->first - 960.0) < 1e-6 && std::abs(centre->second - 540.0) < 1e-6);
  CHECK(!PictureAt(hd, 1.0, {1000, 1000}, view, 500.0, 50.0).has_value());

  // Overlays.
  OverlayOptions overlays;
  overlays.safe_margins = true;
  overlays.thirds = true;
  overlays.center_cross = true;
  overlays.guides = {1.0, 9.0 / 16.0, 0.0};
  const Rect2 picture{100, 50, 800, 450};
  const auto shapes = BuildOverlays(picture, overlays);
  int action = 0, title = 0, thirds = 0, centre_lines = 0, guides = 0;
  for (const auto& shape : shapes) {
    if (shape.tag == "action_safe") {
      ++action;
      CHECK(std::abs(shape.rect.width - 800.0 * 0.93) < 1e-6 && std::abs(shape.rect.x + shape.rect.width / 2.0 - 500.0) < 1e-6);
    }
    if (shape.tag == "title_safe") {
      ++title;
      CHECK(std::abs(shape.rect.height - 450.0 * 0.90) < 1e-6);
    }
    if (shape.tag == "third") ++thirds;
    if (shape.tag == "center") ++centre_lines;
    if (shape.tag == "guide") {
      ++guides;
      CHECK(shape.rect.x >= picture.x - 1e-9 && shape.rect.x + shape.rect.width <= picture.x + picture.width + 1e-9);
      CHECK(shape.rect.y >= picture.y - 1e-9 && shape.rect.y + shape.rect.height <= picture.y + picture.height + 1e-9);
    }
  }
  CHECK(action == 1 && title == 1 && thirds == 4 && centre_lines == 2 && guides == 2);   // the zero guide is ignored
  CHECK(BuildOverlays(picture, OverlayOptions{}).empty());
  CHECK(BuildOverlays({0, 0, 0, 0}, overlays).empty());
}

CUTLINE_TEST(ThePresenterRendersTheLatestRequestAndDropsWhatWasOvertaken) {
  using namespace cutline::ui;
  std::mutex mutex;
  std::vector<RationalTime> rendered;
  std::vector<std::uint64_t> delivered_serials;
  std::atomic<bool> hold{true};
  FramePresenter presenter(
      [&](const RationalTime& at, SizePx size) {
        {
          const std::lock_guard<std::mutex> lock(mutex);
          rendered.push_back(at);
        }
        // The first render is slow, so the requests made meanwhile pile up behind it.
        if (at.Compare(RationalTime(0, 1)) == 0) {
          while (hold.load()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return cutline::media::VideoFrame::Allocate(cutline::media::PixelFormat::Rgba8, size.width, size.height);
      },
      [&](cutline::media::VideoFrame frame, const RationalTime&, std::uint64_t serial) {
        const std::lock_guard<std::mutex> lock(mutex);
        if (frame.valid()) delivered_serials.push_back(serial);
      });
  (void)presenter.Request(RationalTime(0, 1), {32, 18});
  std::this_thread::sleep_for(std::chrono::milliseconds(50));   // let the first begin
  std::uint64_t last = 0;
  for (int i = 1; i <= 20; ++i) last = presenter.Request(RationalTime(i, 25), {32, 18});
  hold = false;
  CHECK(presenter.WaitIdle());
  const auto stats = presenter.statistics();
  CHECK_EQ(stats.requested, std::uint64_t{21});
  // Of the twenty made while the first was rendering, nineteen were replaced before they started.
  CHECK_EQ(stats.coalesced, std::uint64_t{19});
  CHECK_EQ(stats.rendered, std::uint64_t{2});          // the first, and only the last of the rest
  CHECK_EQ(stats.stale, std::uint64_t{1});             // the first was overtaken, so not shown
  CHECK_EQ(stats.delivered, std::uint64_t{1});
  CHECK_EQ(stats.failed, std::uint64_t{0});
  {
    const std::lock_guard<std::mutex> lock(mutex);
    CHECK_EQ(rendered.size(), std::size_t{2});
    CHECK(rendered.back().Compare(RationalTime(20, 25)) == 0);
    CHECK_EQ(delivered_serials.size(), std::size_t{1});
    CHECK_EQ(delivered_serials[0], last);
  }
  // Invalidate renders the last request again (the sequence changed).
  const auto again = presenter.Invalidate();
  CHECK(again > last);
  CHECK(presenter.WaitIdle());
  CHECK_EQ(presenter.statistics().delivered, std::uint64_t{2});

  // A renderer that throws does not stop the presenter, and the error is kept.
  std::atomic<int> calls{0};
  FramePresenter flaky(
      [&](const RationalTime&, SizePx) -> cutline::media::VideoFrame {
        if (++calls == 1) throw std::runtime_error("decoder fell over");
        return cutline::media::VideoFrame::Allocate(cutline::media::PixelFormat::Rgba8, 16, 16);
      },
      [](cutline::media::VideoFrame, const RationalTime&, std::uint64_t) {});
  (void)flaky.Request(RationalTime(0, 1), {16, 16});
  CHECK(flaky.WaitIdle());
  CHECK_EQ(flaky.statistics().failed, std::uint64_t{1});
  CHECK(flaky.statistics().last_error.find("decoder fell over") != std::string::npos);
  (void)flaky.Request(RationalTime(1, 25), {16, 16});
  CHECK(flaky.WaitIdle());
  CHECK_EQ(flaky.statistics().delivered, std::uint64_t{1});
  // Destroying while busy joins cleanly (as the presenter above does when the test ends).
}

// -------------------------------------------------------------------- transport ----

CUTLINE_TEST(TheTransportPlaysShuttlesStepsAndStopsOrLoopsAtTheEnds) {
  using cutline::ui::Transport;
  Transport transport(cutline::time::kFrameRate25);
  transport.SetDuration(S(10));
  CHECK(!transport.playing());
  // Space plays at normal speed, which the audio drives; space again stops.
  transport.TogglePlay();
  CHECK(transport.playing() && transport.audio_driven() && transport.rate() == 1.0);
  auto tick = transport.Advance(0.4);
  CHECK(tick.moved && tick.position.Compare(RationalTime(2, 5)) == 0);
  transport.TogglePlay();
  CHECK(!transport.playing());
  // L doubles forward up to 8x; J from forward goes to 1x backward; K stops.
  transport.PressForward();
  CHECK(transport.rate() == 1.0);
  transport.PressForward();
  CHECK(transport.rate() == 2.0 && !transport.audio_driven());
  transport.PressForward();
  transport.PressForward();
  transport.PressForward();
  CHECK(transport.rate() == 8.0);
  transport.PressReverse();
  CHECK(transport.rate() == -1.0);
  transport.PressReverse();
  CHECK(transport.rate() == -2.0);
  transport.PressStop();
  CHECK(!transport.playing());
  // K held with J or L is slow motion.
  transport.SetSlowModifier(true);
  transport.PressForward();
  CHECK(transport.rate() == 0.25);
  transport.PressReverse();
  CHECK(transport.rate() == -0.25);
  transport.SetSlowModifier(false);
  transport.Stop();

  // Stepping lands on frames, stops playback, and stays within the sequence.
  transport.Seek(S(2));
  transport.StepFrames(1);
  CHECK(transport.position().Compare(RationalTime::FromFrames(51, cutline::time::kFrameRate25)) == 0);
  transport.StepFrames(-5);
  CHECK(transport.position().Compare(RationalTime::FromFrames(46, cutline::time::kFrameRate25)) == 0);
  transport.StepFrames(-100000);
  CHECK(transport.position().Compare(S(0)) == 0);
  transport.StepFrames(100000);
  CHECK(transport.position().Compare(S(10)) == 0);
  transport.Seek(RationalTime(3, 100));                 // between frames: the nearest frame
  CHECK(transport.position().Compare(RationalTime::FromFrames(1, cutline::time::kFrameRate25)) == 0);

  // Playing to the end stops there and says so; playing again from the end starts over.
  transport.Seek(RationalTime(19, 2));
  transport.TogglePlay();
  tick = transport.Advance(1.0);
  CHECK(tick.reached_boundary && !transport.playing() && tick.position.Compare(S(10)) == 0);
  transport.TogglePlay();
  CHECK(transport.playing() && transport.position().Compare(S(0)) == 0);
  // Backward to the start stops at zero.
  transport.Stop();
  transport.Seek(S(1));
  transport.PressReverse();
  tick = transport.Advance(5.0);
  CHECK(tick.reached_boundary && tick.position.Compare(S(0)) == 0 && !transport.playing());

  // Looping over a marked range wraps instead of stopping.
  transport.SetLooping(true);
  transport.SetLoopRange(S(2), S(4));
  transport.Seek(RationalTime(88, 25));
  transport.TogglePlay();
  tick = transport.Advance(0.88);                        // 3.52 -> 4.4, wraps to 2.4
  CHECK(tick.looped && tick.position.Compare(RationalTime(12, 5)) == 0 && transport.playing());
  // With no marked range it loops the whole sequence.
  transport.SetLoopRange(std::nullopt, std::nullopt);
  transport.Seek(RationalTime(48, 5));
  tick = transport.Advance(0.8);
  CHECK(tick.looped && tick.position.Compare(RationalTime(2, 5)) == 0);
  // A zero or negative tick, and a stopped transport, do not move.
  transport.Stop();
  CHECK(!transport.Advance(1.0).moved);
  CHECK(!transport.Advance(-1.0).moved);
  // A shorter sequence pulls the playhead in.
  transport.Seek(S(9));
  transport.SetDuration(S(5));
  CHECK(transport.position().Compare(S(5)) == 0);
}

// ---------------------------------------------------------------- the inspector ----

CUTLINE_TEST(TheInspectorShowsEachEffectWithItsValuesAtTheTimeAndEditsThemAsOneStepEach) {
  Project project;
  project.Add("clip", "v1", 10, 0, 10);
  auto snap = project.Snapshot();
  auto ctx = project.Context(snap);

  // Add effects from the catalogue; the catalogue is grouped, searchable and leaves out what a clip has already.
  const auto catalogue = cutline::ui::EffectCatalogue();
  CHECK(catalogue.size() > 15);
  for (const auto& entry : catalogue) CHECK(entry.id != "opacity" && entry.id != "motion" && entry.id != "time_remap");
  CHECK(!cutline::ui::EffectCatalogue("blur").empty());
  CHECK(cutline::ui::EffectCatalogue("zzzzzz").empty());
  for (std::size_t i = 1; i < catalogue.size(); ++i) {
    CHECK(std::tie(catalogue[i - 1].category, catalogue[i - 1].name) <= std::tie(catalogue[i].category, catalogue[i].name));
  }
  auto plan = cutline::ui::PlanAddEffect(ctx, "clip", "gaussian_blur");
  CHECK(plan.ok && project.Apply(plan));
  snap = project.Snapshot();
  ctx = project.Context(snap);
  plan = cutline::ui::PlanAddEffect(ctx, "clip", "tint");
  CHECK(plan.ok && project.Apply(plan));
  snap = project.Snapshot();
  ctx = project.Context(snap);
  CHECK(!cutline::ui::PlanAddEffect(ctx, "clip", "no_such_effect").ok);
  CHECK(!cutline::ui::PlanAddEffect(ctx, "clip", "lut").ok);                       // needs a file
  CHECK(!cutline::ui::PlanAddEffect(ctx, "nothing", "tint").ok);

  const auto* clip = Find(snap, "clip");
  auto state = cutline::ui::BuildInspector(*clip, S(0));
  CHECK_EQ(state.effects.size(), std::size_t{2});
  CHECK_EQ(state.effects[0].type, std::string("gaussian_blur"));
  CHECK(state.effects[0].known && state.effects[0].display_name == "Gaussian Blur");
  const auto blur_id = state.effects[0].id;
  CHECK_EQ(state.effects[0].parameters.size(), std::size_t{1});
  const auto& radius = state.effects[0].parameters[0];
  CHECK(radius.name == "radius" && radius.display_name == "Radius");
  CHECK(radius.minimum.has_value() && *radius.minimum == 0.0 && radius.maximum.has_value() && *radius.maximum == 128.0);
  CHECK(radius.unit == cutline::effects::Unit::Pixels && !radius.keyframed && std::abs(radius.value.scalar()) < 1e-12);

  // Change the value: steady for the whole clip.
  plan = cutline::ui::PlanSetParameter(ctx, "clip", blur_id, "radius", cutline::anim::Value::Scalar(12.0), S(2));
  CHECK(plan.ok && plan.commands.size() == 1 && plan.label == "Change Radius");
  CHECK(project.Apply(plan));
  snap = project.Snapshot();
  ctx = project.Context(snap);
  state = cutline::ui::BuildInspector(*Find(snap, "clip"), S(5));
  CHECK(std::abs(state.effects[0].parameters[0].value.scalar() - 12.0) < 1e-12);
  // Out of range, wrong size, wrong effect: refused. A clamp helper brings a value inside.
  CHECK(!cutline::ui::PlanSetParameter(ctx, "clip", blur_id, "radius", cutline::anim::Value::Scalar(500.0), S(0)).ok);
  CHECK(!cutline::ui::PlanSetParameter(ctx, "clip", blur_id, "radius", cutline::anim::Value::Vec2(1, 2), S(0)).ok);
  CHECK(!cutline::ui::PlanSetParameter(ctx, "clip", "no-such-effect", "radius", cutline::anim::Value::Scalar(1.0), S(0)).ok);
  CHECK(!cutline::ui::PlanSetParameter(ctx, "clip", blur_id, "no-such-parameter", cutline::anim::Value::Scalar(1.0), S(0)).ok);
  CHECK(std::abs(cutline::ui::ClampToRange(state.effects[0].parameters[0], cutline::anim::Value::Scalar(500.0)).scalar() - 128.0) < 1e-12);
  CHECK(std::abs(cutline::ui::ClampToRange(state.effects[0].parameters[0], cutline::anim::Value::Scalar(-5.0)).scalar()) < 1e-12);

  // The stopwatch: animating puts a key at this time holding the current value; further changes add keys.
  plan = cutline::ui::PlanToggleAnimation(ctx, "clip", blur_id, "radius", S(1));
  CHECK(plan.ok && project.Apply(plan));
  snap = project.Snapshot();
  ctx = project.Context(snap);
  state = cutline::ui::BuildInspector(*Find(snap, "clip"), S(1));
  CHECK(state.effects[0].parameters[0].keyframed && state.effects[0].parameters[0].key_here && state.effects[0].parameters[0].key_times.size() == 1);
  plan = cutline::ui::PlanSetParameter(ctx, "clip", blur_id, "radius", cutline::anim::Value::Scalar(40.0), S(3));
  CHECK(plan.ok && project.Apply(plan));
  snap = project.Snapshot();
  ctx = project.Context(snap);
  state = cutline::ui::BuildInspector(*Find(snap, "clip"), S(2));
  CHECK(state.effects[0].parameters[0].key_times.size() == 2 && !state.effects[0].parameters[0].key_here);
  CHECK(std::abs(state.effects[0].parameters[0].value.scalar() - 26.0) < 1e-9);       // half way between 12 and 40
  // Navigation between keys.
  const auto& row = state.effects[0].parameters[0];
  CHECK(cutline::ui::NextKeyframe(row, S(2)).value().Compare(S(3)) == 0);
  CHECK(cutline::ui::PreviousKeyframe(row, S(2)).value().Compare(S(1)) == 0);
  CHECK(!cutline::ui::NextKeyframe(row, S(3)).has_value() && !cutline::ui::PreviousKeyframe(row, S(1)).has_value());
  // Toggle a key off (not the last one), then on again at another time.
  plan = cutline::ui::PlanToggleKeyframe(ctx, "clip", blur_id, "radius", S(3));
  CHECK(plan.ok && plan.label == "Delete Keyframe" && project.Apply(plan));
  snap = project.Snapshot();
  ctx = project.Context(snap);
  plan = cutline::ui::PlanToggleKeyframe(ctx, "clip", blur_id, "radius", S(6));
  CHECK(plan.ok && plan.label == "Add Keyframe" && project.Apply(plan));
  snap = project.Snapshot();
  ctx = project.Context(snap);
  CHECK_EQ(cutline::ui::BuildInspector(*Find(snap, "clip"), S(0)).effects[0].parameters[0].key_times.size(), std::size_t{2});
  // Stopping animation keeps the value at the playhead and removes every key.
  plan = cutline::ui::PlanToggleAnimation(ctx, "clip", blur_id, "radius", S(6));
  CHECK(plan.ok && project.Apply(plan));
  snap = project.Snapshot();
  ctx = project.Context(snap);
  state = cutline::ui::BuildInspector(*Find(snap, "clip"), S(0));
  CHECK(!state.effects[0].parameters[0].keyframed);
  CHECK(std::abs(state.effects[0].parameters[0].value.scalar() - 12.0) < 1e-9);       // 6 s is past the last key (1 s): it held 12
  // Reset returns to the default.
  plan = cutline::ui::PlanResetParameter(ctx, "clip", blur_id, "radius");
  CHECK(plan.ok && project.Apply(plan));
  snap = project.Snapshot();
  ctx = project.Context(snap);
  CHECK(std::abs(cutline::ui::BuildInspector(*Find(snap, "clip"), S(0)).effects[0].parameters[0].value.scalar()) < 1e-12);

  // Disable, reorder, remove.
  plan = cutline::ui::PlanSetEffectEnabled(ctx, "clip", blur_id, false);
  CHECK(plan.ok && project.Apply(plan));
  snap = project.Snapshot();
  ctx = project.Context(snap);
  CHECK(!cutline::ui::BuildInspector(*Find(snap, "clip"), S(0)).effects[0].enabled);
  CHECK(!cutline::ui::PlanSetEffectEnabled(ctx, "clip", blur_id, false).ok);
  plan = cutline::ui::PlanMoveEffect(ctx, "clip", blur_id, 1);
  CHECK(plan.ok && project.Apply(plan));
  snap = project.Snapshot();
  ctx = project.Context(snap);
  state = cutline::ui::BuildInspector(*Find(snap, "clip"), S(0));
  CHECK_EQ(state.effects[0].type, std::string("tint"));
  CHECK_EQ(state.effects[1].id, blur_id);
  CHECK(!cutline::ui::PlanMoveEffect(ctx, "clip", blur_id, 1).ok);                    // already last
  plan = cutline::ui::PlanRemoveEffect(ctx, "clip", blur_id);
  CHECK(plan.ok && project.Apply(plan));
  snap = project.Snapshot();
  CHECK_EQ(cutline::ui::BuildInspector(*Find(snap, "clip"), S(0)).effects.size(), std::size_t{1});
  // A locked track refuses every edit.
  project.Run(cmd::CommandType::SetTrackState, cmd::SetTrackStatePayload{"v1", true, false, false, 0.0, 0.0, "V1"});
  snap = project.Snapshot();
  ctx = project.Context(snap);
  CHECK(!cutline::ui::PlanAddEffect(ctx, "clip", "tint").ok);
  CHECK(!cutline::ui::PlanSetParameter(ctx, "clip", state.effects[0].id, "amount", cutline::anim::Value::Scalar(0.5), S(0)).ok);
}

CUTLINE_TEST(AnEffectTheRegistryDoesNotKnowIsStillShownWithoutRanges) {
  // A clip carrying an effect from a plug-in or a newer build.
  cutline::timeline::Clip clip;
  clip.id = "c";
  cutline::timeline::Effect effect;
  effect.id = "fx";
  effect.effect_type = "from_the_future";
  cutline::timeline::Parameter parameter;
  parameter.id = "fx:strength";
  parameter.name = "strength";
  parameter.value = cutline::anim::AnimatedValue(cutline::anim::Value::Scalar(0.7));
  effect.parameters.push_back(parameter);
  clip.effects.push_back(effect);
  const auto state = cutline::ui::BuildInspector(clip, S(0));
  CHECK_EQ(state.effects.size(), std::size_t{1});
  CHECK(!state.effects[0].known && state.effects[0].display_name == "from_the_future" && state.effects[0].category == "Other");
  CHECK(state.effects[0].parameters[0].display_name == "strength" && !state.effects[0].parameters[0].minimum.has_value());
  CHECK(std::abs(state.effects[0].parameters[0].value.scalar() - 0.7) < 1e-12);
}

// ----------------------------------------------------------------------- jobs ----

CUTLINE_TEST(JobsReportProgressCanBeCancelledAndFailuresAreRecordedNotThrown) {
  using namespace cutline::ui;
  JobTracker tracker;
  std::mutex mutex;
  std::vector<JobState> seen;
  tracker.Observe([&](const JobInfo& info) {
    const std::lock_guard<std::mutex> lock(mutex);
    seen.push_back(info.state);
  });
  JobRunner runner(tracker);
  // A job that finishes, reporting as it goes.
  const auto good = runner.Run("tracking", "Tracking Clip 1", [](JobContext& context) {
    for (std::uint64_t i = 1; i <= 4; ++i) context.Progress(i, 4, "frame " + std::to_string(i));
  });
  // One that fails.
  const auto bad = runner.Run("export", "Exporting", [](JobContext&) { throw std::runtime_error("disk full"); });
  // One that runs until it is told to stop.
  std::atomic<bool> running{false};
  const auto long_job = runner.Run("optical_flow", "Analysing", [&](JobContext& context) {
    running = true;
    while (!context.cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  });
  while (!running.load()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  CHECK(tracker.ActiveCount() >= 1);
  tracker.RequestCancel(long_job);
  runner.WaitAll();
  const auto a = tracker.Find(good);
  const auto b = tracker.Find(bad);
  const auto c = tracker.Find(long_job);
  CHECK(a.has_value() && a->state == JobState::Succeeded && a->progress == 1.0 && a->message == "frame 4");
  CHECK(b.has_value() && b->state == JobState::Failed && b->error == "disk full");
  CHECK(c.has_value() && c->state == JobState::Cancelled);
  CHECK_EQ(tracker.ActiveCount(), 0);
  CHECK_EQ(tracker.Snapshot().size(), std::size_t{3});
  {
    const std::lock_guard<std::mutex> lock(mutex);
    CHECK(std::count(seen.begin(), seen.end(), JobState::Succeeded) == 1 && std::count(seen.begin(), seen.end(), JobState::Failed) == 1 && std::count(seen.begin(), seen.end(), JobState::Cancelled) == 1);
    CHECK(std::count(seen.begin(), seen.end(), JobState::Running) >= 3);
    CHECK(seen.front() == JobState::Queued);
  }
  tracker.ClearFinished();
  CHECK(tracker.Snapshot().empty());

  // A job cancelled before it starts never runs; progress of an unknown total is negative; finished jobs ignore updates.
  JobTracker other;
  const auto queued = other.Create("proxy", "Proxy");
  other.RequestCancel(queued);
  CHECK(other.Find(queued)->state == JobState::Cancelled);
  other.Start(queued);
  CHECK(other.Find(queued)->state == JobState::Cancelled);
  const auto open = other.Create("proxy", "Proxy 2");
  other.Start(open);
  other.Progress(open, 5, 0);
  CHECK(other.Find(open)->progress < 0.0);
  other.Progress(open, 50, 100);
  CHECK(std::abs(other.Find(open)->progress - 0.5) < 1e-12);
  other.Succeed(open, "done");
  other.Progress(open, 1, 100);
  other.Fail(open, "late");
  CHECK(other.Find(open)->state == JobState::Succeeded && other.Find(open)->progress == 1.0);
  CHECK(!other.Find(9999).has_value() && !other.CancelRequested(9999));
  // The token an analysis job polls.
  const auto token_job = other.Create("tracking", "T");
  const auto token = other.CancelToken(token_job);
  CHECK(!token());
  other.RequestCancel(token_job);
  CHECK(token());
}

// --------------------------------------------------------------------- the LUT browser ----

CUTLINE_TEST(TheLutBrowserFindsCubeFilesReportsBrokenOnesSearchesAndPreviewsTheLook) {
  namespace fs = std::filesystem;
  const auto directory = fs::temp_directory_path() / "cutline-test-luts";
  fs::remove_all(directory);
  fs::create_directories(directory / "film");
  // A 2-point identity LUT, a 2-point LUT that swaps red and blue, and a broken file.
  const auto write = [](const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
  };
  write(directory / "identity.cube", "TITLE \"Identity\"\nLUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n");
  write(directory / "film" / "Swap Red Blue.CUBE", "LUT_3D_SIZE 2\n0 0 0\n0 0 1\n0 1 0\n0 1 1\n1 0 0\n1 0 1\n1 1 0\n1 1 1\n");
  write(directory / "film" / "broken.cube", "this is not a lut");
  write(directory / "notes.txt", "not a lut either");
  cutline::ui::LutLibrary library;
  library.SetFolders({directory, directory / "nowhere"});
  library.Rescan();
  CHECK_EQ(library.entries().size(), std::size_t{3});
  CHECK_EQ(library.entries()[0].name, std::string("broken"));      // sorted by name, case-insensitively
  CHECK(!library.entries()[0].error.empty());
  CHECK_EQ(library.entries()[1].name, std::string("identity"));
  CHECK_EQ(library.entries()[1].size, 2);
  CHECK_EQ(library.entries()[2].name, std::string("Swap Red Blue"));
  CHECK_EQ(library.Search("").size(), std::size_t{2});             // broken ones are left out
  CHECK_EQ(library.Search("", true).size(), std::size_t{3});
  CHECK_EQ(library.Search("swap blue").size(), std::size_t{1});
  CHECK_EQ(library.Search("SWAP").size(), std::size_t{1});
  CHECK(library.Search("zzz").empty());
  CHECK(library.Find(directory / "identity.cube") != nullptr && library.Find(directory / "x.cube") == nullptr);
  // Scanning again after a file is added picks it up; the same folder listed twice does not double entries.
  write(directory / "later.cube", "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n");
  library.SetFolders({directory, directory});
  library.Rescan();
  CHECK_EQ(library.entries().size(), std::size_t{4});

  // The reference stored on the effect is relative inside the project's asset root.
  CHECK_EQ(cutline::ui::AssetReferenceFor(directory / "film" / "x.cube", directory), std::string("film/x.cube"));
  CHECK(cutline::ui::AssetReferenceFor(directory / "film" / "x.cube", directory / "film" / "other").find("..") == std::string::npos);
  CHECK(cutline::ui::AssetReferenceFor(directory / "film" / "x.cube", {}).find("film/x.cube") != std::string::npos);

  // The preview: the chart through an identity LUT is the chart; through the swap, red and blue trade places.
  const auto chart = cutline::ui::TestChart(120, 80);
  CHECK(chart.width() == 120 && chart.height() == 80);
  const auto same = cutline::ui::ApplyLutTo(chart, directory / "identity.cube");
  const auto swapped = cutline::ui::ApplyLutTo(chart, directory / "film" / "Swap Red Blue.CUBE");
  const auto half = cutline::ui::ApplyLutTo(chart, directory / "film" / "Swap Red Blue.CUBE", 0.5f);
  double worst_identity = 0.0, worst_swap = 0.0, worst_half = 0.0;
  for (int y = 0; y < chart.height(); ++y) {
    for (int x = 0; x < chart.width(); ++x) {
      const auto* original = chart.row_f32(y) + x * 4;
      const auto* a = same.row_f32(y) + x * 4;
      const auto* b = swapped.row_f32(y) + x * 4;
      const auto* c = half.row_f32(y) + x * 4;
      for (int k = 0; k < 3; ++k) worst_identity = std::max(worst_identity, static_cast<double>(std::abs(a[k] - original[k])));
      worst_swap = std::max({worst_swap, static_cast<double>(std::abs(b[0] - original[2])), static_cast<double>(std::abs(b[2] - original[0])), static_cast<double>(std::abs(b[1] - original[1]))});
      worst_half = std::max(worst_half, static_cast<double>(std::abs(c[0] - 0.5f * (original[0] + original[2]))));
    }
  }
  CHECK(worst_identity < 1e-5 && worst_swap < 1e-5 && worst_half < 1e-5);
  CHECK_THROWS(cutline::ui::ApplyLutTo(chart, directory / "film" / "broken.cube"));
  fs::remove_all(directory);
}

// ------------------------------------------------------------------ speed ramps ----

namespace {

double SourceAtTimeline(Project& project, double timeline_seconds) {
  const auto graph = cutline::timeline::LoadSequenceGraph(project.store(), "seq");
  const auto plan = cutline::timeline::TimelineCompiler{}.Compile(graph, RationalTime(static_cast<std::int64_t>(std::llround(timeline_seconds * 1000)), 1000));
  if (plan.video.empty()) return -1.0;
  return static_cast<double>(plan.video[0].source_time.numerator()) / static_cast<double>(plan.video[0].source_time.denominator());
}

}  // namespace

CUTLINE_TEST(ARampIsEditedAsASpeedGraphAndEveryEditKeepsItsShapeAndLength) {
  using cutline::ui::RampEase;
  using cutline::ui::RampModel;
  auto model = RampModel::Constant(S(6), 1.0, cutline::time::kFrameRate25);
  CHECK(model.segments().size() == 1 && model.duration().Compare(S(6)) == 0);
  CHECK(std::abs(model.SpeedAt(S(3)) - 1.0) < 1e-12 && std::abs(model.SourceSecondsAt(S(4)) - 4.0) < 1e-12);
  // Cut at 2 s: the shape is unchanged. Then give the cut a speed of 3: a ramp 1 -> 3 over the second part is not what that
  // makes (it is two segments, 1 -> 3 and 3 -> 1), so check the numbers the integral gives.
  CHECK(model.SplitAt(S(2)));
  CHECK(model.segments().size() == 2 && model.segments()[0].duration.Compare(S(2)) == 0 && model.segments()[1].duration.Compare(S(4)) == 0);
  CHECK(std::abs(model.SourceSecondsAt(S(6)) - 6.0) < 1e-12);
  CHECK(!model.SplitAt(S(2)));              // already a boundary
  CHECK(!model.SplitAt(S(9)));              // outside
  CHECK(model.SetBoundarySpeed(1, 3.0));
  CHECK(std::abs(model.segments()[0].end_speed - 3.0) < 1e-12 && std::abs(model.segments()[1].start_speed - 3.0) < 1e-12);
  // 2 s from 1 to 3 sweeps 4 s of source; 4 s from 3 to 1 sweeps 8.
  CHECK(std::abs(model.SourceSecondsAt(S(2)) - 4.0) < 1e-12 && std::abs(model.SourceSecondsAt(S(6)) - 12.0) < 1e-12);
  CHECK(std::abs(model.SpeedAt(S(1)) - 2.0) < 1e-12 && std::abs(model.SpeedAt(S(4)) - 2.0) < 1e-12);
  CHECK(model.SetBoundarySpeed(1, 0.5, RampModel::Side::After));       // a jump: only the later side
  CHECK(std::abs(model.segments()[0].end_speed - 3.0) < 1e-12 && std::abs(model.segments()[1].start_speed - 0.5) < 1e-12);
  CHECK(!model.SetBoundarySpeed(1, 500.0) && !model.SetBoundarySpeed(5, 1.0));
  CHECK(model.GraphPoints().size() == 4);
  // Moving a boundary trades length between neighbours and keeps the total; it cannot squeeze a segment below a frame.
  CHECK(model.MoveBoundary(1, S(3)));
  CHECK(model.segments()[0].duration.Compare(S(3)) == 0 && model.segments()[1].duration.Compare(S(3)) == 0 && model.duration().Compare(S(6)) == 0);
  CHECK(model.MoveBoundary(1, S(100)));
  CHECK(model.segments()[1].duration.Compare(RationalTime(1, 25)) == 0 && model.duration().Compare(S(6)) == 0);
  CHECK(model.MoveBoundary(1, S(-5)));
  CHECK(model.segments()[0].duration.Compare(RationalTime(1, 25)) == 0);
  CHECK(!model.MoveBoundary(0, S(1)) && !model.MoveBoundary(2, S(1)));
  CHECK(model.MoveBoundary(1, S(2)));
  // Joining two segments back, freezing, reversing.
  CHECK(model.RemoveBoundary(1));
  CHECK(model.segments().size() == 1 && model.duration().Compare(S(6)) == 0);
  CHECK(!model.RemoveBoundary(1) && !model.RemoveBoundary(0));
  model.SplitAt(S(3));
  CHECK(model.Freeze(0) && model.segments()[0].start_speed == 0.0 && model.segments()[0].end_speed == 0.0);
  CHECK(model.Reverse(1) && model.segments()[1].start_speed < 0.0);
  CHECK(!model.Freeze(7) && !model.Reverse(-1));
  // A freeze followed by a reverse reaches back past where the picture started.
  const auto extent = model.SourceExtent();
  CHECK(extent.first < 0.0 && extent.second == 0.0);

  // Easing replaces a segment with steps that follow the curve: same length, same ends, monotone speed.
  auto eased = RampModel::Constant(S(4), 1.0, cutline::time::kFrameRate25);
  CHECK(eased.SetSegmentSpeed(0, 1.0, 5.0));
  for (const auto ease : {RampEase::EaseIn, RampEase::EaseOut, RampEase::EaseInOut}) {
    auto copy = eased;
    CHECK(copy.SetEase(0, ease));
    CHECK(copy.segments().size() > 3 && copy.duration().Compare(S(4)) == 0);
    CHECK(std::abs(copy.segments().front().start_speed - 1.0) < 1e-12 && std::abs(copy.segments().back().end_speed - 5.0) < 1e-12);
    for (std::size_t i = 0; i < copy.segments().size(); ++i) {
      CHECK(copy.segments()[i].end_speed >= copy.segments()[i].start_speed - 1e-12);
      if (i > 0) CHECK(std::abs(copy.segments()[i].start_speed - copy.segments()[i - 1].end_speed) < 1e-12);   // continuous
    }
    // At the middle, ease-in is slower than a straight line, ease-out faster, ease-in-out the same.
    const auto mid = copy.SpeedAt(S(2));
    if (ease == RampEase::EaseIn) CHECK(mid < 2.9);
    if (ease == RampEase::EaseOut) CHECK(mid > 3.1);
    if (ease == RampEase::EaseInOut) CHECK(std::abs(mid - 3.0) < 0.5);
  }
  auto linear = eased;
  CHECK(linear.SetEase(0, RampEase::Linear) && linear.segments().size() == 1);
  CHECK(eased.ScaleSpeeds(2.0) && std::abs(eased.segments()[0].end_speed - 10.0) < 1e-12);
  CHECK(!eased.ScaleSpeeds(1000.0) && !eased.ScaleSpeeds(-1.0));
  CHECK(eased.SetLastDuration(S(8)) && eased.duration().Compare(S(8)) == 0);

  // What can and cannot be applied.
  auto check = RampModel::Constant(S(4), 2.0, cutline::time::kFrameRate25);          // sweeps 8 s
  CHECK(check.Check(S(10), RationalTime(60, 1)).empty());
  CHECK(!check.Check(S(55), RationalTime(60, 1)).empty());                            // runs past the end
  auto back = RampModel::Constant(S(4), -2.0, cutline::time::kFrameRate25);
  CHECK(!back.Check(S(3), RationalTime(60, 1)).empty());                              // back past the start
  CHECK(back.Check(S(9), RationalTime(60, 1)).empty());
  CHECK(!RampModel::Constant(S(4), 0.0, cutline::time::kFrameRate25).Check(S(3), {}).empty());   // goes nowhere
  CHECK(!RampModel().Check(S(3), {}).empty());
}

CUTLINE_TEST(ARampWrittenToAClipComesBackAsTheSameGraphAndReEditingKeepsWhereItStarts) {
  using cutline::ui::RampModel;
  Project project;
  project.Add("clip", "v1", 0, 10, 20);
  auto snap = project.Snapshot();

  // A ramp with every kind of segment: steady, a ramp up, a freeze, a jump to double speed.
  RampModel model = RampModel::Constant(S(7), 1.0, cutline::time::kFrameRate25);
  CHECK(model.SplitAt(S(2)) && model.SplitAt(S(4)) && model.SplitAt(S(5)));
  CHECK(model.SetSegmentSpeed(0, 1.0, 1.0) && model.SetSegmentSpeed(1, 1.0, 3.0) && model.SetSegmentSpeed(2, 0.0, 0.0) && model.SetSegmentSpeed(3, 2.0, 2.0));
  auto plan = cutline::ui::PlanApplyRamp(project.Context(snap), "clip", model);
  CHECK(plan.ok);
  CHECK(project.Apply(plan));
  snap = project.Snapshot();
  const auto* stored = Find(snap, "clip");
  CHECK(stored->end().Compare(S(7)) == 0);

  // Read back: the same four segments, to a frame and a few hundredths of a speed.
  const auto back = RampModel::FromClip(*stored, cutline::time::kFrameRate25);
  Expect(back.segments().size() == 4, "read back " + std::to_string(back.segments().size()) + " segments", __LINE__);
  if (back.segments().size() == 4) {
    const double lengths[4] = {2, 2, 1, 2};
    const double starts[4] = {1, 1, 0, 2}, ends[4] = {1, 3, 0, 2};
    for (std::size_t i = 0; i < 4; ++i) {
      CHECK(std::abs(static_cast<double>(back.segments()[i].duration.numerator()) / back.segments()[i].duration.denominator() - lengths[i]) <= 0.041);
      CHECK(std::abs(back.segments()[i].start_speed - starts[i]) < 0.06 && std::abs(back.segments()[i].end_speed - ends[i]) < 0.06);
    }
  }
  CHECK(back.duration().Compare(S(7)) == 0);

  // A clip with no ramp reads as its steady speed, backward if reversed.
  Project plain;
  plain.Add("p", "v1", 0, 0, 10);
  plain.Run(cmd::CommandType::SetClipSpeed, cmd::SetClipSpeedPayload{"p", {2, 1}, true, true, false});
  const auto steady = RampModel::FromClip(*Find(plain.Snapshot(), "p"), cutline::time::kFrameRate25);
  (void)steady;
  const auto psnap = plain.Snapshot();
  const auto steady_model = RampModel::FromClip(*Find(psnap, "p"), cutline::time::kFrameRate25);
  CHECK(steady_model.segments().size() == 1 && std::abs(steady_model.segments()[0].start_speed + 2.0) < 1e-9 && steady_model.duration().Compare(S(5)) == 0);

  // Edit the stored ramp and apply it again: the picture still starts where it did, and its length follows.
  const double start_before = SourceAtTimeline(project, 0.0);
  auto edited = back;
  CHECK(edited.MoveBoundary(2, S(5)));                // the freeze starts a second later
  plan = cutline::ui::PlanApplyRamp(project.Context(snap), "clip", edited);
  CHECK(plan.ok && project.Apply(plan));
  CHECK(std::abs(SourceAtTimeline(project, 0.0) - start_before) < 1e-3);
  CHECK(std::abs(SourceAtTimeline(project, 3.9) - (start_before + edited.SourceSecondsAt(RationalTime(39, 10)))) < 0.05);
  CHECK_NO_THROW(project.store().ValidateDatabase());
  // One undo takes the edit back to the first ramp.
  (void)project.store().Undo("tester", "2026-10-06T01:00:00Z");
  CHECK(std::abs(SourceAtTimeline(project, 3.0) - (start_before + model.SourceSecondsAt(S(3)))) < 0.05);

  // A ramp that begins by running backward starts at the clip's start, and re-editing does not move it.
  Project reverse;
  reverse.Add("r", "v1", 0, 20, 40);
  auto rsnap = reverse.Snapshot();
  RampModel first = RampModel::Constant(S(6), -1.0, cutline::time::kFrameRate25);
  first.SplitAt(S(3));
  first.SetSegmentSpeed(1, 1.0, 1.0);
  plan = cutline::ui::PlanApplyRamp(reverse.Context(rsnap), "r", first);
  CHECK(plan.ok && reverse.Apply(plan));
  const auto where = SourceAtTimeline(reverse, 0.0);
  CHECK(std::abs(where - 20.0) < 1e-3);
  rsnap = reverse.Snapshot();
  auto changed = RampModel::FromClip(*Find(rsnap, "r"), cutline::time::kFrameRate25);
  CHECK(changed.MoveBoundary(1, S(2)));
  plan = cutline::ui::PlanApplyRamp(reverse.Context(rsnap), "r", changed);
  CHECK(plan.ok && reverse.Apply(plan));
  CHECK(std::abs(SourceAtTimeline(reverse, 0.0) - 20.0) < 1e-3);

  // Refused: past the media's end; an unramped clip cannot be cleared; clearing keeps the length.
  auto too_long = RampModel::Constant(S(30), 4.0, cutline::time::kFrameRate25);
  CHECK(!cutline::ui::PlanApplyRamp(reverse.Context(rsnap), "r", too_long).ok);
  CHECK(!cutline::ui::PlanClearRamp(project.Context(project.Snapshot()), "nothing").ok);
  Project none;
  none.Add("n", "v1", 0, 0, 5);
  CHECK(!cutline::ui::PlanClearRamp(none.Context(none.Snapshot()), "n").ok);
  const auto cleared = cutline::ui::PlanClearRamp(project.Context(project.Snapshot()), "clip");
  CHECK(cleared.ok && project.Apply(cleared));
  CHECK(Find(project.Snapshot(), "clip") != nullptr);
}

// ------------------------------------------------------------------- multicam ----

namespace {

cutline::ui::GroupSetup TwoClips() {
  cutline::ui::GroupSetup setup;
  setup.name = "Interview";
  cutline::ui::AngleDraft a, b;
  a.media_id = "m1";
  a.duration = S(60);
  b.media_id = "m2";
  b.duration = S(60);
  setup.angles = {a, b};
  return setup;
}

cutline::timeline::multicam::Group LoadMc(Project& project, const std::string& id) { return cutline::timeline::multicam::LoadGroup(project.store(), id); }

}  // namespace

CUTLINE_TEST(AMulticamGroupIsSetUpFromClipsWithUniqueNamesAReferenceAndASyncAndStartsOnTheReference) {
  namespace mc = cutline::timeline::multicam;
  Project project;
  const auto ids = [&](const std::string& prefix) { return prefix + "-" + std::to_string(project.NextId()); };
  // Names: typed, else the clip's, else "Angle N"; a repeat gets a number.
  {
    auto setup = TwoClips();
    setup.angles[0].name = "Wide";
    setup.angles[1].name = "Wide";
    const auto names = cutline::ui::AngleNames(setup.angles, {"m1", "m2"});
    CHECK(names[0] == "Wide" && names[1] == "Wide 2");
    setup.angles[1].name.clear();
    CHECK(cutline::ui::AngleNames(setup.angles, {"", ""})[1] == "Angle 2");
    CHECK(cutline::ui::AngleNames(setup.angles, {"m1", "Close"})[1] == "Close");
  }
  // What cannot be set up.
  {
    auto one = TwoClips();
    one.angles.pop_back();
    CHECK(!cutline::ui::PlanCreateGroup(one, {}, ids).plan.ok);
    auto same = TwoClips();
    same.angles[1].media_id = "m1";
    CHECK(!cutline::ui::PlanCreateGroup(same, {}, ids).plan.ok);
    auto blind = TwoClips();
    blind.angles[1].has_video = false;
    CHECK(!cutline::ui::PlanCreateGroup(blind, {}, ids).plan.ok);
    auto bad_reference = TwoClips();
    bad_reference.reference = 5;
    CHECK(!cutline::ui::PlanCreateGroup(bad_reference, {}, ids).plan.ok);
    auto no_marker = TwoClips();
    no_marker.sync = cutline::ui::SyncChoice::Marker;
    CHECK(!cutline::ui::PlanCreateGroup(no_marker, {}, ids).plan.ok);
    auto no_sound = TwoClips();
    no_sound.sync = cutline::ui::SyncChoice::Audio;
    CHECK(!cutline::ui::PlanCreateGroup(no_sound, {}, ids).plan.ok);
  }
  // Sync by marker: the second camera's clap is 2 s into its clip and the first one's is 0.5 s in, so the second starts
  // 1.5 s later in its own time; the group runs to the end of the longer one.
  {
    auto setup = TwoClips();
    setup.name = "Marked";
    setup.sync = cutline::ui::SyncChoice::Marker;
    setup.angles[0].marker = RationalTime(1, 2);
    setup.angles[1].marker = S(2);
    const auto result = cutline::ui::PlanCreateGroup(setup, {"First", "Second"}, ids);
    CHECK(result.plan.ok);
    CHECK(result.offsets[0].Compare(S(0)) == 0 && result.offsets[1].Compare(RationalTime(3, 2)) == 0);
    CHECK(project.Apply(result.plan));
    const auto group = LoadMc(project, result.group_id);
    CHECK(group.name == "Marked" && group.sync_method == "marker" && group.angles.size() == 2);
    CHECK(group.angles[0].name == "First" && group.angles[1].name == "Second");
    CHECK(group.angles[1].source_offset.Compare(RationalTime(3, 2)) == 0);
    CHECK(group.duration.Compare(S(60)) == 0);
    // The programme starts on the reference angle: one cut at zero.
    CHECK(group.switches.size() == 1 && group.switches[0].timeline_time.Compare(S(0)) == 0 && group.switches[0].angle_id == group.angles[0].id);
    // One undo takes the whole setup away.
    (void)project.store().Undo("tester", "2026-10-07T00:00:00Z");
    CHECK(cutline::timeline::multicam::ListGroupIds(project.store()).empty());
  }
  // Sync by timecode, by hand, and by sound; the reference may be any angle.
  {
    auto setup = TwoClips();
    setup.sync = cutline::ui::SyncChoice::Timecode;
    setup.angles[0].timecode_start = S(100);
    setup.angles[1].timecode_start = S(98);
    const auto timecode = cutline::ui::PlanCreateGroup(setup, {}, ids);
    CHECK(timecode.plan.ok && timecode.offsets[1].Compare(S(2)) == 0);
    setup.sync = cutline::ui::SyncChoice::Manual;
    setup.reference = 1;
    setup.angles[0].offset = S(5);
    setup.angles[1].offset = S(1);
    const auto manual = cutline::ui::PlanCreateGroup(setup, {}, ids);
    CHECK(manual.plan.ok && manual.offsets[1].Compare(S(0)) == 0 && manual.offsets[0].Compare(S(4)) == 0);
    // The programme starts on the reference, here the second clip.
    CHECK(project.Apply(manual.plan));
    const auto group = LoadMc(project, manual.group_id);
    CHECK(group.switches.size() == 1 && group.switches[0].angle_id == group.angles[1].id && group.reference_angle_id == group.angles[1].id);
    // Sound: a clap at 0.50 s in the reference and 0.80 s in the other lines them 0.30 s apart.
    auto sound = TwoClips();
    sound.sync = cutline::ui::SyncChoice::Audio;
    sound.angles[0].envelope.assign(200, 0.01f);
    sound.angles[1].envelope.assign(200, 0.01f);
    for (int i = 0; i < 3; ++i) {
      sound.angles[0].envelope[static_cast<std::size_t>(50 + i)] = 0.9f - 0.2f * static_cast<float>(i);
      sound.angles[1].envelope[static_cast<std::size_t>(80 + i)] = 0.9f - 0.2f * static_cast<float>(i);
    }
    const auto heard = cutline::ui::PlanCreateGroup(sound, {}, ids);
    CHECK(heard.plan.ok);
    CHECK(heard.offsets[1].Compare(RationalTime(3, 10)) == 0);
    CHECK(heard.confidence > 0.9);
  }
}

CUTLINE_TEST(LiveCutsAreSnappedToFramesAndRefinedByMovingNudgingChangingAndRemovingThemWithinTheirNeighbours) {
  namespace mc = cutline::timeline::multicam;
  Project project;
  project.Run(cmd::CommandType::ImportMedia, [] {
    cmd::ImportMediaPayload media;
    media.id = "m3";
    media.display_name = "m3";
    media.original_path = "/m3";
    media.fingerprint = "fp-m3";
    media.duration = S(10);   // a short third camera
    return media;
  }());
  int serial = 0;
  const auto ids = [&](const std::string& prefix) { return prefix + "-" + std::to_string(++serial); };
  auto setup = TwoClips();
  cutline::ui::AngleDraft third;
  third.media_id = "m3";
  third.duration = S(10);
  setup.angles.push_back(third);
  setup.sync = cutline::ui::SyncChoice::Manual;
  const auto made = cutline::ui::PlanCreateGroup(setup, {"Wide", "Close", "Cutaway"}, ids);
  CHECK(made.plan.ok && project.Apply(made.plan));
  const auto id = made.group_id;
  const auto rate = cutline::time::kFrameRate25;
  const auto frame = RationalTime(1, 25);

  // running yet is refused.
  auto plan = cutline::ui::PlanCutTo(LoadMc(project, id), 1, RationalTime(301, 100), rate);
  CHECK(plan.ok && plan.commands.size() == 1 && plan.result_time->Compare(RationalTime(75, 25)) == 0);
  CHECK(project.Apply(plan));
  plan = cutline::ui::PlanCutTo(LoadMc(project, id), 1, S(4), rate);
  CHECK(plan.ok && plan.commands.empty() && !plan.notes.empty());
  CHECK(!cutline::ui::PlanCutTo(LoadMc(project, id), 2, S(20), rate).ok);       // the cutaway is only 10 s long
  CHECK(!cutline::ui::PlanCutTo(LoadMc(project, id), 7, S(1), rate).ok);
  CHECK(!cutline::ui::PlanCutTo(LoadMc(project, id), 0, S(61), rate).ok);
  CHECK(project.Apply(cutline::ui::PlanCutTo(LoadMc(project, id), 2, S(6), rate)));
  CHECK(project.Apply(cutline::ui::PlanCutTo(LoadMc(project, id), 0, S(8), rate)));
  auto group = LoadMc(project, id);
  CHECK(group.switches.size() == 4);   // 0 wide, 3 close, 6 cutaway, 8 wide
  CHECK(cutline::ui::ShowingAt(group, S(7))->name == "Cutaway" && cutline::ui::ShowingAt(group, S(9))->name == "Wide");

  // Moving the cut at 6 s: later by a second, snapped to a frame, and never past a neighbour or onto the start.
  plan = cutline::ui::PlanMoveCut(group, S(6), RationalTime(701, 100), rate);
  CHECK(plan.ok && plan.commands.size() == 2 && plan.result_time->Compare(RationalTime(175, 25)) == 0);
  CHECK(project.Apply(plan));
  group = LoadMc(project, id);
  CHECK(group.switches[2].timeline_time.Compare(RationalTime(175, 25)) == 0 && group.switches[2].angle_id == group.angles[2].id);
  plan = cutline::ui::PlanMoveCut(group, RationalTime(175, 25), S(30), rate);       // past the cut at 8 s: stops a frame short
  CHECK(plan.ok && plan.result_time->Compare(S(8).Subtract(frame)) == 0);
  plan = cutline::ui::PlanMoveCut(group, RationalTime(175, 25), S(1), rate);        // before the cut at 3 s: stops a frame after
  CHECK(plan.ok && plan.result_time->Compare(S(3).Add(frame)) == 0);
  CHECK(!cutline::ui::PlanMoveCut(group, S(0), S(1), rate).ok);                       // the first cut cannot move
  CHECK(!cutline::ui::PlanMoveCut(group, S(5), S(6), rate).ok);                       // there is no cut at 5 s
  plan = cutline::ui::PlanNudgeCut(group, S(3), -3, rate);
  CHECK(plan.ok && plan.result_time->Compare(S(3).Subtract(RationalTime(3, 25))) == 0);
  CHECK(project.Apply(plan));
  group = LoadMc(project, id);
  CHECK(group.switches[1].timeline_time.Compare(RationalTime(72, 25)) == 0);

  // Changing a cut's angle keeps its time; removing one lets the angle before it run on; the first can only be changed.
  CHECK(project.Apply(cutline::ui::PlanChangeCutAngle(group, S(8), 1)));
  group = LoadMc(project, id);
  CHECK(group.switches.size() == 4 && group.switches[3].angle_id == group.angles[1].id);
  CHECK(!cutline::ui::PlanChangeCutAngle(group, S(9), 1).ok);
  CHECK(!cutline::ui::PlanRemoveCut(group, S(0)).ok);
  CHECK(project.Apply(cutline::ui::PlanRemoveCut(group, RationalTime(175, 25))));
  group = LoadMc(project, id);
  // With the cutaway gone, Close runs through to the end and the cut that was to Close coalesces away.
  CHECK(group.switches.size() == 2);
  CHECK(cutline::ui::ShowingAt(group, S(9))->name == "Close");

  // Names: renamed through a command, refused when empty or taken.
  CHECK(!cutline::ui::PlanRenameAngle(group, 0, "").ok && !cutline::ui::PlanRenameAngle(group, 0, "Close").ok);
  CHECK(project.Apply(cutline::ui::PlanRenameAngle(group, 0, "Master")));
  CHECK(LoadMc(project, id).angles[0].name == "Master");

  // Nudging an angle's sync by a frame moves its offset, records it as manual, and leaves the reference alone.
  CHECK(!cutline::ui::PlanNudgeSync(group, 0, frame).ok);
  CHECK(project.Apply(cutline::ui::PlanNudgeSync(group, 1, frame)));
  group = LoadMc(project, id);
  CHECK(group.sync_method == "manual" && group.angles[1].source_offset.Compare(frame) == 0 && group.angles[0].source_offset.Compare(S(0)) == 0);
  CHECK_NO_THROW(project.store().ValidateDatabase());
}

CUTLINE_TEST(AMulticamProgrammeIsLaidOnTheTimelineWithTheSoundFollowingThePictureOrTakenFromOneAngle) {
  namespace mc = cutline::timeline::multicam;
  Project project;
  for (const auto* name : {"c1", "c2"}) {
    cmd::ImportMediaPayload media;
    media.id = name;
    media.display_name = name;
    media.original_path = std::string("/") + name;
    media.fingerprint = std::string("fp-") + name;
    media.duration = S(60);
    cmd::MediaStream picture;
    picture.kind = cutline::model::StreamKind::Video;
    picture.width = 1920;
    picture.height = 1080;
    picture.frame_rate = {25, 1};
    cmd::MediaStream sound;
    sound.stream_index = 1;
    sound.kind = cutline::model::StreamKind::Audio;
    sound.sample_rate = 48000;
    sound.channel_count = 2;
    sound.channel_layout = "stereo";
    media.streams = {picture, sound};
    project.Run(cmd::CommandType::ImportMedia, media);
  }
  int serial = 0;
  const auto ids = [&](const std::string& prefix) { return prefix + "-" + std::to_string(++serial); };
  auto setup = TwoClips();
  setup.angles[0].media_id = "c1";
  setup.angles[1].media_id = "c2";
  auto made = cutline::ui::PlanCreateGroup(setup, {"A", "B"}, ids);
  CHECK(project.Apply(made.plan));
  const auto id = made.group_id;
  CHECK(project.Apply(cutline::ui::PlanCutTo(LoadMc(project, id), 1, S(10), cutline::time::kFrameRate25)));
  CHECK(project.Apply(cutline::ui::PlanCutTo(LoadMc(project, id), 0, S(20), cutline::time::kFrameRate25)));
  auto snapshot = project.Snapshot();
  auto ctx = project.Context(snapshot);
  const auto group = LoadMc(project, id);
  // Refusals: no cuts yet, wrong kinds of track, a locked track.
  mc::Group uncut = group;
  uncut.switches.clear();
  CHECK(!cutline::ui::PlanFlatten(ctx, uncut, "v1", "a1", cutline::ui::AudioSource::FollowsPicture, 0, S(0)).ok);
  CHECK(!cutline::ui::PlanFlatten(ctx, group, "a1", "a1", cutline::ui::AudioSource::FollowsPicture, 0, S(0)).ok);
  CHECK(!cutline::ui::PlanFlatten(ctx, group, "v1", "v2", cutline::ui::AudioSource::FollowsPicture, 0, S(0)).ok);
  CHECK(!cutline::ui::PlanFlatten(ctx, group, "v1", "a1", cutline::ui::AudioSource::OneAngle, 9, S(0)).ok);
  project.Add("blocker", "v2", 6, 0, 10);                                   // something already on V2 from 6 s to 16 s
  {
    const auto busy_snapshot = project.Snapshot();
    CHECK(!cutline::ui::PlanFlatten(project.Context(busy_snapshot), group, "v2", "", cutline::ui::AudioSource::FollowsPicture, 0, S(0)).ok);
  }
  // Picture on V1 and sound following it on A1, from 5 s on the timeline.
  const auto plan = cutline::ui::PlanFlatten(ctx, group, "v1", "a1", cutline::ui::AudioSource::FollowsPicture, 0, S(5));
  CHECK(plan.ok && plan.commands.size() == 1);
  CHECK(project.Apply(plan));
  snapshot = project.Snapshot();
  const auto* video = snapshot.FindTrack("v1");
  const auto* sound = snapshot.FindTrack("a1");
  CHECK(video->clips.size() == 3 && sound->clips.size() == 3);
  CHECK(video->clips[0].timeline_start.Compare(S(5)) == 0 && video->clips[1].timeline_start.Compare(S(15)) == 0 && video->clips[2].timeline_start.Compare(S(25)) == 0);
  CHECK(video->clips[1].source_id == "c2" && sound->clips[1].source_id == "c2");
  // The sound taken from one angle throughout (the second one), on another stretch of the timeline.
  snapshot = project.Snapshot();
  const auto fixed = cutline::ui::PlanFlatten(project.Context(snapshot), group, "v2", "a2", cutline::ui::AudioSource::OneAngle, 1, S(30));
  CHECK(fixed.ok);
  CHECK(project.Apply(fixed));
  snapshot = project.Snapshot();
  const auto* second_sound = snapshot.FindTrack("a2");
  CHECK(second_sound->clips.size() == 1 && second_sound->clips[0].source_id == "c2");
  CHECK(second_sound->clips[0].duration().Compare(S(60)) == 0);
  CHECK_NO_THROW(project.store().ValidateDatabase());
}

CUTLINE_TEST(TheAngleMonitorReadsEveryAnglesPictureTilesThemMarksTheLiveOneAndFindsAnAnglesEnvelope) {
  namespace mc = cutline::timeline::multicam;
  cutline::media::RegisterAllProviders();
  const auto path = [](cutline::media::SyntheticPattern pattern, float r, float g, float b, double tone, int seconds) {
    cutline::media::SyntheticSpec spec;
    spec.pattern = pattern;
    spec.width = 64;
    spec.height = 36;
    spec.frame_rate = cutline::time::kFrameRate25;
    spec.duration = S(seconds);
    spec.red = r;
    spec.green = g;
    spec.blue = b;
    spec.tone_hz = tone;
    return spec.ToPath();
  };
  const std::map<std::string, std::string> paths{
      {"red", path(cutline::media::SyntheticPattern::Solid, 1, 0, 0, 440.0, 6)},
      {"green", path(cutline::media::SyntheticPattern::Solid, 0, 1, 0, 0.0, 6)},
      {"blue", path(cutline::media::SyntheticPattern::Solid, 0, 0, 1, 0.0, 3)}};
  cutline::ui::AngleMonitor monitor([&](const std::string& id) { return paths.count(id) != 0 ? paths.at(id) : std::string(); });
  mc::Group group;
  group.id = "g";
  group.duration = S(6);
  for (const auto* name : {"red", "green", "blue", "ghost"}) {
    mc::Angle angle;
    angle.id = name;
    angle.name = name;
    angle.source_id = name;
    angle.duration = std::string(name) == "blue" ? S(3) : S(6);
    group.angles.push_back(angle);
  }
  group.angles[3].source_id = "missing";            // offline
  group.switches = {{S(0), "red"}, {S(2), "green"}};
  cutline::render::AngleMonitorConfig config;
  config.width = 320;
  config.height = 180;
  const auto rects = cutline::render::AngleMonitorLayout(4, config);
  const auto at = [&](const cutline::media::VideoFrame& frame, const cutline::render::TileRect& r) {
    const auto* row = frame.row_f32(r.y + r.height / 2);
    const int x = r.x + r.width / 2;
    return std::array<float, 3>{row[x * 4], row[x * 4 + 1], row[x * 4 + 2]};
  };
  // At 1 s red is live; blue still has picture; the offline angle shows an empty tile.
  auto frame = monitor.Render(group, S(1), config);
  CHECK(frame.width() == 320 && frame.height() == 180);
  auto red = at(frame, rects[0]), green = at(frame, rects[1]), blue = at(frame, rects[2]), ghost = at(frame, rects[3]);
  CHECK(red[0] > 0.9f && red[1] < 0.1f);
  CHECK(green[1] > 0.9f && green[0] < 0.1f);
  CHECK(blue[2] > 0.9f);
  CHECK(std::abs(ghost[0] - config.empty[0]) < 0.02f);
  // The live angle has the marker-colour border: red now, green at 3 s.
  const auto border = [&](const cutline::media::VideoFrame& f, const cutline::render::TileRect& r) { return f.row_f32(r.y + r.height / 2)[(r.x + 1) * 4]; };
  CHECK(std::abs(border(frame, rects[0]) - config.active_border[0]) < 0.02f);
  CHECK(std::abs(border(frame, rects[1]) - config.active_border[0]) > 0.3f);
  // At 4 s the blue camera (3 s long) has ended and its tile is empty; the live angle has changed.
  frame = monitor.Render(group, S(4), config);
  CHECK(std::abs(at(frame, rects[2])[2] - config.empty[2]) < 0.02f);
  CHECK(std::abs(border(frame, rects[1]) - config.active_border[0]) < 0.02f);
  // A click on a tile resolves to its angle.
  CHECK(cutline::ui::AngleMonitor::AngleAt(4, rects[2].x + 3, rects[2].y + 3, config) == 2);
  CHECK(cutline::ui::AngleMonitor::AngleAt(4, 0, 0, config) == -1);
  // The sound envelope: a tone reads as a steady level near its RMS, silence as zero, and it is a hundredth-second grid.
  auto tone = cutline::media::OpenSynthetic([&] {
    cutline::media::SyntheticSpec spec;
    spec.duration = S(4);
    spec.tone_hz = 440.0;
    spec.tone_amplitude = 0.5f;
    return spec;
  }());
  const auto envelope = cutline::ui::AudioEnvelope(*tone, S(2));
  CHECK(envelope.size() >= 195 && envelope.size() <= 205);
  CHECK(std::abs(envelope[100] - 0.5f / std::sqrt(2.0f)) < 0.03f);
  auto quiet = cutline::media::OpenSynthetic([&] {
    cutline::media::SyntheticSpec spec;
    spec.duration = S(2);
    return spec;
  }());
  const auto none = cutline::ui::AudioEnvelope(*quiet, S(2));
  CHECK(none.empty() || *std::max_element(none.begin(), none.end()) < 1e-6f);   // no sound stream: nothing to line up by
}

// -------------------------------------------------------------------- masks ----

namespace {

using cutline::ui::DragOptions;
using cutline::ui::MaskDocument;
using cutline::ui::MaskHandle;
using cutline::ui::PictureSize;
const PictureSize kHd{1920.0, 1080.0};

MaskDocument Box(double cx, double cy, double w, double h, double rotation = 0.0) {
  MaskDocument document;
  document.center_x = cx;
  document.center_y = cy;
  document.width = w;
  document.height = h;
  document.rotation = rotation;
  return document;
}

// The distance from a point to a closed polyline, in pixels.
double DistanceToOutline(const std::vector<std::pair<double, double>>& outline, double x, double y) {
  double best = 1e18;
  for (std::size_t i = 0; i < outline.size(); ++i) {
    const auto a = outline[i], b = outline[(i + 1) % outline.size()];
    const double ax = a.first * kHd.width, ay = a.second * kHd.height, bx = b.first * kHd.width, by = b.second * kHd.height;
    const double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
    const double px = x * kHd.width, py = y * kHd.height;
    const double t = l2 <= 0 ? 0 : std::clamp(((px - ax) * dx + (py - ay) * dy) / l2, 0.0, 1.0);
    best = std::min(best, std::hypot(px - (ax + dx * t), py - (ay + dy * t)));
  }
  return best;
}

}  // namespace

CUTLINE_TEST(ARectangleAndAnEllipseOfferTheirHandlesWhereTheCompositorDrawsThemEvenWhenRotated) {
  using Kind = MaskHandle::Kind;
  const auto rectangle = Box(0.5, 0.5, 0.4, 0.3);
  const auto handles = cutline::ui::Handles(rectangle, kHd);
  CHECK_EQ(handles.size(), std::size_t{5});
  const auto find = [&](const std::vector<cutline::ui::HandlePosition>& list, Kind kind) {
    for (const auto& h : list) {
      if (h.handle.kind == kind) return h;
    }
    return cutline::ui::HandlePosition{};
  };
  CHECK(std::abs(find(handles, Kind::EdgeRight).x - 0.7) < 1e-12 && std::abs(find(handles, Kind::EdgeRight).y - 0.5) < 1e-12);
  CHECK(std::abs(find(handles, Kind::EdgeTop).y - 0.35) < 1e-12);
  // The knob sits 28 pixels beyond the top edge.
  CHECK(std::abs((0.35 - find(handles, Kind::Rotate).y) * kHd.height - 28.0) < 1e-9);
  // Turned a quarter, the right edge is below the centre: the shape is rotated about its centre in pixel space.
  const auto turned = cutline::ui::Handles(Box(0.5, 0.5, 0.4, 0.3, 90.0), kHd);
  CHECK(std::abs(find(turned, Kind::EdgeRight).x - 0.5) < 1e-9 && find(turned, Kind::EdgeRight).y > 0.5);
  CHECK(std::abs((find(turned, Kind::EdgeRight).y - 0.5) * kHd.height - 0.2 * kHd.width) < 1e-6);
  // What is under the pointer: handles first, then the body, then nothing.
  CHECK(cutline::ui::HitTest(rectangle, kHd, 0.7, 0.5, 8).kind == Kind::EdgeRight);
  CHECK(cutline::ui::HitTest(rectangle, kHd, 0.69, 0.51, 8).kind == Kind::Body || cutline::ui::HitTest(rectangle, kHd, 0.69, 0.51, 8).kind == Kind::EdgeRight);
  CHECK(cutline::ui::HitTest(rectangle, kHd, 0.5, 0.5, 8).kind == Kind::Body);
  CHECK(cutline::ui::HitTest(rectangle, kHd, 0.9, 0.9, 8).kind == Kind::None);
  CHECK(cutline::ui::HitTest(rectangle, kHd, 0.5, 0.35 - 28.0 / kHd.height, 6).kind == Kind::Rotate);
  auto ellipse = rectangle;
  ellipse.shape = cutline::effects::mask::Shape::Ellipse;
  CHECK(cutline::ui::Inside(ellipse, kHd, 0.5, 0.5) && !cutline::ui::Inside(ellipse, kHd, 0.69, 0.64));   // a corner of the box is outside the ellipse
  CHECK(cutline::ui::Inside(rectangle, kHd, 0.69, 0.64));
  // The outline of a rotated rectangle has its four corners.
  const auto outline = cutline::ui::Outline(Box(0.5, 0.5, 0.2, 0.2, 45.0), kHd);
  CHECK_EQ(outline.size(), std::size_t{4});
  CHECK(DistanceToOutline(outline, 0.5, 0.5) > 50.0);
}

CUTLINE_TEST(DraggingAHandleMovesResizesAndTurnsAShapeWithoutAccumulatingErrorAndKeepsTheOppositeEdgeStill) {
  using Kind = MaskHandle::Kind;
  const auto before = Box(0.5, 0.5, 0.4, 0.3);
  // Body: the whole shape moves by what the pointer moved.
  auto moved = cutline::ui::Drag(before, {Kind::Body, 0}, 0.5, 0.5, 0.6, 0.45, kHd);
  CHECK(std::abs(moved.center_x - 0.6) < 1e-12 && std::abs(moved.center_y - 0.45) < 1e-12 && moved.width == before.width);
  // Right edge: the left edge stays; the centre follows.
  auto wider = cutline::ui::Drag(before, {Kind::EdgeRight, 0}, 0.7, 0.5, 0.8, 0.5, kHd);
  CHECK(std::abs(wider.width - 0.5) < 1e-12 && std::abs((wider.center_x - wider.width / 2) - 0.3) < 1e-12 && wider.height == before.height);
  // It cannot be dragged past the other edge: it stops at a couple of pixels wide.
  auto crossed = cutline::ui::Drag(before, {Kind::EdgeRight, 0}, 0.7, 0.5, 0.1, 0.5, kHd);
  CHECK(crossed.width > 0.0 && crossed.width * kHd.width < 3.0 && std::abs((crossed.center_x - crossed.width / 2) - 0.3) < 1e-9);
  // The same on a shape turned 30 degrees: the edge opposite the one dragged does not move on the picture.
  const auto turned = Box(0.4, 0.55, 0.3, 0.2, 30.0);
  const auto handles = cutline::ui::Handles(turned, kHd);
  const auto edge = [&](const std::vector<cutline::ui::HandlePosition>& list, Kind kind) {
    for (const auto& h : list) {
      if (h.handle.kind == kind) return std::pair{h.x, h.y};
    }
    return std::pair{0.0, 0.0};
  };
  const auto left_before = edge(handles, Kind::EdgeLeft);
  const auto right = edge(handles, Kind::EdgeRight);
  // Drag the right edge 80 pixels further along the shape's own axis.
  const double along_x = std::cos(30.0 * 3.14159265358979 / 180.0), along_y = std::sin(30.0 * 3.14159265358979 / 180.0);
  auto stretched = cutline::ui::Drag(turned, {Kind::EdgeRight, 0}, right.first, right.second, right.first + 80.0 * along_x / kHd.width, right.second + 80.0 * along_y / kHd.height, kHd);
  const auto left_after = edge(cutline::ui::Handles(stretched, kHd), Kind::EdgeLeft);
  CHECK(std::abs((left_after.first - left_before.first) * kHd.width) < 1e-6 && std::abs((left_after.second - left_before.second) * kHd.height) < 1e-6);
  CHECK(std::abs(stretched.width * kHd.width - (turned.width * kHd.width + 80.0)) < 1e-6 && stretched.rotation == 30.0);
  // Constraining keeps the proportions.
  DragOptions square;
  square.constrain = true;
  auto kept = cutline::ui::Drag(before, {Kind::EdgeRight, 0}, 0.7, 0.5, 0.9, 0.5, kHd, square);
  CHECK(std::abs(kept.height / kept.width - before.height / before.width) < 1e-9);
  // Rotate: pointing straight right of the centre is a quarter turn; constrained it lands on a multiple of 15.
  auto quarter = cutline::ui::Drag(before, {Kind::Rotate, 0}, 0.5, 0.2, 0.9, 0.5, kHd);
  CHECK(std::abs(quarter.rotation - 90.0) < 1e-9);
  auto snapped = cutline::ui::Drag(before, {Kind::Rotate, 0}, 0.5, 0.2, 0.9, 0.38, kHd, square);
  CHECK(std::abs(std::fmod(snapped.rotation, 15.0)) < 1e-9);
  // Computed from the press, so nothing accumulates: the same end gives the same result however it was reached.
  const auto once = cutline::ui::Drag(before, {Kind::Body, 0}, 0.5, 0.5, 0.7, 0.7, kHd);
  const auto again = cutline::ui::Drag(before, {Kind::Body, 0}, 0.5, 0.5, 0.7, 0.7, kHd);
  CHECK(once.center_x == again.center_x && before.center_x == 0.5);
}

CUTLINE_TEST(APathIsBuiltFromPointsSmoothedCurvedWithTangentsAndGainsAndLosesVerticesWithoutChangingItsShape) {
  using Kind = MaskHandle::Kind;
  auto path = cutline::ui::NewPath({{0.3, 0.3}, {0.7, 0.3}, {0.5, 0.7}});
  CHECK(path.shape == cutline::effects::mask::Shape::Bezier && path.points.size() == 3 && !cutline::ui::IsSmooth(path, 0));
  CHECK(cutline::ui::Inside(path, kHd, 0.5, 0.4) && !cutline::ui::Inside(path, kHd, 0.2, 0.2));
  // Every vertex is a handle; a vertex with no tangent shows none.
  CHECK_EQ(cutline::ui::Handles(path, kHd, 1).size(), std::size_t{3});
  CHECK(cutline::ui::HitTest(path, kHd, 0.7, 0.3, 6).kind == Kind::Point && cutline::ui::HitTest(path, kHd, 0.7, 0.3, 6).index == 1);
  CHECK(cutline::ui::HitTest(path, kHd, 0.5, 0.3, 6).kind == Kind::Segment && cutline::ui::HitTest(path, kHd, 0.5, 0.3, 6).index == 0);
  CHECK(cutline::ui::HitTest(path, kHd, 0.5, 0.45, 6).kind == Kind::Body);
  // Smoothing a vertex gives it opposite tangents a sixth of the way across its neighbours.
  const auto smooth = cutline::ui::Smooth(path, 1);
  CHECK(cutline::ui::IsSmooth(smooth, 1));
  CHECK(std::abs(smooth.points[1].out_x - (0.7 + (0.5 - 0.3) / 6.0)) < 1e-12 && std::abs(smooth.points[1].in_x - (0.7 - (0.5 - 0.3) / 6.0)) < 1e-12);
  CHECK(cutline::ui::Handles(smooth, kHd, 1).size() == 5);   // three vertices and its two tangents
  CHECK(!cutline::ui::IsSmooth(cutline::ui::Corner(smooth, 1), 1));
  // Dragging one tangent of a smooth vertex swings the other opposite, keeping its length; breaking leaves it.
  const auto& s = smooth.points[1];
  const double partner_length = std::hypot((s.in_x - s.x) * kHd.width, (s.in_y - s.y) * kHd.height);
  auto swung = cutline::ui::Drag(smooth, {Kind::TangentOut, 1}, s.out_x, s.out_y, 0.8, 0.5, kHd);
  CHECK(cutline::ui::IsSmooth(swung, 1));
  CHECK(std::abs(std::hypot((swung.points[1].in_x - s.x) * kHd.width, (swung.points[1].in_y - s.y) * kHd.height) - partner_length) < 1e-6);
  DragOptions broken;
  broken.break_tangent = true;
  auto cornered = cutline::ui::Drag(smooth, {Kind::TangentOut, 1}, s.out_x, s.out_y, 0.8, 0.3, kHd, broken);   // off the line of the other tangent
  CHECK(cornered.points[1].in_x == s.in_x);
  CHECK(cornered.points[1].out_x == 0.8);
  CHECK(!cutline::ui::IsSmooth(cornered, 1));
  // A vertex takes its tangents with it.
  auto shifted = cutline::ui::Drag(smooth, {Kind::Point, 1}, 0.7, 0.3, 0.75, 0.35, kHd);
  CHECK(std::abs(shifted.points[1].x - 0.75) < 1e-12 && std::abs(shifted.points[1].out_x - (s.out_x + 0.05)) < 1e-12);
  // A vertex added on a side leaves the curve exactly where it was; a click away from every side adds nothing.
  auto curved = cutline::ui::Smooth(cutline::ui::Smooth(cutline::ui::Smooth(path, 0), 1), 2);
  const auto before_outline = cutline::ui::Outline(curved, kHd, 64);
  const auto grown = cutline::ui::InsertPoint(curved, kHd, before_outline[40].first, before_outline[40].second, 5.0);
  CHECK_EQ(grown.points.size(), std::size_t{4});
  const auto after_outline = cutline::ui::Outline(grown, kHd, 64);
  double worst = 0;
  for (const auto& p : before_outline) worst = std::max(worst, DistanceToOutline(after_outline, p.first, p.second));
  CHECK(worst < 0.2);   // pixels: the curve did not move (the two polylines differ by their own chord error)
  CHECK_EQ(cutline::ui::InsertPoint(curved, kHd, 0.05, 0.05, 5.0).points.size(), std::size_t{3});
  // Removing: down to three vertices and no further.
  CHECK_EQ(cutline::ui::RemovePoint(grown, 1).points.size(), std::size_t{3});
  CHECK_EQ(cutline::ui::RemovePoint(path, 1).points.size(), std::size_t{3});
  // Beginning a shape from two corners, whichever way it is dragged.
  const auto drawn = cutline::ui::NewRectangle(0.8, 0.7, 0.2, 0.3, kHd);
  CHECK(std::abs(drawn.center_x - 0.5) < 1e-12 && std::abs(drawn.width - 0.6) < 1e-12 && std::abs(drawn.height - 0.4) < 1e-12);
  CHECK(cutline::ui::NewEllipse(0.5, 0.5, 0.5, 0.5, kHd).width * kHd.width >= 2.0);   // a click without a drag still makes something
}

CUTLINE_TEST(AValueThatIsKeyedGetsAKeyAtTheCurrentTimeAndOneThatIsNotChangesForTheWholeClip) {
  using Kind = MaskHandle::Kind;
  MaskDocument stored = Box(0.5, 0.5, 0.4, 0.3);
  stored = cutline::ui::StartAnimating(stored, "center_x", 0.0);
  CHECK(cutline::ui::IsAnimated(stored, "center_x") && !cutline::ui::IsAnimated(stored, "center_y"));
  CHECK_EQ(cutline::ui::StartAnimating(stored, "center_x", 3.0).animations.size(), std::size_t{1});   // already animated: unchanged
  // Drag the whole shape at 2 s: x gets a key there, y (not keyed) changes everywhere.
  const auto evaluated = cutline::effects::mask::Evaluate(stored, 2.0);
  const auto dragged = cutline::ui::Drag(evaluated, {Kind::Body, 0}, 0.5, 0.5, 0.6, 0.4, kHd);
  const auto updated = cutline::ui::ApplyEdit(stored, evaluated, dragged, 2.0);
  CHECK_EQ(updated.animations.size(), std::size_t{1});
  CHECK_EQ(updated.animations[0].keys.size(), std::size_t{2});
  CHECK(updated.animations[0].keys[1].time == 2.0 && std::abs(updated.animations[0].keys[1].value - 0.6) < 1e-12);
  CHECK(std::abs(updated.center_y - 0.4) < 1e-12);
  CHECK(std::abs(cutline::effects::mask::Evaluate(updated, 1.0).center_x - 0.55) < 1e-12);   // halfway between the keys
  CHECK(std::abs(cutline::effects::mask::Evaluate(updated, 0.0).center_y - 0.4) < 1e-12);
  // Editing at a time that already has a key replaces it; an edit that changes nothing changes nothing.
  const auto replaced = cutline::ui::ApplyEdit(updated, cutline::effects::mask::Evaluate(updated, 2.0), cutline::ui::Drag(cutline::effects::mask::Evaluate(updated, 2.0), {Kind::Body, 0}, 0.6, 0.4, 0.7, 0.4, kHd), 2.0);
  CHECK_EQ(replaced.animations[0].keys.size(), std::size_t{2});
  CHECK(std::abs(replaced.animations[0].keys[1].value - 0.7) < 1e-12);
  const auto same = cutline::ui::ApplyEdit(stored, evaluated, evaluated, 2.0);
  CHECK(same.center_x == stored.center_x && same.animations.size() == 1 && same.animations[0].keys.size() == 1);
  // Opacity and the like are clamped to what the document allows; keys cannot be before the start.
  auto loud = stored;
  loud.opacity = 5.0;
  CHECK(cutline::ui::ApplyEdit(stored, stored, loud, 0.0).opacity == 1.0);
  // Stopping animation keeps the value it has at this moment; removing a key removes the animation with the last one.
  const auto stopped = cutline::ui::StopAnimating(updated, "center_x", 1.0);
  CHECK(!cutline::ui::IsAnimated(stopped, "center_x") && std::abs(stopped.center_x - 0.55) < 1e-12);
  CHECK(!cutline::ui::IsAnimated(cutline::ui::RemoveKey(stored, "center_x", 0.0), "center_x"));
  CHECK_EQ(cutline::ui::RemoveKey(updated, "center_x", 2.0).animations[0].keys.size(), std::size_t{1});
  // The names a document is keyed by cover every value: eight shape values and six per vertex.
  CHECK_EQ(cutline::ui::PropertyNames(cutline::ui::NewPath({{0.2, 0.2}, {0.8, 0.2}, {0.5, 0.8}})).size(), std::size_t{8 + 18});
  CHECK(cutline::ui::ValueOf(stored, "width") == 0.4 && cutline::ui::ValueOf(cutline::ui::NewPath({{0.2, 0.2}, {0.8, 0.2}, {0.5, 0.8}}), "point_1_x") == 0.8);
  // A path's vertex animation is dropped when the number of vertices changes: the old keys would be on the wrong points.
  auto path = cutline::ui::NewPath({{0.3, 0.3}, {0.7, 0.3}, {0.5, 0.7}, {0.4, 0.5}});
  path = cutline::ui::StartAnimating(path, "point_1_x", 0.0);
  CHECK(cutline::ui::RemovePoint(path, 3).animations.empty());
}

CUTLINE_TEST(AMaskFollowsATrackByTheDistanceItMovedSinceTheMaskWasDrawnAndSkipsWhatWasLost) {
  cutline::render::tracking::TrackResult track;
  const auto sample = [](std::int64_t frame, double x, double y, bool valid) {
    cutline::render::tracking::PointSample s;
    s.time = RationalTime(frame, 25);
    s.x = x;
    s.y = y;
    s.confidence = valid ? 0.95 : 0.1;
    s.valid = valid;
    return s;
  };
  // 100 px right and 36 px down over a second, with one frame lost on the way.
  for (int frame = 0; frame <= 25; ++frame) track.samples.push_back(sample(frame, 960.0 + 4.0 * frame, 540.0 + 1.44 * frame, frame != 10));
  const auto mask = Box(0.5, 0.5, 0.3, 0.3);
  const auto following = cutline::ui::FollowTrack(mask, track, 0.0, kHd);
  CHECK(cutline::ui::IsAnimated(following, "center_x") && cutline::ui::IsAnimated(following, "center_y") && !cutline::ui::IsAnimated(following, "width"));
  const auto end = cutline::effects::mask::Evaluate(following, 1.0);
  CHECK(std::abs(end.center_x - (0.5 + 100.0 / 1920.0)) < 1e-9 && std::abs(end.center_y - (0.5 + 36.0 / 1080.0)) < 1e-9);
  CHECK(std::abs(cutline::effects::mask::Evaluate(following, 0.5).center_x - (0.5 + 50.0 / 1920.0)) < 1e-9);
  CHECK_EQ(following.animations[0].keys.size(), std::size_t{25});   // 26 samples, one lost
  // A mask drawn halfway through follows the track from where it was then: it does not jump back to the start.
  const auto later = cutline::ui::FollowTrack(Box(0.6, 0.5, 0.2, 0.2), track, 0.48, kHd);
  CHECK(std::abs(cutline::effects::mask::Evaluate(later, 0.48).center_x - 0.6) < 1e-9);
  CHECK(std::abs(cutline::effects::mask::Evaluate(later, 1.0).center_x - (0.6 + 52.0 / 1920.0)) < 1e-9);
  // A path moves every vertex and handle.
  auto path = cutline::ui::Smooth(cutline::ui::NewPath({{0.3, 0.3}, {0.7, 0.3}, {0.5, 0.7}}), 0);
  const auto path_following = cutline::ui::FollowTrack(path, track, 0.0, kHd);
  CHECK_EQ(path_following.animations.size(), std::size_t{18});
  const auto path_end = cutline::effects::mask::Evaluate(path_following, 1.0);
  CHECK(std::abs(path_end.points[2].x - (0.5 + 100.0 / 1920.0)) < 1e-9 && std::abs(path_end.points[0].out_y - (path.points[0].out_y + 36.0 / 1080.0)) < 1e-9);
  // Too little track: the mask is left alone.
  cutline::render::tracking::TrackResult lost;
  lost.samples.push_back(sample(0, 960, 540, true));
  lost.samples.push_back(sample(1, 961, 540, false));
  CHECK(cutline::ui::FollowTrack(mask, lost, 0.0, kHd).animations.empty());
}

CUTLINE_TEST(AddingEditingAndRemovingAMaskIsOneUndoStepEachAndRefusesWhatCannotBeDone) {
  Project project;
  project.Add("clip", "v1", 0, 0, 10);
  auto snapshot = project.Snapshot();
  CHECK(project.Apply(cutline::ui::PlanAddEffect(project.Context(snapshot), "clip", "grade", "")));
  snapshot = project.Snapshot();
  const auto* clip = Find(snapshot, "clip");
  const std::string effect_id = clip->effects.back().id;
  const auto steps = [&] { return project.store().HistorySteps().size(); };
  const auto before = steps();

  const auto drawn = cutline::ui::NewEllipse(0.3, 0.3, 0.7, 0.6, kHd);
  CHECK(project.Apply(cutline::ui::PlanAddMask(project.Context(snapshot), "clip", effect_id, drawn)));
  CHECK_EQ(steps(), before + 1);
  snapshot = project.Snapshot();
  const auto masks = Find(snapshot, "clip")->effects.back().masks;   // a copy: the snapshot is replaced below
  CHECK_EQ(masks.size(), std::size_t{1});
  CHECK(masks[0].document.shape == cutline::effects::mask::Shape::Ellipse && std::abs(masks[0].document.width - 0.4) < 1e-12);
  const std::string mask_id = masks[0].id;
  // A second mask goes after the first.
  CHECK(project.Apply(cutline::ui::PlanAddMask(project.Context(snapshot), "clip", effect_id, cutline::ui::NewRectangle(0.1, 0.1, 0.2, 0.2, kHd))));
  snapshot = project.Snapshot();
  CHECK_EQ(Find(snapshot, "clip")->effects.back().masks.size(), std::size_t{2});
  CHECK(Find(snapshot, "clip")->effects.back().masks[1].order > Find(snapshot, "clip")->effects.back().masks[0].order);
  // Edit: a different feather and the combine mode.
  auto edited = masks[0].document;
  edited.feather = 12.0;
  edited.combine = cutline::effects::mask::Combine::Subtract;
  CHECK(project.Apply(cutline::ui::PlanUpdateMask(project.Context(snapshot), "clip", mask_id, edited)));
  snapshot = project.Snapshot();
  CHECK(Find(snapshot, "clip")->effects.back().masks[0].document.feather == 12.0 && Find(snapshot, "clip")->effects.back().masks[0].document.combine == cutline::effects::mask::Combine::Subtract);
  (void)project.store().Undo("tester", "2026-10-07T00:00:00Z");
  CHECK(Find(project.Snapshot(), "clip")->effects.back().masks[0].document.feather == 0.0);
  CHECK(project.Apply(cutline::ui::PlanRemoveMask(project.Context(project.Snapshot()), "clip", mask_id)));
  CHECK_EQ(Find(project.Snapshot(), "clip")->effects.back().masks.size(), std::size_t{1});
  // Refused: no such clip, no such effect or mask, a locked track, a document the format rejects.
  snapshot = project.Snapshot();
  const auto ctx = project.Context(snapshot);
  CHECK(!cutline::ui::PlanAddMask(ctx, "nothing", effect_id, drawn).ok);
  CHECK(!cutline::ui::PlanAddMask(ctx, "clip", "no-effect", drawn).ok);
  CHECK(!cutline::ui::PlanUpdateMask(ctx, "clip", "no-mask", drawn).ok);
  auto bad = drawn;
  bad.opacity = 3.0;
  CHECK(!cutline::ui::PlanAddMask(ctx, "clip", effect_id, bad).ok);
  project.Run(cmd::CommandType::SetTrackState, cmd::SetTrackStatePayload{"v1", true, false, false, 0.0, 0.0, "V1"});
  snapshot = project.Snapshot();
  CHECK(!cutline::ui::PlanAddMask(project.Context(snapshot), "clip", effect_id, drawn).ok);
  CHECK_NO_THROW(project.store().ValidateDatabase());
}

// ------------------------------------------------------------------- text-based editing ----

namespace {

// Words said in m1 (seconds in the media): one before the clip, two across its start, the rest inside, the last across its end.
cutline::speech::Transcript SaidInM1() {
  cutline::speech::Transcript transcript;
  transcript.media_id = "m1";
  transcript.language = "en";
  const struct { const char* text; double start, end; } words[] = {{"one", 4.0, 4.5}, {"two", 4.8, 5.4}, {"three", 6.0, 6.5}, {"um", 7.0, 7.4},
                                                                    {"four", 7.6, 8.0}, {"five", 9.0, 9.5}, {"six", 9.6, 10.0}, {"seven", 14.8, 15.4}};
  for (const auto& w : words) transcript.words.push_back({w.text, w.start, w.end});
  return transcript;
}

std::vector<std::string> Said(const cutline::speech::Transcript& transcript, const cutline::ui::WordMapping& mapping) {
  std::vector<std::string> out;
  for (const auto& word : mapping.words) out.push_back(transcript.words[word.word].text);
  return out;
}

// The clips of a track in order, as "start:in-out" in seconds for a readable comparison.
std::string ClipsOf(const Sequence& sequence, const std::string& track) {
  std::vector<const cutline::timeline::Clip*> clips;
  for (const auto& clip : sequence.FindTrack(track)->clips) clips.push_back(&clip);
  std::sort(clips.begin(), clips.end(), [](const auto* a, const auto* b) { return a->timeline_start.Compare(b->timeline_start) < 0; });
  const auto seconds = [](const RationalTime& t) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(t.numerator()) / static_cast<double>(t.denominator()));
    return std::string(buffer);
  };
  std::string text;
  for (const auto* clip : clips) text += (text.empty() ? "" : " ") + seconds(clip->timeline_start) + ":" + seconds(clip->source_in) + "-" + seconds(clip->source_out);
  return text;
}

}  // namespace

CUTLINE_TEST(WordsAreMappedOntoTrimmedAndRepeatedClipsAtTheirTimelineTimes) {
  Project project;
  project.Add("a", "a1", 10, 5, 15, "g");
  project.Add("v", "v1", 10, 5, 15, "g");
  const auto transcript = SaidInM1();
  auto snap = project.Snapshot();
  auto mapping = cutline::ui::MapWords(snap, transcript, "m1");
  // The clip shows 5 to 15: "one" is before it and is not on the timeline; "two" and "seven" are cut through by its edges.
  CHECK(Said(transcript, mapping) == (std::vector<std::string>{"two", "three", "um", "four", "five", "six", "seven"}));
  CHECK(mapping.words[0].partial && mapping.words[6].partial && !mapping.words[1].partial);
  CHECK(mapping.words[0].start.Compare(S(10)) == 0);                                 // the part of "two" that is in the clip starts at the clip
  CHECK(mapping.words[1].start.Compare(S(11)) == 0 && mapping.words[1].end.Compare(RationalTime(288, 25)) == 0);   // three: 6.0-6.5 in the media
  CHECK(mapping.words[1].clip_id == "a");                                              // the sound carries the words, not the picture
  CHECK(mapping.words[6].end.Compare(S(20)) == 0);
  CHECK(mapping.notes.empty());
  CHECK(!cutline::ui::WordAt(mapping, RationalTime(1, 2)).has_value());
  CHECK_EQ(*cutline::ui::WordAt(mapping, RationalTime(23, 2).Add(RationalTime(1, 4))), std::size_t{1});   // in the gap after "three": the word just said

  // The same media used again, shorter and later: its words are heard twice; a video-only use of other media does not matter.
  project.Add("a2", "a1", 30, 6, 8);
  snap = project.Snapshot();
  mapping = cutline::ui::MapWords(snap, transcript, "m1");
  CHECK_EQ(mapping.words.size(), std::size_t{10});
  CHECK(mapping.words[7].clip_id == "a2" && transcript.words[mapping.words[7].word].text == "three" && mapping.words[7].start.Compare(S(30)) == 0);

  // Only a video clip: the picture carries the words. A reversed clip and a ramped one are left out, and say so.
  Project silent;
  silent.Add("v", "v1", 0, 5, 15);
  const auto silent_snap = silent.Snapshot();
  CHECK_EQ(cutline::ui::MapWords(silent_snap, transcript, "m1").words.size(), std::size_t{7});
  CHECK(cutline::ui::MapWords(silent_snap, transcript, "other").words.empty());
  Project reversed;
  reversed.Add("a", "a1", 0, 5, 15);
  reversed.Run(cmd::CommandType::SetClipSpeed, cmd::SetClipSpeedPayload{"a", RationalTime(1, 1), true, false});
  const auto reversed_snap = reversed.Snapshot();
  const auto none = cutline::ui::MapWords(reversed_snap, transcript, "m1");
  CHECK(none.words.empty() && none.notes.size() == 1);
}

CUTLINE_TEST(DeletingWordsRemovesTheirTimeFromEveryTrackAsOneUndoStepAndTheRestStaysInSync) {
  Project project;
  project.Add("a", "a1", 10, 5, 15, "g");
  project.Add("v", "v1", 10, 5, 15, "g");
  const auto transcript = SaidInM1();
  auto snap = project.Snapshot();
  const auto mapping = cutline::ui::MapWords(snap, transcript, "m1");
  const auto steps = project.store().AppliedStepCount();
  // "um" and, as one run, "five six": two separate removals in one clip, in one step.
  const auto three = cutline::ui::PlanDeleteWords(project.Context(snap), mapping, {2, 4, 5}, {});
  CHECK(three.ok);
  CHECK(project.Apply(three));
  CHECK_EQ(project.store().AppliedStepCount(), steps + 1);
  snap = project.Snapshot();
  // The media's 5-7, 7.4-9, 10-15 remain, closed up end to end, on the sound and on the picture.
  CHECK(ClipsOf(snap, "a1") == "10.00:5.00-7.00 12.00:7.40-9.00 13.60:10.00-15.00");
  CHECK(ClipsOf(snap, "v1") == ClipsOf(snap, "a1"));
  // What is heard now: "um" and "five six" are gone and what follows came forward by the time they took.
  const auto after = cutline::ui::MapWords(snap, transcript, "m1");
  CHECK(Said(transcript, after) == (std::vector<std::string>{"two", "three", "four", "seven"}));
  CHECK(after.words[2].start.Compare(RationalTime(61, 5)) == 0);   // four: 12.2
  // One undo gives everything back.
  (void)project.store().Undo("tester", "2026-10-07T00:00:00Z");
  CHECK(ClipsOf(project.Snapshot(), "a1") == "10.00:5.00-15.00" && ClipsOf(project.Snapshot(), "v1") == "10.00:5.00-15.00");

  // Lift instead: the gaps stay, later words keep their places.
  snap = project.Snapshot();
  cutline::ui::DeleteWordsOptions lift;
  lift.ripple = false;
  CHECK(project.Apply(cutline::ui::PlanDeleteWords(project.Context(snap), mapping, {2, 4, 5}, lift)));
  CHECK(ClipsOf(project.Snapshot(), "a1") == "10.00:5.00-7.00 12.40:7.40-9.00 15.00:10.00-15.00");
  (void)project.store().Undo("tester", "2026-10-07T00:00:00Z");

  // Time left on at each side of a removal; never more than the removal itself has.
  snap = project.Snapshot();
  cutline::ui::DeleteWordsOptions keep;
  keep.keep_seconds = 0.08;
  CHECK(project.Apply(cutline::ui::PlanDeleteWords(project.Context(snap), mapping, {2}, keep)));
  CHECK(ClipsOf(project.Snapshot(), "a1") == "10.00:5.00-7.08 12.08:7.32-15.00");
  (void)project.store().Undo("tester", "2026-10-07T00:00:00Z");
  keep.keep_seconds = 5.0;
  snap = project.Snapshot();
  CHECK(project.Apply(cutline::ui::PlanDeleteWords(project.Context(snap), mapping, {2}, keep)));
  CHECK(ClipsOf(project.Snapshot(), "a1") == "10.00:5.00-7.00 12.00:7.40-15.00");   // too much to keep: the word is cut whole
  (void)project.store().Undo("tester", "2026-10-07T00:00:00Z");

  // Refusals: no words, positions that are not words.
  snap = project.Snapshot();
  CHECK(!cutline::ui::PlanDeleteWords(project.Context(snap), mapping, {}, {}).ok);
  CHECK(!cutline::ui::PlanDeleteWords(project.Context(snap), mapping, {99}, {}).ok);
  CHECK_NO_THROW(project.store().ValidateDatabase());
}

CUTLINE_TEST(RemovingSeveralRangesPlansEachAgainstTheClipsTheEarlierOnesLeave) {
  Project project;
  project.Add("a", "a1", 0, 0, 30);
  auto snap = project.Snapshot();
  // Unordered, overlapping and touching ranges are merged; one past the end of the clips is not an error.
  std::vector<cutline::ui::TimeRange> ranges = {{S(20), S(22)}, {S(5), S(7)}, {S(6), S(8)}, {S(8), S(9)}, {S(40), S(41)}};
  const auto plan = cutline::ui::PlanRemoveRanges(project.Context(snap), ranges, true, "Remove");
  CHECK(plan.ok);
  CHECK(project.Apply(plan));
  // 5 to 9 and 20 to 22 are gone: 30 - 4 - 2 seconds remain, in three pieces that touch.
  CHECK(ClipsOf(project.Snapshot(), "a1") == "0.00:0.00-5.00 5.00:9.00-20.00 16.00:22.00-30.00");
  (void)project.store().Undo("tester", "2026-10-07T00:00:00Z");
  CHECK(ClipsOf(project.Snapshot(), "a1") == "0.00:0.00-30.00");
  snap = project.Snapshot();
  CHECK(!cutline::ui::PlanRemoveRanges(project.Context(snap), {{S(50), S(60)}}, true, "Remove").ok);
  CHECK(!cutline::ui::PlanRemoveRanges(project.Context(snap), {}, true, "Remove").ok);
  CHECK(!cutline::ui::PlanRemoveRanges(project.Context(snap), {{S(5), S(5)}}, false, "Remove").ok);
  // Lifted, the same ranges leave their gaps.
  CHECK(project.Apply(cutline::ui::PlanRemoveRanges(project.Context(snap), ranges, false, "Lift")));
  CHECK(ClipsOf(project.Snapshot(), "a1") == "0.00:0.00-5.00 9.00:9.00-20.00 22.00:22.00-30.00");
  // Many ranges in one clip are each planned from the clip as it is left: fifty cuts, one step, and the total is right.
  Project many;
  many.Add("a", "a1", 0, 0, 60);
  snap = many.Snapshot();
  std::vector<cutline::ui::TimeRange> cuts;
  for (int i = 0; i < 50; ++i) cuts.push_back({S(i + 5), RationalTime(2 * (i + 5) + 1, 2)});   // each half a second at i + 5
  const auto steps = many.store().AppliedStepCount();
  const auto many_plan = cutline::ui::PlanRemoveRanges(many.Context(snap), cuts, true, "Remove");
  CHECK(many_plan.ok);
  CHECK(many.Apply(many_plan) || (std::cerr << "ERR " << many.last_error << std::endl, false));
  CHECK_EQ(many.store().AppliedStepCount(), steps + 1);
  snap = many.Snapshot();
  CHECK_EQ(snap.FindTrack("a1")->clips.size(), std::size_t{51});
  CHECK(snap.Duration().Compare(S(35)) == 0);
  CHECK_NO_THROW(many.store().ValidateDatabase());

  // A long recording with hundreds of cuts: the work planned grows with the cuts, not with the cuts times the clips.
  Project long_take;
  long_take.Add("a", "a1", 0, 0, 60);
  long_take.Add("v", "v1", 0, 0, 60);
  snap = long_take.Snapshot();
  std::vector<cutline::ui::TimeRange> many_cuts;
  for (int i = 0; i < 250; ++i) many_cuts.push_back({RationalTime(i * 5, 25), RationalTime(i * 5 + 3, 25)});   // 3 frames out of every 5, from the start
  const auto started = std::chrono::steady_clock::now();
  const auto big = cutline::ui::PlanRemoveRanges(long_take.Context(snap), many_cuts, true, "Remove");
  const auto planning = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  CHECK(big.ok);
  CHECK(planning < 1.0);
  CHECK(big.commands.size() <= std::size_t{2 * 4 * 250 + 8});   // two tracks, a split, a trim and a move for each cut
  CHECK(long_take.Apply(big));
  snap = long_take.Snapshot();
  CHECK_EQ(snap.FindTrack("a1")->clips.size(), std::size_t{250});
  CHECK(snap.Duration().Compare(S(60).Subtract(RationalTime(750, 25))) == 0);
  CHECK(ClipsOf(snap, "a1") == ClipsOf(snap, "v1"));
  CHECK_NO_THROW(long_take.store().ValidateDatabase());
}

CUTLINE_TEST(PausesBetweenWordsAreRemovedByLengthAndFillersAreFoundAsWordPositions) {
  Project project;
  project.Add("a", "a1", 10, 5, 15, "g");
  project.Add("v", "v1", 10, 5, 15, "g");
  auto transcript = SaidInM1();
  auto snap = project.Snapshot();
  const auto mapping = cutline::ui::MapWords(snap, transcript, "m1");
  const auto frame = RationalTime(1, 25);
  // Pauses of 0.8 s or more: between "four" and "five" (1.0 s) and between "six" and "seven" (4.8 s); 0.2 s kept at each side.
  const auto ranges = cutline::ui::PauseRanges(mapping, frame, 0.8, 0.2);
  CHECK_EQ(ranges.size(), std::size_t{2});
  CHECK(ranges[0].in.Compare(RationalTime(66, 5)) == 0 && ranges[0].out.Compare(RationalTime(69, 5)) == 0);   // 13.2 to 13.8
  CHECK_EQ(cutline::ui::PauseRanges(mapping, frame, 0.45, 0.0).size(), std::size_t{4});
  CHECK(cutline::ui::PauseRanges(mapping, frame, 30.0, 0.0).empty());
  CHECK(cutline::ui::PauseRanges(mapping, frame, 0.8, 0.6).size() == 1);   // keeping 0.6 each side leaves nothing of a one-second pause
  const auto steps = project.store().AppliedStepCount();
  const auto plan = cutline::ui::PlanRemovePauses(project.Context(snap), mapping, 0.8, 0.2);
  CHECK(plan.ok && plan.label == "Remove Pauses");
  CHECK(project.Apply(plan));
  CHECK_EQ(project.store().AppliedStepCount(), steps + 1);
  CHECK(ClipsOf(project.Snapshot(), "a1") == "10.00:5.00-8.20 13.20:8.80-10.20 14.60:14.60-15.00");
  CHECK(project.Snapshot().Duration().Compare(S(15)) == 0);   // 10 seconds of clip less 0.6 and 4.4
  CHECK(!cutline::ui::PlanRemovePauses(project.Context(project.Snapshot()), cutline::ui::MapWords(project.Snapshot(), transcript, "m1"), 30.0, 0.0).ok);

  // Fillers, as found in the words, are positions in the mapping.
  const auto fillers = cutline::speech::FindFillers(transcript);
  CHECK_EQ(fillers.size(), std::size_t{1});
  (void)project.store().Undo("tester", "2026-10-07T00:00:00Z");
  const auto remapped = cutline::ui::MapWords(project.Snapshot(), transcript, "m1");
  const auto positions = cutline::ui::MappedIndices(remapped, fillers);
  CHECK_EQ(positions.size(), std::size_t{1});
  CHECK(transcript.words[remapped.words[positions[0]].word].text == "um");
}

CUTLINE_TEST(CaptionsAreMadeFromTheWordsOnTheTimelineAtTheirTimelineTimesInOneStep) {
  Project project;
  project.Add("a", "a1", 10, 5, 15, "g");
  project.Add("v", "v1", 10, 5, 15, "g");
  auto transcript = SaidInM1();
  transcript.speakers = {"Ana"};
  transcript.words[5].speaker = 0;   // "five", which starts the second cue
  auto snap = project.Snapshot();
  const auto mapping = cutline::ui::MapWords(snap, transcript, "m1");
  cutline::ui::CaptionPlanOptions options;
  const auto steps = project.store().AppliedStepCount();
  const auto plan = cutline::ui::PlanCaptions(project.Context(snap), mapping, transcript, options);
  CHECK(plan.ok);
  CHECK(project.Apply(plan));
  CHECK_EQ(project.store().AppliedStepCount(), steps + 1);
  snap = project.Snapshot();
  CHECK_EQ(snap.caption_tracks.size(), std::size_t{1});
  const auto& track = snap.caption_tracks[0];
  CHECK(track.language == "en" && track.name == "Transcript");
  // Pauses of a second or more split the speech into three cues: "two three um four", "five six", "seven".
  CHECK_EQ(track.cues.size(), std::size_t{3});
  CHECK(track.cues[0].text == "two three um four" && track.cues[0].start.Compare(S(10)) == 0 && track.cues[0].end.Compare(S(13)) == 0);
  CHECK(track.cues[1].text == "five six" && track.cues[1].start.Compare(S(14)) == 0);
  CHECK(track.cues[2].text == "seven");
  CHECK(track.cues[0].speaker.empty() && track.cues[1].speaker == "Ana");   // a cue takes its speaker from its first word
  for (std::size_t i = 1; i < track.cues.size(); ++i) CHECK(track.cues[i].start.Compare(track.cues[i - 1].end) >= 0);
  // Into the track that is there: no second track.
  options.track_id = track.id;
  snap = project.Snapshot();
  CHECK(project.Apply(cutline::ui::PlanCaptions(project.Context(snap), mapping, transcript, options)));
  CHECK_EQ(project.Snapshot().caption_tracks.size(), std::size_t{1});
  CHECK_EQ(project.Snapshot().caption_tracks[0].cues.size(), std::size_t{6});
  // Nothing on the timeline, nothing to caption.
  Project empty;
  const auto empty_snap = empty.Snapshot();
  CHECK(!cutline::ui::PlanCaptions(empty.Context(empty_snap), cutline::ui::MapWords(empty_snap, transcript, "m1"), transcript, {}).ok);
}

// ----------------------------------------------------------------------- the look pack ----

CUTLINE_TEST(TheLookPackIsWrittenOnceReadsAsCubesAndEveryLookIsASaneMonotoneGrade) {
  const auto directory = std::filesystem::temp_directory_path() / "cutline-look-pack";
  std::filesystem::remove_all(directory);
  const auto& looks = cutline::ui::BuiltInLooks();
  CHECK(looks.size() >= std::size_t{8});
  CHECK_EQ(cutline::ui::WriteBuiltInLooks(directory, 17), static_cast<int>(looks.size()));
  // A second start writes nothing; a person's own file under a look's name is left alone; another size is rewritten.
  CHECK_EQ(cutline::ui::WriteBuiltInLooks(directory, 17), 0);
  {
    std::ofstream mine(directory / (looks[0].name + ".cube"), std::ios::trunc);
    mine << "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
  }
  CHECK_EQ(cutline::ui::WriteBuiltInLooks(directory, 17), 0);
  CHECK_EQ(cutline::ui::WriteBuiltInLooks(directory, 9), static_cast<int>(looks.size()) - 1);

  cutline::ui::LutLibrary library;
  library.SetFolders({directory});
  library.Rescan();
  CHECK_EQ(library.entries().size(), looks.size());
  for (const auto& entry : library.entries()) CHECK(entry.error.empty() && !entry.curves);

  // Every look, as the table itself says it: black stays dark, white stays light, a grey ramp only ever gets lighter,
  // and nothing leaves the picture's range.
  for (const auto& look : looks) {
    for (const int size : {17, 33}) {
      cutline::ui::WriteBuiltInLooks(directory / std::to_string(size), size);
    }
    const auto lut = cutline::render::CubeLut::Load(directory / "33" / (look.name + ".cube"));
    CHECK_EQ(lut.size(), 33);
    auto previous = -1.0f;
    for (int i = 0; i <= 32; ++i) {
      const float v = static_cast<float>(i) / 32.0f;
      const auto out = lut.Sample(v, v, v);
      for (int c = 0; c < 3; ++c) CHECK(out[static_cast<std::size_t>(c)] >= 0.0f && out[static_cast<std::size_t>(c)] <= 1.0f);
      const float luma = 0.2126f * out[0] + 0.7152f * out[1] + 0.0722f * out[2];
      CHECK(luma >= previous - 1e-5f);
      previous = luma;
    }
    const auto black = lut.Sample(0.0f, 0.0f, 0.0f), white = lut.Sample(1.0f, 1.0f, 1.0f);
    CHECK(black[0] < 0.2f && black[1] < 0.2f && black[2] < 0.2f);
    CHECK(white[0] > 0.7f && white[1] > 0.7f && white[2] > 0.6f);
    // It is the grade the function says: the table at a lattice point is the function there.
    const auto direct = look.map(0.5f, 0.25f, 0.75f);
    const auto tabled = lut.Sample(0.5f, 0.25f, 0.75f);
    for (int c = 0; c < 3; ++c) CHECK(std::abs(direct[static_cast<std::size_t>(c)] - tabled[static_cast<std::size_t>(c)]) < 0.01f);
  }
  // The black and white look is neutral; the sepia is warm; a look is not the identity.
  const auto find = [&](const char* name) { return cutline::render::CubeLut::Load(directory / "33" / (std::string(name) + ".cube")); };
  const auto grey = find("Black and White").Sample(0.8f, 0.3f, 0.1f);
  CHECK(std::abs(grey[0] - grey[1]) < 1e-4f && std::abs(grey[1] - grey[2]) < 1e-4f);
  const auto brown = find("Sepia").Sample(0.5f, 0.5f, 0.5f);
  CHECK(brown[0] > brown[1] && brown[1] > brown[2]);
  const auto teal = find("Teal and Orange").Sample(0.15f, 0.15f, 0.15f);
  CHECK(teal[2] > teal[0]);   // cool shadows
  std::filesystem::remove_all(directory);
}

// ------------------------------------------------------------------------ the audio workflow ----

namespace {

std::vector<std::string> EffectTypes(const cutline::timeline::Clip& clip) {
  auto effects = clip.effects;
  std::sort(effects.begin(), effects.end(), [](const auto& a, const auto& b) { return a.order < b.order; });
  std::vector<std::string> types;
  for (const auto& effect : effects) types.push_back(effect.effect_type);
  return types;
}

double ParameterValue(const cutline::timeline::Effect& effect, const std::string& name) {
  for (const auto& parameter : effect.parameters) {
    if (parameter.name == name) return parameter.value.animated() ? parameter.value.keyframes().front().value.scalar() : parameter.value.constant().scalar();
  }
  return -9999.0;
}

}  // namespace

CUTLINE_TEST(ARoleIsKeptOnTheClipCarriedBySplitsAndItsChainIsReplacedNotStacked) {
  Project project;
  project.Add("dlg", "a1", 0, 0, 10);
  project.Add("pic", "v1", 0, 0, 10);
  auto snap = project.Snapshot();
  const auto steps = project.store().AppliedStepCount();
  cutline::ui::RoleChainOptions options;
  options.repair = 0.4;
  const auto plan = cutline::ui::PlanApplyRole(project.Context(snap), {"dlg"}, cutline::ui::AudioRole::Dialogue, true, options);
  CHECK(plan.ok && project.Apply(plan));
  CHECK_EQ(project.store().AppliedStepCount(), steps + 1);
  snap = project.Snapshot();
  CHECK(Find(snap, "dlg")->audio_role == "dialogue");
  CHECK((EffectTypes(*Find(snap, "dlg")) == std::vector<std::string>{"denoise", "eq", "compressor", "limiter"}));
  for (const auto& effect : Find(snap, "dlg")->effects) {
    CHECK(effect.id.rfind("role-", 0) == 0);
    if (effect.effect_type == "denoise") CHECK(std::abs(ParameterValue(effect, "amount") - 0.4) < 1e-9);
    if (effect.effect_type == "compressor") CHECK(ParameterValue(effect, "threshold") == -22.0 && ParameterValue(effect, "ratio") == 3.0);
  }
  // A second role replaces the first chain; the music duck names the track that pushes it down.
  cutline::ui::RoleChainOptions music;
  music.duck_under_track = "a2";
  CHECK(project.Apply(cutline::ui::PlanApplyRole(project.Context(snap), {"dlg"}, cutline::ui::AudioRole::Music, true, music)));
  snap = project.Snapshot();
  CHECK(Find(snap, "dlg")->audio_role == "music");
  CHECK((EffectTypes(*Find(snap, "dlg")) == std::vector<std::string>{"eq", "compressor", "duck", "limiter"}));
  for (const auto& effect : Find(snap, "dlg")->effects) {
    if (effect.effect_type == "duck") CHECK(effect.preset_name == "a2");
  }
  // A split keeps the role on both halves, and undo takes the whole role change back as one step.
  CHECK(project.Apply(cutline::ui::PlanSplit(project.Context(snap), S(4), {"a1"})));
  snap = project.Snapshot();
  CHECK_EQ(Count(snap, "a1"), std::size_t{2});
  for (const auto& clip : snap.FindTrack("a1")->clips) CHECK(clip.audio_role == "music");
  (void)project.store().Undo("tester", "2026-10-07T00:00:00Z");
  (void)project.store().Undo("tester", "2026-10-07T00:00:00Z");
  snap = project.Snapshot();
  CHECK(Find(snap, "dlg")->audio_role == "dialogue" && EffectTypes(*Find(snap, "dlg")).size() == 4);

  // The role without its chain, clearing, and what is refused: a picture clip, a clip that is not there, a locked track.
  CHECK(project.Apply(cutline::ui::PlanApplyRole(project.Context(snap), {"dlg"}, cutline::ui::AudioRole::Effects, false)));
  CHECK(Find(project.Snapshot(), "dlg")->audio_role == "effects" && EffectTypes(*Find(project.Snapshot(), "dlg")).size() == 4);   // chain left alone
  snap = project.Snapshot();
  CHECK(project.Apply(cutline::ui::PlanClearRole(project.Context(snap), {"dlg"})));
  CHECK(Find(project.Snapshot(), "dlg")->audio_role.empty() && Find(project.Snapshot(), "dlg")->effects.empty());
  snap = project.Snapshot();
  CHECK(!cutline::ui::PlanApplyRole(project.Context(snap), {"pic"}, cutline::ui::AudioRole::Dialogue, true).ok);
  CHECK(!cutline::ui::PlanApplyRole(project.Context(snap), {"nothing"}, cutline::ui::AudioRole::Dialogue, true).ok);
  CHECK(!cutline::ui::PlanApplyRole(project.Context(snap), {}, cutline::ui::AudioRole::Dialogue, true).ok);
  CHECK(!cutline::ui::PlanApplyRole(project.Context(snap), {"dlg"}, cutline::ui::AudioRole::None, true).ok);   // nothing to change
  project.Run(cmd::CommandType::SetTrackState, cmd::SetTrackStatePayload{"a1", true, false, false, 0.0, 0.0, "A1"});
  snap = project.Snapshot();
  CHECK(!cutline::ui::PlanApplyRole(project.Context(snap), {"dlg"}, cutline::ui::AudioRole::Dialogue, true).ok);
  // The store refuses a role it does not know, and the recipes are what they say.
  CHECK_THROWS(project.Run(cmd::CommandType::SetClipAudioRole, cmd::SetClipAudioRolePayload{"dlg", "narration"}));
  CHECK(cutline::ui::ParseAudioRole("music") == cutline::ui::AudioRole::Music && !cutline::ui::ParseAudioRole("narration").has_value());
  CHECK(cutline::ui::RoleChain(cutline::ui::AudioRole::None).empty());
  cutline::ui::RoleChainOptions bare;
  bare.repair = 0.0;
  bare.tone = false;
  bare.dynamics = false;
  CHECK(cutline::ui::RoleChain(cutline::ui::AudioRole::Dialogue, bare).empty());
}

CUTLINE_TEST(LoudnessAdviceNamesTheGainAndWarnsWhenThePeakWouldPassTheCeilingAndGainsAreOrdinaryEdits) {
  const auto* r128 = cutline::ui::FindLoudnessTarget("r128");
  const auto* streaming = cutline::ui::FindLoudnessTarget("streaming");
  CHECK(r128 != nullptr && streaming != nullptr && cutline::ui::FindLoudnessTarget("nothing") == nullptr);
  CHECK(r128->lufs == -23.0 && streaming->lufs == -14.0 && cutline::ui::LoudnessTargets().size() >= std::size_t{4});
  cutline::audio::dsp::Loudness measured;
  measured.integrated_lufs = -30.0;
  measured.true_peak_db = -9.0;
  auto advice = cutline::ui::AdviseLoudness(measured, *r128);
  CHECK(advice.measurable && std::abs(advice.gain_db - 7.0) < 1e-9 && !advice.over_ceiling && std::abs(advice.result_true_peak_db - (-2.0)) < 1e-9);
  advice = cutline::ui::AdviseLoudness(measured, *streaming);   // +16 dB would put the peak at +7
  CHECK(advice.over_ceiling && std::abs(advice.gain_for_ceiling_db - 8.0) < 1e-9 && advice.message.find("limiter") != std::string::npos);
  measured.integrated_lufs = -200.0;
  CHECK(!cutline::ui::AdviseLoudness(measured, *r128).measurable);
  CHECK(cutline::ui::RoleLoudness(cutline::ui::AudioRole::Dialogue) > cutline::ui::RoleLoudness(cutline::ui::AudioRole::Music));

  Project project;
  project.Add("a", "a1", 0, 0, 10);
  project.Add("v", "v1", 0, 0, 10);
  auto snap = project.Snapshot();
  const auto steps = project.store().AppliedStepCount();
  CHECK(project.Apply(cutline::ui::PlanSetClipGain(project.Context(snap), "a", 6.5)));
  CHECK_EQ(project.store().AppliedStepCount(), steps + 1);
  snap = project.Snapshot();
  CHECK_EQ(Find(snap, "a")->effects.size(), std::size_t{1});
  CHECK(Find(snap, "a")->effects[0].effect_type == "volume" && std::abs(ParameterValue(Find(snap, "a")->effects[0], "level") - 6.5) < 1e-9);
  // Again: the same effect changes, nothing is stacked; a gain past the range is held to it and says so.
  CHECK(project.Apply(cutline::ui::PlanSetClipGain(project.Context(snap), "a", -3.0)));
  snap = project.Snapshot();
  CHECK_EQ(Find(snap, "a")->effects.size(), std::size_t{1});
  CHECK(std::abs(ParameterValue(Find(snap, "a")->effects[0], "level") + 3.0) < 1e-9);
  const auto held = cutline::ui::PlanSetClipGain(project.Context(snap), "a", 40.0);
  CHECK(held.ok && !held.notes.empty());
  CHECK(!cutline::ui::PlanSetClipGain(project.Context(snap), "v", 3.0).ok);          // a picture clip has no gain
  CHECK(!cutline::ui::PlanSetClipGain(project.Context(snap), "missing", 3.0).ok);
  // The master: a Volume effect of the sequence.
  CHECK(project.Apply(cutline::ui::PlanSetMasterGain(project.Context(snap), -4.0)));
  snap = project.Snapshot();
  CHECK(snap.effects.size() == 1 && snap.effects[0].effect_type == "volume" && std::abs(ParameterValue(snap.effects[0], "level") + 4.0) < 1e-9);
  CHECK(project.Apply(cutline::ui::PlanSetMasterGain(project.Context(snap), -1.0)));
  snap = project.Snapshot();
  CHECK_EQ(snap.effects.size(), std::size_t{1});
  CHECK(std::abs(ParameterValue(snap.effects[0], "level") + 1.0) < 1e-9);
}

CUTLINE_TEST(AutomationIsWrittenByTheRuleOfItsModeThinnedAndReplacingOnlyWhatItCovers) {
  using cutline::ui::AutomationMode;
  using cutline::ui::AutomationSample;
  const auto at = [](double seconds) { return RationalTime(static_cast<std::int64_t>(seconds * 1000.0), 1000); };
  // A fader moved from -6 dB down to -18 dB over two seconds in a straight line, then held a second, then released.
  std::vector<AutomationSample> pass;
  for (int i = 0; i <= 20; ++i) pass.push_back({at(1.0 + 0.1 * i), -6.0 - 0.6 * i, true});
  pass.push_back({at(4.0), -18.0, false});
  const auto stop = at(6.0);

  CHECK(!cutline::ui::AutomationKeys(AutomationMode::Read, pass, stop, {}).any);
  CHECK(!cutline::ui::AutomationKeys(AutomationMode::Write, {}, stop, {}).any);
  // A straight move thins to its ends and the corner where it stops, however many samples made it.
  const auto written = cutline::ui::AutomationKeys(AutomationMode::Write, pass, stop, {});
  CHECK(written.any && written.keys.size() == 3);   // the start, the corner at 3 s, the stop
  CHECK(written.keys.front().first.Compare(at(1.0)) == 0 && written.keys.front().second == -6.0);
  CHECK(written.keys.back().first.Compare(stop) == 0 && written.keys.back().second == -18.0);   // held to the stop
  CHECK(written.span_start.Compare(at(1.0)) == 0 && written.span_end.Compare(stop) == 0);
  // A looser tolerance and a curve that is not a line.
  std::vector<AutomationSample> bend;
  for (int i = 0; i <= 10; ++i) bend.push_back({at(0.1 * i), static_cast<double>(i * i) * -0.1, true});
  CHECK(cutline::ui::AutomationKeys(AutomationMode::Write, bend, at(1.0), {}, {0.05, {1, 4}}).keys.size() > 4);
  CHECK(cutline::ui::AutomationKeys(AutomationMode::Write, bend, at(1.0), {}, {5.0, {1, 4}}).keys.size() == 2);

  // Latch starts at the first touch and keeps going to the stop with the value where the hand left it.
  std::vector<AutomationSample> late = {{at(0.5), -3.0, false}, {at(2.0), -5.0, true}, {at(3.0), -9.0, true}, {at(3.5), -9.0, false}};
  const auto latched = cutline::ui::AutomationKeys(AutomationMode::Latch, late, stop, {});
  CHECK(latched.any && latched.keys.front().first.Compare(at(2.0)) == 0 && latched.keys.back().first.Compare(stop) == 0 && latched.keys.back().second == -9.0);

  // Touch writes only while the hand is on, and goes back to what was automated: here a line from -10 at 0 s to -2 at 10 s.
  const std::vector<std::pair<RationalTime, double>> existing = {{at(0.0), -10.0}, {at(10.0), -2.0}};
  const auto touched = cutline::ui::AutomationKeys(AutomationMode::Touch, pass, stop, existing, {0.1, {1, 2}});
  CHECK(touched.any && touched.span_start.Compare(at(1.0)) == 0 && touched.span_end.Compare(at(4.5)) == 0);   // release at 4.0 + 0.5 s back
  CHECK(std::abs(touched.keys.back().second - (-10.0 + 8.0 * 0.45)) < 1e-9);    // the existing curve's value at 4.5 s
  // No hand on the control, nothing written. Without a curve to go back to, it goes back to where the touch began.
  CHECK(!cutline::ui::AutomationKeys(AutomationMode::Touch, {{at(1.0), -3.0, false}, {at(2.0), -4.0, false}}, stop, existing).any);
  const auto bare = cutline::ui::AutomationKeys(AutomationMode::Touch, pass, stop, {}, {0.1, {1, 2}});
  CHECK(bare.keys.back().second == -6.0);
  // Two touches in one pass are two writes in one set of keys.
  auto twice = pass;
  twice.push_back({at(5.0), -2.0, true});
  twice.push_back({at(5.5), -1.0, false});
  const auto both = cutline::ui::AutomationKeys(AutomationMode::Touch, twice, stop, existing, {0.1, {1, 2}});
  CHECK(both.span_end.Compare(at(6.0)) == 0 && both.keys.size() > touched.keys.size());

  // On the track: the effect is made, written, and a later write replaces only the keys inside the span it covers.
  Project project;
  project.Add("a", "a1", 0, 0, 30);
  auto snap = project.Snapshot();
  const auto steps = project.store().AppliedStepCount();
  CHECK(project.Apply(cutline::ui::PlanWriteAutomation(project.Context(snap), "a1", cutline::ui::AutomationTarget::Volume, written)));
  CHECK_EQ(project.store().AppliedStepCount(), steps + 1);
  snap = project.Snapshot();
  auto keys = cutline::ui::AutomationOf(*snap.FindTrack("a1"), cutline::ui::AutomationTarget::Volume);
  CHECK_EQ(keys.size(), std::size_t{3});
  CHECK(keys[0].second == -6.0 && keys[1].second == -18.0 && keys[2].second == -18.0);
  CHECK(cutline::ui::AutomationOf(*snap.FindTrack("a1"), cutline::ui::AutomationTarget::Pan).empty());
  // A pass over 3 s to 5 s with a different move: what is inside it is replaced, what is outside stays.
  std::vector<AutomationSample> second = {{at(3.0), -10.0, true}, {at(5.0), -2.0, true}, {at(5.0) , -2.0, false}};
  const auto rewrite = cutline::ui::AutomationKeys(AutomationMode::Write, second, at(5.0), {});
  CHECK(project.Apply(cutline::ui::PlanWriteAutomation(project.Context(snap), "a1", cutline::ui::AutomationTarget::Volume, rewrite)));
  snap = project.Snapshot();
  keys = cutline::ui::AutomationOf(*snap.FindTrack("a1"), cutline::ui::AutomationTarget::Volume);
  CHECK_EQ(keys.size(), std::size_t{4});   // 1 s kept, 3 s and 5 s written, 6 s (outside what was covered) kept
  CHECK(keys[0].first.Compare(at(1.0)) == 0 && keys[0].second == -6.0);
  CHECK(keys[1].first.Compare(at(3.0)) == 0 && keys[1].second == -10.0);   // the old -18 at 3 s was inside the span and is gone
  CHECK(keys[2].first.Compare(at(5.0)) == 0 && keys[2].second == -2.0);
  CHECK(keys[3].first.Compare(at(6.0)) == 0 && keys[3].second == -18.0);
  // Pan is its own effect; a bus or a missing or locked track is refused.
  CHECK(project.Apply(cutline::ui::PlanWriteAutomation(project.Context(snap), "a1", cutline::ui::AutomationTarget::Pan, cutline::ui::AutomationKeys(AutomationMode::Write, {{at(1.0), -0.5, true}, {at(2.0), 0.5, true}}, at(2.0), {}))));
  CHECK(!cutline::ui::AutomationOf(*project.Snapshot().FindTrack("a1"), cutline::ui::AutomationTarget::Pan).empty());
  snap = project.Snapshot();
  CHECK(!cutline::ui::PlanWriteAutomation(project.Context(snap), "v1", cutline::ui::AutomationTarget::Volume, written).ok);
  CHECK(!cutline::ui::PlanWriteAutomation(project.Context(snap), "nope", cutline::ui::AutomationTarget::Volume, written).ok);
  CHECK(!cutline::ui::PlanWriteAutomation(project.Context(snap), "a1", cutline::ui::AutomationTarget::Volume, {}).ok);
  CHECK(cutline::ui::ParseAutomationMode("touch") == AutomationMode::Touch && !cutline::ui::ParseAutomationMode("arm").has_value());
}

// --------------------------------------------------------------------------- transitions ----

CUTLINE_TEST(TransitionSitesAreTheCutsAndLoneEdgesOfPictureClipsNearestFirst) {
  Project project;
  project.Add("a", "v1", 0, 10, 20);
  project.Add("b", "v1", 10, 30, 40);
  project.Add("c", "v1", 25, 0, 5);          // a gap before it: its edges stand alone
  project.Add("sound", "a1", 0, 0, 10);     // sound clips are not sites
  auto snap = project.Snapshot();
  auto sites = cutline::ui::TransitionSitesNear(snap, S(10), S(3));
  CHECK_EQ(sites.size(), std::size_t{1});
  CHECK(sites[0].cut() && *sites[0].from_clip == "a" && *sites[0].to_clip == "b" && sites[0].at.Compare(S(10)) == 0 && sites[0].track_id == "v1");
  // Wider: the edges come after, by distance, and at the same distance a cut comes before a lone edge.
  sites = cutline::ui::TransitionSitesNear(snap, S(10), S(20));
  CHECK_EQ(sites.size(), std::size_t{5});   // a's start, the cut, b's end, c's start and end
  CHECK(sites[0].cut());
  CHECK(!sites[1].cut() && sites[1].at.Compare(S(0)) == 0 && !sites[1].from_clip.has_value() && *sites[1].to_clip == "a");   // equally far: in clip order
  CHECK(!sites[2].cut() && sites[2].at.Compare(S(20)) == 0 && *sites[2].from_clip == "b" && !sites[2].to_clip.has_value());
  CHECK(sites[3].at.Compare(S(25)) == 0 && sites[4].at.Compare(S(30)) == 0);
  CHECK(cutline::ui::TransitionSitesNear(snap, S(10), S(20), "v2").empty());
  CHECK(cutline::ui::TransitionSitesNear(snap, S(100), S(3)).empty());

  // Handles: what the media has beyond the frames the clip shows, as time on the timeline; sped up it is shorter, reversed it swaps.
  const auto ctx = project.Context(snap);
  const auto handles = cutline::ui::HandlesOf(ctx, *Find(snap, "a"));
  CHECK(handles.known && handles.head.Compare(S(10)) == 0 && handles.tail.Compare(S(40)) == 0);
  auto fast = *Find(snap, "a");
  fast.playback_rate = RationalTime(2, 1);
  CHECK(cutline::ui::HandlesOf(ctx, fast).head.Compare(S(5)) == 0 && cutline::ui::HandlesOf(ctx, fast).tail.Compare(S(20)) == 0);
  auto backwards = *Find(snap, "a");
  backwards.reversed = true;
  CHECK(cutline::ui::HandlesOf(ctx, backwards).head.Compare(S(40)) == 0 && cutline::ui::HandlesOf(ctx, backwards).tail.Compare(S(10)) == 0);
  auto generator = *Find(snap, "a");
  generator.source_kind = cutline::model::SourceKind::Adjustment;
  CHECK(!cutline::ui::HandlesOf(ctx, generator).known && cutline::ui::HandlesOf(ctx, generator).tail.Compare(S(1000)) > 0);
}

CUTLINE_TEST(ATransitionIsAddedAtTheCutKeptWithinTheHandlesAndRefusedWhereThereIsNoRoomOrAnotherIsThere) {
  Project project;
  project.Add("a", "v1", 0, 10, 20);     // 40 s of media after it
  project.Add("b", "v1", 10, 30, 40);    // 30 s before it
  project.Add("c", "v1", 20, 50, 60);    // nothing after it, 50 before
  project.Add("d", "v1", 30, 0, 10);     // nothing before it
  project.Add("e", "v1", 40, 5, 15);     // only 5 s before it
  auto snap = project.Snapshot();
  const auto steps = project.store().AppliedStepCount();
  const auto site = [&](const char* from, const char* to, double at) {
    cutline::ui::TransitionSite s;
    s.track_id = "v1";
    if (from != nullptr) s.from_clip = from;
    if (to != nullptr) s.to_clip = to;
    s.at = RationalTime(static_cast<std::int64_t>(at), 1);
    return s;
  };
  cutline::ui::TransitionRequest request;
  request.duration = S(2);

  // Centred: a second either side of the cut.
  auto plan = cutline::ui::PlanAddTransition(project.Context(snap), site("a", "b", 10), request);
  CHECK(plan.ok && plan.notes.empty() && plan.commands.size() == 1);
  CHECK(project.Apply(plan));
  CHECK_EQ(project.store().AppliedStepCount(), steps + 1);
  snap = project.Snapshot();
  const auto& transitions = snap.FindTrack("v1")->transitions;
  CHECK_EQ(transitions.size(), std::size_t{1});
  CHECK(transitions[0].kind == "cross_dissolve" && transitions[0].timeline_start.Compare(S(9)) == 0 && transitions[0].duration.Compare(S(2)) == 0);
  CHECK(transitions[0].alignment == cutline::model::TransitionAlignment::Center && *transitions[0].from_clip_id == "a" && *transitions[0].to_clip_id == "b");
  CHECK_NO_THROW(project.store().ValidateDatabase());

  // The same cut again, or one that overlaps it, is refused.
  CHECK(!cutline::ui::PlanAddTransition(project.Context(snap), site("a", "b", 10), request).ok);
  // Starting at the cut and ending at it.
  request.alignment = cutline::model::TransitionAlignment::Start;
  plan = cutline::ui::PlanAddTransition(project.Context(snap), site("b", "c", 20), request);
  CHECK(plan.ok && project.Apply(plan));
  request.alignment = cutline::model::TransitionAlignment::End;
  plan = cutline::ui::PlanAddTransition(project.Context(snap = project.Snapshot()), site("c", "d", 30), request);
  CHECK(!plan.ok && plan.refusal.find("room") != std::string::npos);    // c has nothing after it and d nothing before: with End it is d's head that is short
  request.alignment = cutline::model::TransitionAlignment::Center;
  CHECK(!cutline::ui::PlanAddTransition(project.Context(snap), site("c", "d", 30), request).ok);
  // Held to the handles: e has 5 s before it, so a centred transition is at most 10 s whatever is asked, in whole frames.
  request.duration = S(30);
  plan = cutline::ui::PlanAddTransition(project.Context(snap), site("d", "e", 40), request);
  CHECK(plan.ok && !plan.notes.empty());
  const auto& add = std::get<cutline::commands::AddTransitionPayload>(plan.commands[0].payload);
  CHECK(add.duration.Compare(S(10)) == 0 && add.timeline_start.Compare(S(35)) == 0);
  // A length that is not whole frames is cut back to whole frames, and a centred one to an even number of them.
  request.duration = RationalTime(33, 10);   // 3.3 s at 25 fps = 82.5 frames
  plan = cutline::ui::PlanAddTransition(project.Context(snap), site("d", "e", 40), request);
  CHECK(plan.ok);
  const auto& frames = std::get<cutline::commands::AddTransitionPayload>(plan.commands[0].payload);
  CHECK(frames.duration.Compare(RationalTime(82, 25)) == 0 && frames.timeline_start.Compare(S(40).Subtract(RationalTime(41, 25))) == 0);

  // Lone edges fade in at a clip's first frame and out at its last, whatever alignment was asked for.
  request.duration = S(2);
  request.alignment = cutline::model::TransitionAlignment::Center;
  plan = cutline::ui::PlanAddTransition(project.Context(snap), site(nullptr, "a", 0), request);
  CHECK(plan.ok && !plan.notes.empty());
  const auto& fade_in = std::get<cutline::commands::AddTransitionPayload>(plan.commands[0].payload);
  CHECK(fade_in.alignment == cutline::model::TransitionAlignment::Start && fade_in.timeline_start.Compare(S(0)) == 0 && !fade_in.from_clip_id.has_value());
  CHECK(project.Apply(plan));
  plan = cutline::ui::PlanAddTransition(project.Context(snap = project.Snapshot()), site("e", nullptr, 50), request);
  CHECK(plan.ok);
  const auto& fade_out = std::get<cutline::commands::AddTransitionPayload>(plan.commands[0].payload);
  CHECK(fade_out.alignment == cutline::model::TransitionAlignment::End && fade_out.timeline_start.Compare(S(48)) == 0);
  CHECK(project.Apply(plan));
  CHECK_NO_THROW(project.store().ValidateDatabase());

  // Refusals: a name the build does not have, clips that do not touch, a missing clip, a sound track, a locked track.
  request.kind = "page_curl_deluxe";
  CHECK(!cutline::ui::PlanAddTransition(project.Context(snap), site("d", "e", 40), request).ok);
  request.kind = "wipe_left";
  CHECK(!cutline::ui::PlanAddTransition(project.Context(snap), site("a", "c", 10), request).ok);
  CHECK(!cutline::ui::PlanAddTransition(project.Context(snap), site("nothing", "b", 10), request).ok);
  auto on_sound = site("a", "b", 10);
  on_sound.track_id = "a1";
  CHECK(!cutline::ui::PlanAddTransition(project.Context(snap), on_sound, request).ok);
  project.Run(cmd::CommandType::SetTrackState, cmd::SetTrackStatePayload{"v1", true, false, false, 0.0, 0.0, "V1"});
  snap = project.Snapshot();
  CHECK(!cutline::ui::PlanAddTransition(project.Context(snap), site("d", "e", 40), request).ok);
}

CUTLINE_TEST(ATransitionIsChangedToAnotherKindAndLengthInOneStepAndTakenOffAgain) {
  Project project;
  project.Add("a", "v1", 0, 10, 20);
  project.Add("b", "v1", 10, 30, 40);
  auto snap = project.Snapshot();
  cutline::ui::TransitionSite site;
  site.track_id = "v1";
  site.from_clip = "a";
  site.to_clip = "b";
  site.at = S(10);
  CHECK(project.Apply(cutline::ui::PlanAddTransition(project.Context(snap), site, {"cross_dissolve", S(2), cutline::model::TransitionAlignment::Center})));
  snap = project.Snapshot();
  const auto id = snap.FindTrack("v1")->transitions[0].id;
  const auto steps = project.store().AppliedStepCount();
  // Another kind, longer, ending at the cut: the old one goes and the new one comes, as one step.
  const auto plan = cutline::ui::PlanChangeTransition(project.Context(snap), id, {"wipe_right", S(4), cutline::model::TransitionAlignment::End});
  CHECK(plan.ok && plan.commands.size() == 2 && plan.label == "Change Transition");
  CHECK(project.Apply(plan));
  CHECK_EQ(project.store().AppliedStepCount(), steps + 1);
  snap = project.Snapshot();
  CHECK_EQ(snap.FindTrack("v1")->transitions.size(), std::size_t{1});
  const auto& changed = snap.FindTrack("v1")->transitions[0];
  CHECK(changed.kind == "wipe_right" && changed.timeline_start.Compare(S(6)) == 0 && changed.duration.Compare(S(4)) == 0 && changed.alignment == cutline::model::TransitionAlignment::End);
  (void)project.store().Undo("tester", "2026-10-07T00:00:00Z");
  snap = project.Snapshot();
  CHECK(snap.FindTrack("v1")->transitions[0].kind == "cross_dissolve" && snap.FindTrack("v1")->transitions[0].duration.Compare(S(2)) == 0);
  // The new length is checked against the handles as a new transition is; a change that cannot be made leaves the old one.
  CHECK(!cutline::ui::PlanChangeTransition(project.Context(snap), id, {"nonsense", S(2), cutline::model::TransitionAlignment::Center}).ok);
  CHECK(!cutline::ui::PlanChangeTransition(project.Context(snap), "no-such", {"wipe_left", S(2), cutline::model::TransitionAlignment::Center}).ok);
  // Taking it off.
  CHECK(project.Apply(cutline::ui::PlanRemoveTransition(project.Context(snap), id)));
  CHECK(project.Snapshot().FindTrack("v1")->transitions.empty());
  CHECK(!cutline::ui::PlanRemoveTransition(project.Context(project.Snapshot()), id).ok);
  CHECK_NO_THROW(project.store().ValidateDatabase());
}

// ------------------------------------------------------------------ the graphics designer ----

namespace {

namespace dz = cutline::ui;
namespace gx = cutline::render::graphics;

bool Close(double a, double b, double tolerance = 1e-6) { return std::abs(a - b) <= tolerance; }

}  // namespace

CUTLINE_TEST(TheDesignerAddsDuplicatesStacksAndRemovesElementsAndChecksWhatItIsGiven) {
  auto document = dz::NewGraphicDocument(1000, 1000);
  const auto text = dz::AddElement(document, gx::ElementType::Text);
  const auto box = dz::AddElement(document, gx::ElementType::Rectangle);
  const auto oval = dz::AddElement(document, gx::ElementType::Ellipse);
  const auto picture = dz::AddElement(document, gx::ElementType::Image);
  CHECK(text == "text-1" && box == "rectangle-1" && oval == "ellipse-1" && picture == "image-1");
  CHECK_EQ(document.elements.size(), std::size_t{4});
  const auto copy = dz::DuplicateElement(document, box);
  CHECK(copy == "rectangle-2" && dz::FindElement(document, copy)->x > dz::FindElement(document, box)->x);
  CHECK(dz::DuplicateElement(document, "nothing").empty());

  // Stacking: later is on top.
  CHECK(dz::Restack(document, text, dz::Stacking::Front) && document.elements.back().id == text);
  CHECK(!dz::Restack(document, text, dz::Stacking::Front) && !dz::Restack(document, text, dz::Stacking::Forward));
  CHECK(dz::Restack(document, text, dz::Stacking::Backward) && document.elements[document.elements.size() - 2].id == text);
  CHECK(dz::Restack(document, text, dz::Stacking::Back) && document.elements.front().id == text);
  CHECK(dz::RemoveElement(document, copy) && !dz::RemoveElement(document, copy));

  // An image with no picture is a draft: reported, shown as a placeholder, and not an error to have.
  CHECK_EQ(dz::Problems(document).size(), std::size_t{1});
  CHECK(dz::ForPreview(document).elements.size() == document.elements.size());
  for (const auto& element : dz::ForPreview(document).elements) CHECK(element.type != gx::ElementType::Image);
  CHECK_NO_THROW(gx::Validate(dz::ForPreview(document)));
  dz::SetProperty(*dz::FindElement(document, picture), "asset", "media:m1");
  CHECK(dz::Problems(document).empty());

  // Properties as text, both ways, and what is refused changes nothing.
  auto& element = *dz::FindElement(document, text);
  dz::SetProperty(element, "text", "Hello");
  dz::SetProperty(element, "font_size", "0.2");
  dz::SetProperty(element, "bold", "true");
  dz::SetProperty(element, "fill", "#FF8000");
  dz::SetProperty(element, "stroke", "0,0,1,0.5");
  dz::SetProperty(element, "rotation", "-15");
  dz::SetProperty(element, "shadow", "#000000C0");
  dz::SetProperty(element, "anchor_x", "center");
  CHECK(dz::PropertyValue(element, "text") == "Hello" && dz::PropertyValue(element, "bold") == "true" && dz::PropertyValue(element, "fill") == "#FF8000FF");
  CHECK(Close(element.fill[1], 128.0 / 255.0) && Close(element.stroke[3], 0.5) && Close(element.shadow[3], 192.0 / 255.0));
  CHECK(Close(element.rotation, -15.0) && element.anchor_x == "center");
  const auto before = dz::PropertyValue(element, "x");
  CHECK_THROWS(dz::SetProperty(element, "x", "left"));
  CHECK_THROWS(dz::SetProperty(element, "fill", "#12345"));
  CHECK_THROWS(dz::SetProperty(element, "align", "justify"));
  CHECK_THROWS(dz::SetProperty(element, "stroke_width", "5"));
  CHECK_THROWS(dz::SetProperty(element, "colour", "red"));
  CHECK(dz::PropertyValue(element, "x") == before && Close(element.stroke_width, 0.0));
  CHECK(dz::ElementLabel(element) == "Hello" && dz::ElementLabel(*dz::FindElement(document, oval)) == "ellipse ellipse-1");

  // Every property a panel is told about can be read and set back.
  for (const auto type : {gx::ElementType::Text, gx::ElementType::Rectangle, gx::ElementType::Ellipse, gx::ElementType::Image}) {
    const auto list = dz::PropertiesOf(type);
    CHECK(list.size() >= 14);
    gx::Element probe;
    probe.id = "probe";
    probe.type = type;
    probe.asset = "x";
    for (const auto& property : list) {
      const auto value = dz::PropertyValue(probe, property.name);
      CHECK_NO_THROW(dz::SetProperty(probe, property.name, value));
      CHECK(dz::PropertyValue(probe, property.name) == value);
    }
  }
}

CUTLINE_TEST(ClickingTheCanvasSelectsTheTopElementAllowingForItsTurnAndGripsFollowTheTurn) {
  auto document = dz::NewGraphicDocument(1000, 1000);
  gx::Element under;
  under.id = "under";
  under.x = 0.1; under.y = 0.1; under.width = 0.5; under.height = 0.5;
  gx::Element over = under;
  over.id = "over";
  over.x = 0.3; over.y = 0.3;
  document.elements = {under, over};
  CHECK(*dz::HitElement(document, {0.4, 0.4}) == "over" && *dz::HitElement(document, {0.2, 0.2}) == "under");
  CHECK(!dz::HitElement(document, {0.9, 0.9}).has_value());

  // A tall bar turned a quarter: its old top is empty now and its new side is covered.
  gx::Element bar;
  bar.id = "bar";
  bar.x = 0.4; bar.y = 0.1; bar.width = 0.2; bar.height = 0.6;
  bar.rotation = 90.0;
  document.elements = {bar};
  CHECK(!dz::HitElement(document, {0.5, 0.15}).has_value() && *dz::HitElement(document, {0.25, 0.4}) == "bar");

  // Grips turn with it: the right grip of the turned bar is at its bottom now.
  const auto grips = dz::GripsOf(document, bar);
  CHECK_EQ(grips.size(), std::size_t{9});
  const auto grip_at = [&](dz::Grip which) {
    for (const auto& grip : grips) if (grip.grip == which) return grip.at;
    return dz::DocPoint{-1, -1};
  };
  CHECK(Close(grip_at(dz::Grip::Right).x, 0.5, 1e-9) && Close(grip_at(dz::Grip::Right).y, 0.4 + 0.1, 1e-9));
  CHECK(dz::GripAt(document, bar, grip_at(dz::Grip::Right)) == dz::Grip::Right);
  CHECK(dz::GripAt(document, bar, grip_at(dz::Grip::TopLeft)) == dz::Grip::TopLeft);
  CHECK(dz::GripAt(document, bar, grip_at(dz::Grip::Turn)) == dz::Grip::Turn);
  CHECK(dz::GripAt(document, bar, {0.5, 0.4}) == dz::Grip::Body);
  CHECK(dz::GripAt(document, bar, {0.95, 0.95}) == dz::Grip::None);
}

CUTLINE_TEST(DraggingMovesWithSnapResizesFromTheOppositeEdgeTurnsAndAlignsToTheDocument) {
  auto document = dz::NewGraphicDocument(1000, 1000);
  const auto id = dz::AddElement(document, gx::ElementType::Rectangle);
  auto& element = *dz::FindElement(document, id);
  element.x = 0.3; element.y = 0.3; element.width = 0.4; element.height = 0.4;   // centred already

  // Close to the middle it lands on it; the guide says where.
  auto guides = dz::MoveBy(document, id, 0.004, 0.0);
  CHECK(Close(element.x, 0.3) && guides.size() >= 1 && guides.front().vertical && Close(guides.front().at, 0.5));
  guides = dz::MoveBy(document, id, 0.1, 0.0);
  CHECK(Close(element.x, 0.4) && Close(element.y, 0.3));
  // Without snap, or beyond the threshold, it goes where it was dragged.
  (void)dz::MoveBy(document, id, 0.0035, 0.0, false);
  CHECK(Close(element.x, 0.4035));
  element.x = 0.3;
  guides = dz::MoveBy(document, id, 0.1, 0.0);
  CHECK(Close(element.x, 0.4));
  element.x = 0.3;
  (void)dz::MoveBy(document, id, 0.02, 0.0);
  CHECK(Close(element.x, 0.32));
  // Its edge snaps to the document's edge.
  element.x = 0.3;
  (void)dz::MoveBy(document, id, -0.296, 0.0);
  CHECK(Close(element.x, 0.0));

  // Resizing the right edge leaves the left where it was.
  element.x = 0.2; element.y = 0.2; element.width = 0.2; element.height = 0.2;
  dz::ResizeBy(document, id, dz::Grip::Right, 0.1, 0.5);
  CHECK(Close(element.x, 0.2) && Close(element.width, 0.3) && Close(element.y, 0.2) && Close(element.height, 0.2));
  dz::ResizeBy(document, id, dz::Grip::TopLeft, 0.05, 0.05);
  CHECK(Close(element.x, 0.25) && Close(element.width, 0.25) && Close(element.y, 0.25) && Close(element.height, 0.15));
  // Never below a hundredth of the document.
  dz::ResizeBy(document, id, dz::Grip::Right, -5.0, 0.0);
  CHECK(Close(element.width, 0.01) && Close(element.x, 0.25));
  // Proportions kept from a corner.
  element.x = 0.1; element.y = 0.1; element.width = 0.2; element.height = 0.1;
  dz::ResizeBy(document, id, dz::Grip::BottomRight, 0.2, 0.01, true);
  CHECK(Close(element.width / element.height, 2.0, 1e-9) && Close(element.x, 0.1) && Close(element.y, 0.1));

  // Turned a quarter, dragging its right grip (now at the bottom) down by a tenth lengthens it that way; the left edge holds.
  element.x = 0.4; element.y = 0.4; element.width = 0.2; element.height = 0.2; element.rotation = 90.0;
  const auto left_before = [&] {
    for (const auto& grip : dz::GripsOf(document, element)) if (grip.grip == dz::Grip::Left) return grip.at;
    return dz::DocPoint{};
  }();
  dz::ResizeBy(document, id, dz::Grip::Right, 0.0, 0.1);
  CHECK(Close(element.width, 0.3) && Close(element.height, 0.2));
  for (const auto& grip : dz::GripsOf(document, element)) {
    if (grip.grip == dz::Grip::Left) CHECK(Close(grip.at.x, left_before.x, 1e-9) && Close(grip.at.y, left_before.y, 1e-9));
    if (grip.grip == dz::Grip::Right) CHECK(Close(grip.at.x, 0.5, 1e-9) && Close(grip.at.y, 0.7, 1e-9));
  }

  // Turning: the top points at the pointer, clockwise positive, and near a multiple of fifteen it snaps.
  element.rotation = 0.0; element.x = 0.4; element.y = 0.4; element.width = 0.2; element.height = 0.2;
  dz::TurnTo(document, id, {0.8, 0.5});
  CHECK(Close(element.rotation, 90.0));
  dz::TurnTo(document, id, {0.5, 0.1});
  CHECK(Close(element.rotation, 0.0));
  dz::TurnTo(document, id, {0.5 + std::sin(44.0 * 3.14159265358979 / 180.0), 0.5 - std::cos(44.0 * 3.14159265358979 / 180.0)});
  CHECK(Close(element.rotation, 45.0));
  dz::TurnTo(document, id, {0.5 + std::sin(38.0 * 3.14159265358979 / 180.0), 0.5 - std::cos(38.0 * 3.14159265358979 / 180.0)});
  CHECK(Close(element.rotation, 38.0, 1e-6));
  dz::TurnTo(document, id, {0.5 + std::sin(44.0 * 3.14159265358979 / 180.0), 0.5 - std::cos(44.0 * 3.14159265358979 / 180.0)}, 0.0);
  CHECK(Close(element.rotation, 44.0, 1e-6));

  // Aligning uses what the turned element covers.
  element.rotation = 0.0; element.x = 0.3; element.y = 0.3; element.width = 0.2; element.height = 0.4;
  dz::AlignElement(document, id, dz::Alignment::Left);
  CHECK(Close(element.x, 0.0));
  dz::AlignElement(document, id, dz::Alignment::Right);
  CHECK(Close(element.x, 0.8));
  dz::AlignElement(document, id, dz::Alignment::Centre);
  CHECK(Close(element.x, 0.4));
  dz::AlignElement(document, id, dz::Alignment::Bottom);
  CHECK(Close(element.y, 0.6));
  element.rotation = 90.0;   // 0.4 across and 0.2 down once turned, about the same middle
  dz::AlignElement(document, id, dz::Alignment::Top);
  CHECK(Close(element.y + element.height / 2.0, 0.1, 1e-9));
  dz::AlignElement(document, id, dz::Alignment::Left);
  CHECK(Close(element.x + element.width / 2.0, 0.2, 1e-9));
}

CUTLINE_TEST(EveryBuiltInTemplateIsValidDrawsWithItsDefaultsAndMakesControlsForWhatAPersonChanges) {
  const auto templates = dz::BuiltInTemplates();
  CHECK(templates.size() >= 6);
  std::set<std::string> ids;
  for (const auto& package : templates) {
    CHECK(ids.insert(package.id).second && !package.name.empty() && !package.controls.empty());
    CHECK_NO_THROW(gx::Validate(package));
    const auto round_trip = gx::ParseTemplate(gx::ToJson(package));
    CHECK(round_trip.id == package.id && round_trip.controls.size() == package.controls.size());
    const auto document = gx::Instantiate(round_trip, {});
    cutline::render::Layer canvas;
    canvas.Reset(640, 360);
    const auto result = gx::Draw(canvas, document, {}, 2.0);
    CHECK(result.elements_drawn == static_cast<int>(document.elements.size()) || !cutline::render::text::Available());
    CHECK(!canvas.empty());
    // A person's values change what is drawn.
    std::map<std::string, std::string> values;
    for (const auto& control : package.controls) {
      if (control.property == "text") values[control.name] = "Edited";
      if (control.property == "fill") values[control.name] = "#00FF00";
    }
    const auto edited = gx::Instantiate(package, values);
    bool changed = false;
    for (std::size_t i = 0; i < edited.elements.size(); ++i) changed = changed || edited.elements[i].text != package.document.elements[i].text || edited.elements[i].fill != package.document.elements[i].fill;
    CHECK(changed);
  }
  // Controls from a document: the text and colour of each text element, the picture of each image, the colour of each shape.
  auto document = dz::NewGraphicDocument();
  (void)dz::AddElement(document, gx::ElementType::Text);
  (void)dz::AddElement(document, gx::ElementType::Image);
  (void)dz::AddElement(document, gx::ElementType::Rectangle);
  dz::FindElement(document, "image-1")->asset = "x";
  const auto controls = dz::AutoControls(document);
  CHECK_EQ(controls.size(), std::size_t{4});
  std::set<std::string> names;
  for (const auto& control : controls) CHECK(names.insert(control.name).second);
  const auto package = dz::MakeTemplate(document, "mine", "Mine", "", controls);
  CHECK(package.version == 1 && package.controls.size() == 4);
  CHECK_THROWS(dz::MakeTemplate(document, "", "Mine", "", controls));
}

CUTLINE_TEST(GraphicsAreCreatedPlacedOnTheTopFreeTrackEditedAndOnlyDeletedWhenNothingShowsThem) {
  Project project;
  auto document = dz::NewGraphicDocument();
  const auto text = dz::AddElement(document, gx::ElementType::Text);
  dz::SetProperty(*dz::FindElement(document, text), "text", "Hello");
  auto snap = project.Snapshot();
  const auto steps = project.store().AppliedStepCount();

  std::string graphic;
  auto plan = dz::PlanCreateGraphic(project.Context(snap), "Greeting", document, &graphic);
  CHECK(plan.ok && !graphic.empty() && project.Apply(plan));
  CHECK_EQ(project.store().AppliedStepCount(), steps + 1);
  CHECK(!dz::PlanCreateGraphic(project.Context(snap), "Bad", [&] { auto bad = document; bad.elements[0].opacity = 4; return bad; }()).ok);

  // On the top free picture track at that time: v2 here, then v1 once v2 is taken, then a new one.
  plan = dz::PlanPlaceGraphic(project.Context(snap), graphic, "Greeting", S(5), S(3));
  CHECK(plan.ok && plan.commands.size() == 1 && project.Apply(plan));
  snap = project.Snapshot();
  const auto* clip = Find(snap, "clip-n2");
  CHECK(clip != nullptr && TrackOf(snap, "clip-n2") == "v2" && clip->timeline_start.Compare(S(5)) == 0 && clip->duration().Compare(S(3)) == 0);
  CHECK(!clip->effects.empty() && clip->effects.front().preset_name == "project:" + graphic);
  plan = dz::PlanPlaceGraphic(project.Context(snap), graphic, "Greeting", S(6), S(1));
  CHECK(plan.ok && project.Apply(plan));
  snap = project.Snapshot();
  CHECK(TrackOf(snap, "clip-n3") == "v1");
  project.Add("filler", "v1", 20, 0, 5);
  project.Add("filler2", "v2", 20, 0, 5);
  snap = project.Snapshot();
  plan = dz::PlanPlaceGraphic(project.Context(snap), graphic, "Greeting", S(21), S(2));
  CHECK(plan.ok && plan.commands.size() == 2 && plan.notes.size() == 1 && project.Apply(plan));
  snap = project.Snapshot();
  CHECK_EQ(snap.tracks.size(), std::size_t{5});
  CHECK(TrackOf(snap, "clip-n5") != "v1" && TrackOf(snap, "clip-n5") != "v2");
  // A named track must be free and not locked; a zero length is refused.
  CHECK(!dz::PlanPlaceGraphic(project.Context(snap), graphic, "x", S(5), S(1), "v2").ok);
  CHECK(!dz::PlanPlaceGraphic(project.Context(snap), graphic, "x", S(40), S(1), "a1").ok);
  CHECK(!dz::PlanPlaceGraphic(project.Context(snap), graphic, "x", S(40), S(0)).ok);
  CHECK(dz::PlanPlaceGraphic(project.Context(snap), graphic, "x", S(40), S(1), "v1").ok);

  // Editing the document changes every clip showing it, in one step; renaming and deleting.
  dz::FindElement(document, text)->text = "Goodbye";
  const auto edited_steps = project.store().AppliedStepCount();
  CHECK(project.Apply(dz::PlanSaveGraphic(project.Context(snap), graphic, document)));
  CHECK_EQ(project.store().AppliedStepCount(), edited_steps + 1);
  CHECK(project.Apply(dz::PlanRenameGraphic(project.Context(snap), graphic, "Farewell")));
  CHECK(!dz::PlanRenameGraphic(project.Context(snap), graphic, "").ok);
  const auto refused = dz::PlanDeleteGraphic(project.Context(snap), graphic);
  CHECK(!refused.ok && refused.refusal.find("timeline") != std::string::npos);
  std::string spare;
  CHECK(project.Apply(dz::PlanCreateGraphic(project.Context(snap), "Spare", document, &spare)));
  CHECK(project.Apply(dz::PlanDeleteGraphic(project.Context(snap), spare)));
  CHECK_NO_THROW(project.store().ValidateDatabase());
}

CUTLINE_TEST(ATemplateIsInstalledOnceMadeIntoTitlesAndItsValuesChangedInOneStep) {
  Project project;
  const auto snap = project.Snapshot();
  const auto package = dz::BuiltInTemplates().front();
  auto plan = dz::PlanInstallTemplate(project.Context(snap), package);
  CHECK(plan.ok && project.Apply(plan));
  // Installing the same package again is not an error; a changed one with the same version is.
  CHECK(project.Apply(dz::PlanInstallTemplate(project.Context(snap), package)));
  auto changed = package;
  changed.document.elements[0].opacity = 0.5;
  CHECK(!project.Apply(dz::PlanInstallTemplate(project.Context(snap), changed)));
  CHECK(!dz::PlanInstallTemplate(project.Context(snap), [&] { auto bad = package; bad.id.clear(); return bad; }()).ok);

  std::string title;
  const auto steps = project.store().AppliedStepCount();
  plan = dz::PlanCreateFromTemplate(project.Context(snap), "Host", package.id, package.version, {{"name", "Ada Lovelace"}}, &title);
  CHECK(plan.ok && project.Apply(plan));
  CHECK_EQ(project.store().AppliedStepCount(), steps + 1);
  CHECK(project.Apply(dz::PlanSetTemplateValues(project.Context(snap), title, {{"name", "Grace Hopper"}, {"role", "Rear Admiral"}})));
  CHECK_EQ(project.store().AppliedStepCount(), steps + 2);
  // A value that the template cannot take is refused and leaves the title as it was.
  CHECK(!project.Apply(dz::PlanSetTemplateValues(project.Context(snap), title, {{"no-such-control", "x"}})));
  // A title made from a template that is not installed is refused.
  CHECK(!project.Apply(dz::PlanCreateFromTemplate(project.Context(snap), "x", "ghost", 1, {})));
  CHECK_NO_THROW(project.store().ValidateDatabase());
}

int main() { return cutline::testing::RunAll("ui"); }
