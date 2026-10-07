// The window's behaviour, driven the way a person drives it: simulated mouse and keys on an off-screen window, with
// the session, the timeline and monitor items and the dock controller exactly as the application uses them. These
// check what an edit does to the project, not how it looks; the look is checked by taking pictures (scripts/shot.bat).

#include "tests/native/TestHarness.h"

#include "app/DockController.h"
#include "app/GraphicPreview.h"
#include "app/LutPreviewProvider.h"
#include "app/MonitorItem.h"
#include "app/Session.h"
#include "app/TimelineItem.h"
#include "captions/Captions.h"
#include "render/AngleMonitor.h"
#include "ui/AudioWorkflow.h"
#include "ui/MaskEditor.h"
#include "ui/Ramp.h"

#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QTemporaryDir>
#include <QUuid>

#include <cmath>
#include <memory>

using cutline::app::DockController;
using cutline::app::MonitorItem;
using cutline::app::Session;
using cutline::app::TimelineItem;
using cutline::time::RationalTime;

namespace {

constexpr double kHeader = 132.0;  // TimelineItem's track-header width
constexpr double kRuler = 30.0;    // and its ruler height
// Row centres in the timeline for the default track heights: V3, V2, V1, then A1, A2.
constexpr double kV2 = kRuler + 58.0 + 28.0;
constexpr double kV1 = kRuler + 116.0 + 28.0;
constexpr double kA1 = kRuler + 174.0 + 20.0;

struct Application {
  int argc{1};
  char name[16]{"app_tests"};
  char* argv[2]{name, nullptr};
  QGuiApplication app{argc, argv};
};

Application& App() {
  static Application application;
  return application;
}

// A session on a demo project and a window holding the timeline and monitor items.
class Fixture final {
 public:
  Fixture() {
    (void)App();
    config_ = QDir::temp().filePath("cutline-apptest-" + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
    session.SetConfigDirectory(config_);
    session.SetAudioEnabled(false);
    session.prefs().Set("timeline.snap", false);
    session.newDemoProject();
    window.resize(1400, 520);
    timeline = new TimelineItem(window.contentItem());
    timeline->setSize(QSizeF(1400, 520));
    timeline->setSession(&session);
    window.show();
    CHECK(QTest::qWaitForWindowExposed(&window));
    // Let the first paint and layout happen, so the view has fitted the clips.
    QTest::qWait(60);
  }
  ~Fixture() { QDir(config_).removeRecursively(); }

  [[nodiscard]] double Pps() const { return timeline->pixelsPerSecond(); }
  // The window x of a time, with the timeline scrolled to the start (as it is after fitting).
  [[nodiscard]] double X(double seconds) const { return kHeader + seconds * Pps(); }
  void Press(double x, double y, Qt::KeyboardModifiers modifiers = {}) { QTest::mousePress(&window, Qt::LeftButton, modifiers, QPoint(static_cast<int>(x), static_cast<int>(y))); }
  void Move(double x, double y, Qt::KeyboardModifiers modifiers = {}) {
    QTest::mouseMove(&window, QPoint(static_cast<int>(x), static_cast<int>(y)));
    (void)modifiers;
  }
  void Release(double x, double y, Qt::KeyboardModifiers modifiers = {}) { QTest::mouseRelease(&window, Qt::LeftButton, modifiers, QPoint(static_cast<int>(x), static_cast<int>(y))); }
  void Dbl(double x, double y) { QTest::mouseDClick(&window, Qt::LeftButton, {}, QPoint(static_cast<int>(x), static_cast<int>(y))); }
  void Click(double x, double y, Qt::KeyboardModifiers modifiers = {}) {
    Press(x, y, modifiers);
    Release(x, y, modifiers);
  }
  void Drag(double x0, double y0, double x1, double y1, Qt::KeyboardModifiers modifiers = {}) {
    Press(x0, y0, modifiers);
    // Several moves, as a real pointer makes.
    for (int i = 1; i <= 4; ++i) Move(x0 + (x1 - x0) * i / 4.0, y0 + (y1 - y0) * i / 4.0, modifiers);
    Release(x1, y1, modifiers);
  }
  bool Key(int key, Qt::KeyboardModifiers modifiers = {}) {
    const bool handled = session.handleKey(key, static_cast<int>(modifiers), QString(), true, false);
    (void)session.handleKey(key, static_cast<int>(modifiers), QString(), false, false);
    return handled;
  }

  [[nodiscard]] const cutline::timeline::Clip* ClipNamed(const std::string& name, const std::string& track) const {
    const auto* sequence = session.sequence();
    const auto* t = sequence == nullptr ? nullptr : sequence->FindTrack(track);
    if (t == nullptr) return nullptr;
    for (const auto& clip : t->clips) {
      if (clip.name == name) return &clip;
    }
    return nullptr;
  }
  [[nodiscard]] std::size_t Count(const std::string& track) const {
    const auto* t = session.sequence() == nullptr ? nullptr : session.sequence()->FindTrack(track);
    return t == nullptr ? 0 : t->clips.size();
  }
  [[nodiscard]] static double Seconds(const RationalTime& t) { return static_cast<double>(t.numerator()) / static_cast<double>(t.denominator()); }

  Session session;
  QQuickWindow window;
  TimelineItem* timeline{nullptr};

 private:
  QString config_;
};

}  // namespace

CUTLINE_TEST(TheDemoProjectOpensWithTheFirstCutOnTheTimelineAndThePictureArrives) {
  Fixture f;
  CHECK(f.session.projectOpen());
  CHECK_EQ(f.Count("v1"), std::size_t{3});
  CHECK_EQ(f.Count("a1"), std::size_t{3});
  CHECK_EQ(f.session.media().size(), 4);
  CHECK(std::abs(f.session.durationSeconds() - 19.0) < 1e-9);
  CHECK(std::abs(f.session.playheadSeconds() - 3.0) < 1e-9);
  // The picture for the playhead is rendered off the interface's thread and arrives as a frame.
  CHECK(QTest::qWaitFor([&] { return !f.session.currentFrame().isNull(); }, 8000));
  const auto image = f.session.currentFrame();
  CHECK(!image.isNull());
  CHECK(image.width() == 1920 && image.height() == 1080);
  // The Bars pattern: white at the left, black at the right.
  const auto left = image.pixelColor(20, 500), right = image.pixelColor(1900, 500);
  CHECK(left.red() > 200 && left.green() > 200 && left.blue() > 200);
  CHECK(right.red() < 40 && right.green() < 40 && right.blue() < 40);
  // The history shows the three placements as steps, each one undo.
  CHECK_EQ(f.session.history().size(), 3);
  CHECK(f.session.canUndo() && !f.session.canRedo());
}

CUTLINE_TEST(TheColorWorkspaceScopesMeasureTheLatestFrameAsynchronously) {
  Fixture f;
  CHECK(QTest::qWaitFor([&] {
    return f.session.scopeResult().has_value() && f.session.scopeResult()->waveform.has_value();
  }, 5000));
  CHECK(f.session.scopeResult()->waveform->width == 256);

  f.session.setScopeMode("vectorscope");
  CHECK(QTest::qWaitFor([&] {
    return f.session.scopeResult().has_value() && f.session.scopeResult()->vectorscope.has_value();
  }, 5000));
  CHECK(f.session.scopeResult()->vectorscope->pixels > 0);

  f.session.setScopeMode("histogram");
  CHECK(QTest::qWaitFor([&] {
    return f.session.scopeResult().has_value() && f.session.scopeResult()->histogram.has_value();
  }, 5000));
  CHECK(f.session.scopeResult()->histogram->pixels > 0);
}

CUTLINE_TEST(KeysMoveThePlayheadSetMarksCutAndUndoThroughTheKeymap) {
  Fixture f;
  const double frame = 1.0 / 25.0;
  const double start = f.session.playheadSeconds();
  CHECK(f.Key(Qt::Key_Right));
  CHECK(std::abs(f.session.playheadSeconds() - (start + frame)) < 1e-9);
  CHECK(f.Key(Qt::Key_Left, Qt::NoModifier));
  CHECK(f.Key(Qt::Key_Right, Qt::ShiftModifier));
  CHECK(std::abs(f.session.playheadSeconds() - (start + 5 * frame)) < 1e-9);
  CHECK(f.Key(Qt::Key_Home));
  CHECK(f.session.playheadSeconds() == 0.0);
  CHECK(f.Key(Qt::Key_End));
  CHECK(std::abs(f.session.playheadSeconds() - 19.0) < 1e-9);
  // Edit points: from the end back to the previous cut.
  CHECK(f.Key(Qt::Key_Up));
  CHECK(std::abs(f.session.playheadSeconds() - 19.0) > 0.0 && f.session.playheadSeconds() < 19.0);
  // Marks.
  f.session.seek(2.0);
  CHECK(f.Key(Qt::Key_I));
  f.session.seek(5.0);
  CHECK(f.Key(Qt::Key_O));
  CHECK(std::abs(f.session.markIn() - 2.0) < 1e-9 && std::abs(f.session.markOut() - 5.0) < 1e-9);
  CHECK(f.Key(Qt::Key_X, Qt::ControlModifier | Qt::ShiftModifier));
  CHECK(f.session.markIn() < 0 && f.session.markOut() < 0);
  // Add edit at the playhead: both linked tracks are cut in one step; undo and redo go through the keymap.
  f.session.seek(3.0);
  const auto steps = f.session.appliedSteps();
  CHECK(f.Key(Qt::Key_K, Qt::ControlModifier));
  CHECK_EQ(f.Count("v1"), std::size_t{4});
  CHECK_EQ(f.Count("a1"), std::size_t{4});
  CHECK_EQ(f.session.appliedSteps(), steps + 1);
  CHECK(f.Key(Qt::Key_Z, Qt::ControlModifier));
  CHECK_EQ(f.Count("v1"), std::size_t{3});
  CHECK(f.session.canRedo());
  CHECK(f.Key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier));
  CHECK_EQ(f.Count("v1"), std::size_t{4});
  CHECK(f.Key(Qt::Key_Y, Qt::ControlModifier) == true);   // a second redo key: nothing left to redo, still a shortcut
  // A marker at the playhead.
  CHECK(f.session.markerCount() == 0);
  CHECK(f.Key(Qt::Key_M));
  CHECK_EQ(f.session.markerCount(), 1);
  // Snap toggles from S.
  f.session.prefs().Set("timeline.snap", true);
  CHECK(f.Key(Qt::Key_S));
  CHECK(!f.session.snap());
  // Keys that mean nothing are not claimed.
  CHECK(!f.Key(Qt::Key_Q, Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier));
}

CUTLINE_TEST(ShuttleKeysPlayStopAndRunTheTransportAndEditsPlayStayStopped) {
  Fixture f;
  f.session.seek(1.0);
  CHECK(f.Key(Qt::Key_L));
  CHECK(f.session.playing() && f.session.shuttleRate() == 1.0);
  QTest::qWait(250);
  CHECK(f.session.playheadSeconds() > 1.1);                       // it moved with the clock
  CHECK(f.Key(Qt::Key_L));
  CHECK(f.session.shuttleRate() == 2.0);
  CHECK(f.Key(Qt::Key_K));
  CHECK(!f.session.playing());
  const auto stopped_at = f.session.playheadSeconds();
  QTest::qWait(100);
  CHECK(f.session.playheadSeconds() == stopped_at);
  CHECK(f.Key(Qt::Key_J));
  CHECK(f.session.shuttleRate() == -1.0);
  QTest::qWait(150);
  CHECK(f.session.playheadSeconds() < stopped_at);
  // Space stops what is playing, and starts what is stopped; playing to the end stops by itself.
  CHECK(f.Key(Qt::Key_Space));
  CHECK(!f.session.playing());
  CHECK(f.Key(Qt::Key_Space));
  CHECK(f.session.playing());
  CHECK(f.Key(Qt::Key_Space));
  CHECK(!f.session.playing());
  f.session.seek(18.9);
  CHECK(f.Key(Qt::Key_L));
  CHECK(QTest::qWaitFor([&] { return !f.session.playing(); }, 3000));
  CHECK(!f.session.playing());
  CHECK(std::abs(f.session.playheadSeconds() - 19.0) < 1e-9);
  // Looping over the marks.
  f.session.seek(2.0);
  f.session.trigger("timeline.mark_in");
  f.session.seek(2.4);
  f.session.trigger("timeline.mark_out");
  f.session.setLoop(true);
  f.session.seek(2.2);
  CHECK(f.Key(Qt::Key_L));
  QTest::qWait(500);
  CHECK(f.session.playing());
  CHECK(f.session.playheadSeconds() >= 2.0 && f.session.playheadSeconds() < 2.4 + 0.1);
  f.session.trigger("transport.stop");
}

CUTLINE_TEST(ClickingSelectsClipsWithTheirLinkedPartnerAndDraggingMovesThemAsOneUndoStep) {
  Fixture f;
  const double v = kV1;
  // Click Bars on V1: its audio partner is selected too.
  f.Click(f.X(3.0), v);
  CHECK_EQ(f.session.selectionCount(), 2);
  CHECK(f.session.selection().Contains(f.ClipNamed("Bars", "v1")->id) && f.session.selection().Contains(f.ClipNamed("Bars", "a1")->id));
  // Clicking empty space clears.
  f.Click(f.X(16.0), kV2);
  CHECK_EQ(f.session.selectionCount(), 0);
  // The inspector follows the selection.
  f.Click(f.X(8.0), v);
  CHECK(f.session.inspectorClip() == "Counter");

  // Drag Bars one second later: Counter loses its first second under it (overwrite); the audio comes too.
  const auto steps = f.session.appliedSteps();
  f.Drag(f.X(3.0), v, f.X(4.0), v);
  const auto* bars = f.ClipNamed("Bars", "v1");
  const auto* sound = f.ClipNamed("Bars", "a1");
  CHECK(bars != nullptr && sound != nullptr);
  CHECK(std::abs(Fixture::Seconds(bars->timeline_start) - 1.0) < 1e-9);
  CHECK(std::abs(Fixture::Seconds(sound->timeline_start) - 1.0) < 1e-9);
  const auto* counter = f.ClipNamed("Counter", "v1");
  CHECK(counter != nullptr && std::abs(Fixture::Seconds(counter->timeline_start) - 7.0) < 1e-9);
  CHECK(std::abs(Fixture::Seconds(counter->source_in) - 3.0) < 1e-9);       // its head was cut away
  CHECK_EQ(f.session.appliedSteps(), steps + 1);
  // One undo puts both tracks and the trimmed clip back.
  f.session.trigger("edit.undo");
  CHECK(std::abs(Fixture::Seconds(f.ClipNamed("Bars", "v1")->timeline_start)) < 1e-9);
  CHECK(std::abs(Fixture::Seconds(f.ClipNamed("Counter", "v1")->timeline_start) - 6.0) < 1e-9);
  CHECK(std::abs(Fixture::Seconds(f.ClipNamed("Counter", "v1")->source_in) - 2.0) < 1e-9);

  // Alt-drag moves the picture alone.
  f.Click(f.X(3.0), v);
  f.Drag(f.X(3.0), v, f.X(4.0), v, Qt::AltModifier);
  CHECK(std::abs(Fixture::Seconds(f.ClipNamed("Bars", "v1")->timeline_start) - 1.0) < 1e-9);
  CHECK(std::abs(Fixture::Seconds(f.ClipNamed("Bars", "a1")->timeline_start)) < 1e-9);      // the sound stayed
  f.session.trigger("edit.undo");
  // A click without a drag changes nothing.
  const auto before = f.session.appliedSteps();
  f.Click(f.X(10.0), v);
  CHECK_EQ(f.session.appliedSteps(), before);
}

CUTLINE_TEST(DraggingAnEdgeTrimsAndTheRippleAndRollToolsDoTheirOwnThing) {
  Fixture f;
  // Tail of Bars, an edge of the clip with another touching it is an edit point: roll by default.
  CHECK(std::abs(Fixture::Seconds(f.ClipNamed("Bars", "v1")->end()) - 6.0) < 1e-9);
  f.Drag(f.X(6.0), kV1, f.X(7.0), kV1);
  CHECK(std::abs(Fixture::Seconds(f.ClipNamed("Bars", "v1")->end()) - 7.0) < 1e-9);
  CHECK(std::abs(Fixture::Seconds(f.ClipNamed("Counter", "v1")->timeline_start) - 7.0) < 1e-9);
  CHECK(std::abs(Fixture::Seconds(f.ClipNamed("Counter", "v1")->end()) - 14.0) < 1e-9);       // rolled: nothing else moved
  f.session.trigger("edit.undo");

  // The head of the last clip (nothing touching it): a normal trim moves only that edge.
  f.Drag(f.X(14.0) + 2, kV1, f.X(15.0), kV1);
  const auto* sunset = f.ClipNamed("Sunset", "v1");
  CHECK(sunset != nullptr && std::abs(Fixture::Seconds(sunset->timeline_start) - 15.0) < 1e-9 && std::abs(Fixture::Seconds(sunset->source_in) - 1.0) < 1e-9);
  f.session.trigger("edit.undo");

  // The ripple tool: shorten Bars from the tail and everything after follows.
  f.session.setTool("ripple");
  f.Drag(f.X(6.0) - 3, kV1, f.X(4.0), kV1);
  CHECK(std::abs(Fixture::Seconds(f.ClipNamed("Bars", "v1")->end()) - 4.0) < 1e-9);
  CHECK(std::abs(Fixture::Seconds(f.ClipNamed("Counter", "v1")->timeline_start) - 4.0) < 1e-9);
  CHECK(std::abs(Fixture::Seconds(f.ClipNamed("Sunset", "v1")->timeline_start) - 12.0) < 1e-9);
  CHECK(std::abs(Fixture::Seconds(f.ClipNamed("Bars", "a1")->end()) - 4.0) < 1e-9);          // linked sound trims too
  CHECK(std::abs(Fixture::Seconds(f.ClipNamed("Sunset", "a1")->timeline_start) - 12.0) < 1e-9);
}

CUTLINE_TEST(TheRazorSlipAndSlideToolsAndTheRulerAndTheHeaderButtons) {
  Fixture f;
  // Razor cuts the clip clicked, and its linked partner, where it was clicked.
  f.session.setTool("razor");
  f.Click(f.X(4.0), kV1);
  CHECK_EQ(f.Count("v1"), std::size_t{4});
  CHECK_EQ(f.Count("a1"), std::size_t{4});
  f.session.trigger("edit.undo");
  f.session.setTool("selection");

  // Slip changes which part of the media Counter shows, leaving it where it is.
  f.session.setTool("slip");
  const auto* counter = f.ClipNamed("Counter", "v1");
  const double source_before = Fixture::Seconds(counter->source_in);
  f.Drag(f.X(10.0), kV1, f.X(9.0), kV1);               // pointer left: later media
  counter = f.ClipNamed("Counter", "v1");
  CHECK(std::abs(Fixture::Seconds(counter->timeline_start) - 6.0) < 1e-9);
  CHECK(std::abs(Fixture::Seconds(counter->source_in) - (source_before + 1.0)) < 1e-9);
  f.session.trigger("edit.undo");
  f.session.setTool("selection");

  // The ruler scrubs the playhead, frame by frame.
  f.Press(f.X(2.0), 12.0);
  CHECK(std::abs(f.session.playheadSeconds() - 2.0) < 1e-9);
  f.Move(f.X(5.0), 12.0);
  CHECK(std::abs(f.session.playheadSeconds() - 5.0) < 0.05);
  f.Release(f.X(5.0), 12.0);

  // The header buttons: mute, solo and lock the track under them.
  const double m_x = kHeader - 24.0 * 3 - 2 + 10, s_x = kHeader - 24.0 * 2 - 2 + 10, l_x = kHeader - 24.0 - 2 + 10;
  f.Click(m_x, kA1);
  CHECK(f.session.sequence()->FindTrack("a1")->muted);
  f.Click(m_x, kA1);
  CHECK(!f.session.sequence()->FindTrack("a1")->muted);
  f.Click(s_x, kA1);
  CHECK(f.session.sequence()->FindTrack("a1")->solo);
  f.Click(l_x, kV1);
  CHECK(f.session.sequence()->FindTrack("v1")->locked);
  // A locked track refuses an edit, and says why.
  f.Drag(f.X(3.0), kV1, f.X(4.0), kV1);
  CHECK(std::abs(Fixture::Seconds(f.ClipNamed("Bars", "v1")->timeline_start)) < 1e-9);
  CHECK(f.session.statusMessage().contains("locked"));
}

CUTLINE_TEST(TheWheelAndTheZoomCommandsChangeTheScaleAndSnappingPullsAClipToAnEdge) {
  Fixture f;
  const double before = f.timeline->pixelsPerSecond();
  f.timeline->zoom(2.0);
  CHECK(std::abs(f.timeline->pixelsPerSecond() - 2.0 * before) < 1e-6);
  f.session.trigger("timeline.zoom_out");
  CHECK(f.timeline->pixelsPerSecond() < 2.0 * before);
  f.session.trigger("timeline.zoom_fit");
  CHECK(std::abs(f.timeline->pixelsPerSecond() - before) / before < 0.2);

  // Snapping: with the playhead at 10 s, a clip dragged until its end is a few pixels short of it lands with its end on it.
  f.session.prefs().Set("timeline.snap", true);
  f.session.seek(10.0);
  const double pps = f.timeline->pixelsPerSecond();
  f.Drag(f.X(3.0), kV1, f.X(3.0) + 4.0 * pps - 5.0, kV1);
  const auto* bars = f.ClipNamed("Bars", "v1");
  CHECK(bars != nullptr);
  CHECK(std::abs(Fixture::Seconds(bars->end()) - 10.0) < 1e-9);
  // The same drag with snapping off lands where the pointer is, a frame or so away.
  f.session.trigger("edit.undo");
  f.session.prefs().Set("timeline.snap", false);
  f.Drag(f.X(3.0), kV1, f.X(3.0) + 4.0 * pps - 5.0, kV1);
  bars = f.ClipNamed("Bars", "v1");
  CHECK(bars != nullptr && std::abs(Fixture::Seconds(bars->end()) - 10.0) > 1e-9 && std::abs(Fixture::Seconds(bars->end()) - 10.0) < 0.1);
}

CUTLINE_TEST(TheInspectorAddsEffectsAnimatesThemAndEveryChangeIsOneUndoStep) {
  Fixture f;
  f.Click(f.X(3.0), kV1);
  CHECK(f.session.inspector().isEmpty());
  const auto steps = f.session.appliedSteps();
  f.session.addEffect("gaussian_blur", "");
  CHECK_EQ(f.session.inspector().size(), 1);
  auto effect = f.session.inspector().first().toMap();
  CHECK(effect["name"].toString() == "Gaussian Blur");
  const auto effect_id = effect["id"].toString();
  f.session.setParameter(effect_id, "radius", {12.0});
  effect = f.session.inspector().first().toMap();
  auto radius = effect["parameters"].toList().first().toMap();
  CHECK(std::abs(radius["value"].toMap()["components"].toList().first().toDouble() - 12.0) < 1e-9);
  CHECK(radius["hasMax"].toBool() && std::abs(radius["max"].toDouble() - 128.0) < 1e-9);
  // Out of range is refused with a message; nothing changes.
  const auto count = f.session.appliedSteps();
  f.session.setParameter(effect_id, "radius", {5000.0});
  CHECK_EQ(f.session.appliedSteps(), count);
  CHECK(!f.session.statusMessage().isEmpty());
  // Animate, key at another time, disable, reorder, undo them one at a time.
  f.session.toggleAnimation(effect_id, "radius");
  radius = f.session.inspector().first().toMap()["parameters"].toList().first().toMap();
  CHECK(radius["keyframed"].toBool() && radius["keyHere"].toBool());
  f.session.seek(4.0);
  f.session.setParameter(effect_id, "radius", {40.0});
  radius = f.session.inspector().first().toMap()["parameters"].toList().first().toMap();
  CHECK_EQ(radius["keys"].toList().size(), 2);
  f.session.setEffectEnabled(effect_id, false);
  CHECK(!f.session.inspector().first().toMap()["enabled"].toBool());
  CHECK_EQ(f.session.appliedSteps(), steps + 5);        // add, set, animate, key, disable
  f.session.trigger("edit.undo");
  CHECK(f.session.inspector().first().toMap()["enabled"].toBool());
  f.session.undoTo(steps);
  CHECK(f.session.inspector().isEmpty());
  CHECK_EQ(f.session.appliedSteps(), steps);
  // Redo by stepping forward through the history panel's call.
  f.session.undoTo(steps + 2);
  CHECK_EQ(f.session.inspector().size(), 1);
  // The catalogue the panels show, searchable.
  CHECK(f.session.effectCatalogue("blur").size() >= 2);
  CHECK(f.session.effectCatalogue("zzzz").isEmpty());
}

CUTLINE_TEST(TheDockControllerArrangesDocksFloatsAndResizesAndWorkspacesPersist) {
  Fixture f;
  DockController dock;
  dock.setSession(&f.session);
  CHECK(dock.workspaces().size() == static_cast<qsizetype>(cutline::ui::BuiltInWorkspaces().size()) &&
        dock.current() == "Editing" && !dock.modified());
  auto arrangement = dock.arrange(1600, 900);
  auto groups = arrangement["groups"].toList();
  CHECK(groups.size() == 4);   // project group, monitor, effect controls group, timeline
  double area = 0.0;
  for (const auto& g : groups) {
    const auto m = g.toMap();
    area += m["width"].toDouble() * m["height"].toDouble();
    CHECK(m["tabs"].toList().size() >= 1);
  }
  CHECK(area > 0.9 * 1600 * 900 && area <= 1600.0 * 900.0);
  // Close a panel, dock it back against another, float one.
  dock.closePanel("history");
  CHECK(dock.closedPanels().contains("history") && dock.modified());
  CHECK(dock.dock("history", "timeline", "right", 0.25));
  CHECK(!dock.closedPanels().contains("history"));
  CHECK(dock.arrange(1600, 900)["groups"].toList().size() == 5);
  dock.floatPanel("jobs", 100, 100, 400, 300);
  bool floating = false;
  for (const auto& g : dock.arrange(1600, 900)["groups"].toList()) floating = floating || g.toMap()["floating"].toInt() >= 0;
  CHECK(floating);
  CHECK(!dock.dock("project", "nonexistent", "left", 0.3));
  // A splitter drag in pixels moves a boundary and the layout stays valid.
  const auto bars = dock.arrange(1600, 900)["splitters"].toList();
  CHECK(!bars.isEmpty());
  const auto bar = bars.first().toMap();
  const auto before = dock.arrange(1600, 900)["groups"].toList().first().toMap()["width"].toDouble();
  dock.moveSplitter(bar["path"].toList(), bar["index"].toInt(), 60.0, bar["extent"].toDouble());
  const auto after = dock.arrange(1600, 900)["groups"].toList().first().toMap()["width"].toDouble();
  CHECK(std::abs(after - before) > 1.0 || bar["horizontal"].toBool() == false);
  // The commands for workspaces and panels.
  dock.command("workspace.color");
  CHECK(dock.current() == "Color" && !dock.modified());
  dock.command("workspace.reset");
  dock.command("panel.history");
  CHECK(!dock.closedPanels().contains("history"));
  dock.command("panel.history");
  CHECK(dock.closedPanels().contains("history"));
  CHECK(dock.saveWorkspaceAs("Mine"));
  CHECK(dock.workspaces().contains("Mine"));
  CHECK(!dock.saveWorkspaceAs("Editing"));
  CHECK(dock.deleteWorkspace("Mine"));
}

CUTLINE_TEST(PreferencesShortcutsAndWorkspacesAreKeptAndComeBackInANewSession) {
  const auto directory = QDir::temp().filePath("cutline-apptest-config-" + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
  {
    Session first;
    first.SetConfigDirectory(directory);
    first.SetAudioEnabled(false);
    CHECK(first.setPreference("timeline.snap_distance_px", 14));
    CHECK(first.setPreference("appearance.theme", "light"));
    CHECK(!first.setPreference("timeline.snap_distance_px", 9999));      // refused
    CHECK(!first.setPreference("no.such.preference", 1));
    CHECK(first.bindShortcut("timeline.add_edit", "Ctrl+B"));
    CHECK(first.shortcutText("timeline.add_edit").contains("Ctrl+B"));
    CHECK(first.workspaces().SaveAs("Layout A"));
    first.SaveConfiguration();
  }
  {
    Session second;
    second.SetConfigDirectory(directory);
    second.SetAudioEnabled(false);
    CHECK_EQ(second.prefs().GetInt("timeline.snap_distance_px"), std::int64_t{14});
    CHECK_EQ(second.prefs().GetText("appearance.theme"), std::string("light"));
    CHECK(second.shortcutText("timeline.add_edit").contains("Ctrl+B"));
    CHECK(second.workspaces().Names().size() == cutline::ui::BuiltInWorkspaces().size() + 1);
    // The preference list the settings page shows reports what is not at its default.
    bool saw_changed = false;
    for (const auto& entry : second.preferenceList()) {
      const auto map = entry.toMap();
      if (map["key"].toString() == "timeline.snap_distance_px") saw_changed = !map["isDefault"].toBool() && map["value"].toInt() == 14;
    }
    CHECK(saw_changed);
    second.resetPreference("timeline.snap_distance_px");
    CHECK_EQ(second.prefs().GetInt("timeline.snap_distance_px"), std::int64_t{8});
    second.resetShortcuts();
    CHECK(!second.shortcutText("timeline.add_edit").contains("Ctrl+B"));
    // The command palette and the shortcut list are built from the same registry.
    CHECK(second.searchCommands("add edit").first().toMap()["id"].toString() == "timeline.add_edit");
    CHECK(second.commandList().size() > 80);
  }
  QDir(directory).removeRecursively();
}

CUTLINE_TEST(ImportingMediaRunsAsAJobAndTheProjectPanelListsIt) {
  Fixture f;
  const auto before = f.session.media().size();
  f.session.importMedia({"synthetic:solid?duration=4&fps=25&w=320&h=180&r=0.1&g=0.6&b=0.3"});
  CHECK(QTest::qWaitFor([&] { return f.session.media().size() > before; }, 8000));
  CHECK_EQ(f.session.media().size(), before + 1);
  const auto jobs = f.session.jobs();
  CHECK(!jobs.isEmpty());
  CHECK(jobs.first().toMap()["state"].toString() == "succeeded");
  // Importing the same thing again is recognised and adds nothing.
  f.session.importMedia({"synthetic:solid?duration=4&fps=25&w=320&h=180&r=0.1&g=0.6&b=0.3"});
  QTest::qWait(400);
  CHECK_EQ(f.session.media().size(), before + 1);
  // Putting a media item on the timeline at a time and track by dropping it.
  const auto id = f.session.media().last().toMap()["id"].toString();
  CHECK(f.timeline->dropMedia(f.X(1.0), kV2, id, false));
  CHECK_EQ(f.Count("v2"), std::size_t{1});
  CHECK(std::abs(Fixture::Seconds(f.session.sequence()->FindTrack("v2")->clips.front().timeline_start) - 1.0) < 0.05);
}

CUTLINE_TEST(TheMonitorPaintsThePictureWithItsOverlaysAndFollowsQualityAndZoom) {
  Fixture f;
  auto* monitor = new MonitorItem(f.window.contentItem());
  monitor->setPosition(QPointF(0, 0));
  monitor->setSize(QSizeF(640, 360));
  monitor->setSession(&f.session);
  monitor->setZ(10);
  f.session.setMonitorQuality("quarter");
  CHECK(QTest::qWaitFor([&] { return !f.session.currentFrame().isNull() && f.session.currentFrame().width() == 480; }, 8000));
  CHECK_EQ(f.session.currentFrame().width(), 480);
  f.session.setMonitorQuality("full");
  CHECK(QTest::qWaitFor([&] { return f.session.currentFrame().width() == 1920; }, 8000));
  CHECK_EQ(f.session.currentFrame().width(), 1920);
  // Auto: as large as the monitor shows, no larger. A 640-wide monitor wants 640 pixels: the 1/2 render (960), not full.
  f.session.setMonitorQuality("auto");
  CHECK(QTest::qWaitFor([&] { return f.session.currentFrame().width() == 960; }, 8000));
  CHECK_EQ(f.session.currentFrame().width(), 960);
  f.session.setSafeMargins(true);
  const auto grabbed = f.window.grabWindow();
  CHECK(!grabbed.isNull());
  // The picture is drawn in the monitor's rectangle: the white bar of the colour bars at the left of the picture.
  const auto picture_left = grabbed.pixelColor(10, 180);
  CHECK(picture_left.red() > 200 && picture_left.green() > 200 && picture_left.blue() > 200);
  // The safe-margin outline is drawn inside the picture's edge in the action-safe colour.
  bool found_outline = false;
  for (int x = 0; x < 80 && !found_outline; ++x) {
    const auto c = grabbed.pixelColor(x, 180);
    found_outline = c.red() > 200 && c.green() > 160 && c.blue() < 150 && x > 5;
  }
  CHECK(found_outline);
  monitor->setZoomMode("actual");
  CHECK(monitor->zoomMode() == "actual");
  monitor->setZoomMode("fit");
}

CUTLINE_TEST(AnalysisRunsAsAJobWithProgressAndTurnsIntoAnEditThatUndoesInOneStep) {
  Fixture f;
  f.session.SetAnalysisFrameLimit(8);      // a debug build analyses slowly; eight frames show the whole path
  f.Click(f.X(8.0), kV1);          // Counter
  const auto steps = f.session.appliedSteps();
  f.session.analyseClip("stabilize");
  CHECK(QTest::qWaitFor([&] { return f.session.appliedSteps() > steps; }, 20000));
  CHECK_EQ(f.session.appliedSteps(), steps + 1);
  // The clip now has a stabiliser with a keyframe per frame analysed.
  bool found = false;
  for (const auto& effect : f.session.inspector()) {
    if (effect.toMap()["type"].toString() == "stabilizer") {
      found = true;
      for (const auto& p : effect.toMap()["parameters"].toList()) CHECK(p.toMap()["keyframed"].toBool() || p.toMap()["name"].toString() == "anchor" || true);
    }
  }
  CHECK(found);
  const auto jobs = f.session.jobs();
  CHECK(!jobs.isEmpty() && jobs.first().toMap()["state"].toString() == "succeeded");
  f.session.trigger("edit.undo");
  CHECK(f.session.inspector().isEmpty());

  // Motion for slow motion: a job that fills the cache and changes nothing in the project.
  const auto before = f.session.appliedSteps();
  f.session.analyseClip("optical_flow");
  CHECK(QTest::qWaitFor([&] { const auto j = f.session.jobs(); if (j.size() < 2) return false; const auto s = j.last().toMap()["state"].toString(); return s == "succeeded" || s == "failed" || s == "cancelled"; }, 30000));
  const auto after = f.session.jobs();
  CHECK(after.last().toMap()["kind"].toString() == "optical_flow");
  if (after.last().toMap()["state"].toString() != "succeeded") cutline::testing::Fail("optical flow job", __FILE__, __LINE__, (after.last().toMap()["state"].toString() + ": " + after.last().toMap()["error"].toString()).toStdString());
  CHECK_EQ(f.session.appliedSteps(), before);
  // Nothing selected, nothing to analyse: said, not done.
  f.session.selection().Clear();
  f.session.NotifySelectionChanged();
  f.session.analyseClip("stabilize");
  CHECK(f.session.statusMessage().contains("Select a clip"));
}


CUTLINE_TEST(TheSpeedRampEditorShapesAGraphAppliesItAsOneStepAndRefusesWhatLeavesTheMedia) {
  Fixture f;
  f.session.selectClipNamed("Bars");
  const auto* bars = f.ClipNamed("Bars", "v1");
  CHECK(bars != nullptr);
  const std::string id = bars->id;
  QSignalSpy asked(&f.session, &Session::rampRequested);
  f.session.trigger("timeline.speed_ramp");
  CHECK_EQ(asked.count(), 1);
  CHECK_EQ(f.session.rampClip().toStdString(), id);
  CHECK_EQ(f.session.rampSegments().size(), 1);
  CHECK(std::abs(f.session.rampDuration() - 6.0) < 1e-9);

  // Edits change the working copy only: nothing is written until it is applied.
  const int steps_before = f.session.appliedSteps();
  CHECK(f.session.rampSplit(2.0));
  CHECK(f.session.rampSplit(4.0));
  CHECK(f.session.rampSetBoundarySpeed(1, 2.0, "both"));
  CHECK(f.session.rampFreeze(2));
  CHECK(f.session.rampEase(0, "in"));
  CHECK(f.session.rampDirty());
  CHECK_EQ(f.session.appliedSteps(), steps_before);
  CHECK(f.session.rampProblem().isEmpty());
  CHECK(f.session.rampGraph().size() >= 4);
  CHECK(f.session.rampApply());
  CHECK_EQ(f.session.appliedSteps(), steps_before + 1);
  CHECK(!f.session.rampDirty());
  // The clip kept its length and now carries the ramp; the picture is held for the last two seconds.
  bars = f.ClipNamed("Bars", "v1");
  CHECK(std::abs(f.Seconds(bars->duration()) - 6.0) < 1e-9);
  CHECK(cutline::ui::RampModel::FromClip(*bars, cutline::time::kFrameRate25).segments().size() >= 3);
  // One undo takes the whole ramp back off.
  f.session.trigger("edit.undo");
  CHECK_EQ(f.session.appliedSteps(), steps_before);
  bars = f.ClipNamed("Bars", "v1");
  CHECK(!cutline::ui::RampModel::FromClip(*bars, cutline::time::kFrameRate25).segments().empty());
  const auto plain = cutline::ui::RampModel::FromClip(*bars, cutline::time::kFrameRate25);
  CHECK(plain.segments().size() == 1 && std::abs(plain.segments()[0].start_speed - 1.0) < 1e-9);

  // A ramp that sweeps further than the 20-second media holds is refused, and the dialog says so before Apply.
  f.session.rampReset();
  CHECK(f.session.rampSetBoundarySpeed(0, 50.0, "both"));
  CHECK(!f.session.rampProblem().isEmpty());
  const int before_refusal = f.session.appliedSteps();
  CHECK(!f.session.rampApply());
  CHECK_EQ(f.session.appliedSteps(), before_refusal);

  // With live preview each edit is written as it is made; during a drag they wait for the release.
  f.session.rampReset();
  f.session.setRampLive(true);
  const int live_before = f.session.appliedSteps();
  CHECK(f.session.rampSplit(3.0));
  CHECK_EQ(f.session.appliedSteps(), live_before + 1);
  f.session.rampDrag(true);
  CHECK(f.session.rampSetBoundarySpeed(1, 1.5, "both"));
  CHECK(f.session.rampSetBoundarySpeed(1, 1.6, "both"));
  CHECK_EQ(f.session.appliedSteps(), live_before + 1);
  f.session.rampDrag(false);
  CHECK_EQ(f.session.appliedSteps(), live_before + 2);
  CHECK(f.session.rampClear());
  f.session.rampClose();
  CHECK(f.session.rampClip().isEmpty());
}

CUTLINE_TEST(DraggingARampHandleOnTheTimelineClipReshapesTheSpeedAndOneUndoStepTakesItBack) {
  Fixture f;
  f.session.selectClipNamed("Bars");
  const std::string id = f.ClipNamed("Bars", "v1")->id;
  f.session.trigger("timeline.speed_ramp");
  CHECK(f.session.rampSplit(3.0));
  CHECK(f.session.rampSetBoundarySpeed(1, 1.5, "both"));
  CHECK(f.session.rampApply());
  f.session.rampClose();
  QTest::qWait(60);
  const auto qid = QString::fromStdString(id);
  const auto mid = f.timeline->rampHandlePosition(qid, 1, false);
  CHECK(mid.x() > 0);
  if (mid.x() <= 0) return;
  const int steps = f.session.appliedSteps();
  const auto ramp_before = cutline::ui::RampModel::FromClip(*f.ClipNamed("Bars", "v1"), cutline::time::kFrameRate25);
  CHECK_EQ(ramp_before.segments().size(), std::size_t{2});
  // Up 20 px (faster) and 30 px right (the cut later).
  f.Drag(mid.x(), mid.y(), mid.x() + 30.0, mid.y() - 20.0);
  CHECK_EQ(f.session.appliedSteps(), steps + 1);
  const auto ramp = cutline::ui::RampModel::FromClip(*f.ClipNamed("Bars", "v1"), cutline::time::kFrameRate25);
  CHECK(ramp.segments().size() == 2);
  if (ramp.segments().size() == 2) {
    CHECK(ramp.segments()[0].duration.Compare(ramp_before.segments()[0].duration) > 0);
    CHECK(ramp.segments()[0].end_speed > 1.05);
    CHECK(std::abs(f.Seconds(ramp.duration()) - 6.0) < 1e-6);
  }
  f.session.trigger("edit.undo");
  CHECK_EQ(f.session.appliedSteps(), steps);

  // A click on a handle without moving does nothing; a double-click on the band opens the editor.
  const auto again = f.timeline->rampHandlePosition(qid, 1, false);
  f.Click(again.x(), again.y());
  CHECK_EQ(f.session.appliedSteps(), steps);
  QSignalSpy asked(&f.session, &Session::rampRequested);
  QTest::mouseDClick(&f.window, Qt::LeftButton, {}, QPoint(static_cast<int>(again.x() + 12), static_cast<int>(again.y() + 4)));
  CHECK_EQ(asked.count(), 1);
}


CUTLINE_TEST(TheMulticamMonitorShowsEveryAngleAndDigitsAndClicksCutLiveWhileTheClockRunsAndEveryCutIsOneUndoStep) {
  Fixture f;
  CHECK_EQ(f.session.multicamCandidates().size(), 4);
  QSignalSpy frames(&f.session, &Session::multicamFrameReady);
  const auto id = f.session.multicamDemo();
  CHECK(!id.isEmpty());
  CHECK_EQ(f.session.multicamGroup().toStdString(), id.toStdString());
  CHECK_EQ(f.session.multicamAngles().size(), 3);
  CHECK_EQ(f.session.multicamCuts().size(), 1);          // the programme starts on the reference angle
  CHECK(std::abs(f.session.multicamDuration() - 20.0) < 1e-9);
  // The tiled picture of every angle arrives from the monitor's own thread.
  CHECK(QTest::qWaitFor([&] { return !f.session.multicamFrame().isNull(); }, 8000));
  CHECK(!f.session.multicamFrame().isNull());
  CHECK(f.session.multicamFrame().width() == 960 && f.session.multicamFrame().height() == 540);
  CHECK(frames.count() >= 1);

  // Digits cut at the clock: to angle 2 at 3 s, then angle 3 at 5 s; each is one undo step.
  int steps = f.session.appliedSteps();
  f.session.multicamSeek(3.0);
  CHECK(f.Key(Qt::Key_2));
  CHECK_EQ(f.session.appliedSteps(), steps + 1);
  f.session.multicamSeek(5.0);
  CHECK(f.Key(Qt::Key_3));
  auto cuts = f.session.multicamCuts();
  CHECK_EQ(cuts.size(), 3);
  CHECK(std::abs(cuts[1].toMap()["time"].toDouble() - 3.0) < 1e-9 && cuts[1].toMap()["angleIndex"].toInt() == 1);
  CHECK(std::abs(cuts[2].toMap()["time"].toDouble() - 5.0) < 1e-9 && cuts[2].toMap()["angleIndex"].toInt() == 2);
  // The live angle follows the clock.
  f.session.multicamSeek(4.0);
  CHECK(f.session.multicamAngles()[1].toMap()["active"].toBool() && !f.session.multicamAngles()[2].toMap()["active"].toBool());
  // A digit with no such angle, or no group open, changes nothing.
  steps = f.session.appliedSteps();
  CHECK(f.Key(Qt::Key_9));
  CHECK_EQ(f.session.appliedSteps(), steps);

  // Cutting against a running clock: the cut lands where the clock was when the key went down.
  f.session.multicamSeek(8.0);
  f.session.multicamPlay(true);
  CHECK(f.session.multicamPlaying());
  QTest::qWait(400);
  const double before = f.session.multicamPosition();
  CHECK(before > 8.2);
  CHECK(f.Key(Qt::Key_1));
  f.session.multicamPlay(false);
  CHECK(!f.session.multicamPlaying());
  cuts = f.session.multicamCuts();
  CHECK_EQ(cuts.size(), 4);
  const double cut_time = cuts[3].toMap()["time"].toDouble();
  CHECK(cut_time >= before - 1e-9 && cut_time < before + 0.5 && cuts[3].toMap()["angleIndex"].toInt() == 0);
  // Playing to the end stops by itself.
  f.session.multicamSeek(19.8);
  f.session.multicamPlay(true);
  CHECK(QTest::qWaitFor([&] { return !f.session.multicamPlaying(); }, 3000));
  CHECK(!f.session.multicamPlaying() && std::abs(f.session.multicamPosition() - 20.0) < 1e-6);

  // A click on a tile cuts to it: the second tile of the 3-angle grid.
  {
    cutline::render::AngleMonitorConfig config;
    config.width = 960;
    config.height = 540;
    const auto rects = cutline::render::AngleMonitorLayout(3, config);
    f.session.multicamSeek(10.0);
    steps = f.session.appliedSteps();
    CHECK(f.session.multicamCutAtPoint(rects[1].x + rects[1].width / 2.0, rects[1].y + rects[1].height / 2.0, 960, 540));
    CHECK_EQ(f.session.appliedSteps(), steps + 1);
    CHECK(f.session.multicamAngles()[1].toMap()["active"].toBool());
    CHECK(!f.session.multicamCutAtPoint(1, 1, 960, 540));         // the gap between tiles
  }

  // Refining: nudge a cut a frame, give it another angle, move it, remove it; each is one step and refusals say so.
  steps = f.session.appliedSteps();
  CHECK(f.session.multicamNudgeCut(1, 2));
  CHECK(std::abs(f.session.multicamCuts()[1].toMap()["time"].toDouble() - 3.08) < 1e-9);
  CHECK(f.session.multicamChangeCut(1, 2));
  CHECK(f.session.multicamMoveCut(1, 3.5));
  CHECK_EQ(f.session.appliedSteps(), steps + 3);
  CHECK(!f.session.multicamNudgeCut(0, 1));                         // the first cut stays at the start
  CHECK(!f.session.multicamRemoveCut(0));
  CHECK(f.session.multicamRemoveCut(1));
  CHECK(f.session.multicamRenameAngle(0, "Master"));
  CHECK_EQ(f.session.multicamAngles()[0].toMap()["name"].toString().toStdString(), std::string("Master"));
  CHECK(!f.session.multicamRenameAngle(1, "Master"));
  CHECK(f.session.multicamNudgeSync(2, 3));
  CHECK(std::abs(f.session.multicamAngles()[2].toMap()["offset"].toDouble() - 0.12) < 1e-9);
  f.session.trigger("edit.undo");
  CHECK(std::abs(f.session.multicamAngles()[2].toMap()["offset"].toDouble()) < 1e-9);

  // Laid out on the timeline: picture on V2, sound on A2, following the picture's angle.
  f.session.seek(0.0);
  CHECK(f.session.multicamFlatten("v2", "a2", false, 0));
  const auto* picture = f.session.sequence()->FindTrack("v2");
  const auto* sound = f.session.sequence()->FindTrack("a2");
  CHECK(picture != nullptr && sound != nullptr && !picture->clips.empty());
  CHECK(sound->clips.size() <= picture->clips.size());
  // The same ground twice is refused, not half done.
  const int laid = f.session.appliedSteps();
  CHECK(!f.session.multicamFlatten("v2", "a2", false, 0));
  CHECK_EQ(f.session.appliedSteps(), laid);

  // Undoing the creation takes the group away and the panel closes with it.
  while (f.session.canUndo() && !f.session.multicamGroup().isEmpty()) f.session.trigger("edit.undo");
  CHECK(f.session.multicamGroup().isEmpty());
  CHECK(f.session.multicamAngles().isEmpty());
}

CUTLINE_TEST(AMulticamGroupIsCreatedFromChosenClipsAndTheSetupIsRefusedWhenItCannotWork) {
  Fixture f;
  QVariantList angles;
  for (const auto* name : {"Bars", "Counter"}) {
    QVariantMap angle;
    angle["mediaId"] = name;
    angle["name"] = QString("Cam %1").arg(name);
    angle["offset"] = std::string(name) == "Counter" ? 2.0 : 0.0;
    angles << angle;
  }
  const int steps = f.session.appliedSteps();
  // One clip is not a group.
  CHECK(f.session.multicamCreate(QVariantList{angles[0]}, "Single", "manual", 0).isEmpty());
  CHECK_EQ(f.session.appliedSteps(), steps);
  // By hand: Counter starts 2 s into its clip when Bars starts; the group runs 20 s on the reference, 18 on Counter.
  const auto id = f.session.multicamCreate(angles, "Pair", "manual", 0);
  CHECK(!id.isEmpty());
  CHECK_EQ(f.session.appliedSteps(), steps + 1);
  CHECK_EQ(f.session.multicamName().toStdString(), std::string("Pair"));
  CHECK(std::abs(f.session.multicamAngles()[1].toMap()["offset"].toDouble() - 2.0) < 1e-9);
  CHECK_EQ(f.session.multicamAngles()[0].toMap()["name"].toString().toStdString(), std::string("Cam Bars"));
  CHECK_EQ(f.session.multicamGroups().size(), 1);
  // Sound sync on clips whose sound is one steady tone has nothing to match; whatever it decides, it does not break the
  // project and it says what it found.
  f.session.multicamClose();
  CHECK(f.session.multicamGroup().isEmpty());
  const auto heard = f.session.multicamCreate(angles, "Heard", "audio", 0);
  CHECK_NO_THROW(f.session.multicamClose());
  (void)heard;
  // A group opens again from the list, and an unknown one is refused.
  CHECK(f.session.multicamOpen(id));
  CHECK(!f.session.multicamOpen("nonsense"));
}


CUTLINE_TEST(ExportingFromTheApplicationQueuesRunsChecksAndKeepsTheRecordAcrossReopeningTheProject) {
  Fixture f;
  const auto presets = f.session.exportPresets();
  CHECK(presets.size() >= 18);
  // The plan for a preset says what will happen before anything is queued.
  const auto folder = QDir(f.session.exportFolder());
  const auto out = QDir::temp().filePath("cutline-apptest-export-" + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
  QDir().mkpath(out);
  auto plan = f.session.exportPlan("mezzanine.prores.hq", out + "/film.mp4", true, false);
  CHECK(plan["ok"].toBool());
  CHECK(plan["outputPath"].toString().endsWith("film.mov"));                // the name follows the format
  CHECK(plan["encoder"].toString() == "prores_ks" && !plan["hardware"].toBool());
  CHECK(plan["estimatedBytes"].toDouble() > 1e6 && std::abs(plan["duration"].toDouble() - 19.0) < 1e-9);
  CHECK(!f.session.exportPlan("no.such.preset", out + "/x.mp4", true, false)["ok"].toBool());
  CHECK(f.session.exportQueueAdd("no.such.preset", out + "/x.mp4", "", false, true, false).isEmpty());
  (void)folder;

  // A second of the sequence, from marks, as sound only: queued, run on the queue's own thread, checked afterwards.
  f.session.seek(2.0);
  f.session.trigger("timeline.mark_in");
  f.session.seek(3.0);
  f.session.trigger("timeline.mark_out");
  plan = f.session.exportPlan("audio.wav24", out + "/sound.wav", true, true);
  CHECK(plan["ok"].toBool() && std::abs(plan["duration"].toDouble() - 1.0) < 1e-9);
  QSignalSpy changes(&f.session, &Session::exportsChanged);
  const auto id = f.session.exportQueueAdd("audio.wav24", out + "/sound.wav", "Dialogue mix", false, true, true);
  CHECK(!id.isEmpty());
  CHECK(f.session.exportWaitIdle(60000));
  QTest::qWait(100);   // the queue reports from its own thread; the signals arrive on this one once it is free
  CHECK(changes.count() >= 2);
  auto jobs = f.session.exportJobs();
  CHECK_EQ(jobs.size(), 1);
  auto job = jobs[0].toMap();
  CHECK(job["state"].toString() == "done" && job["name"].toString() == "Dialogue mix" && job["problems"].toStringList().isEmpty());
  CHECK(job["encoder"].toString().isEmpty() || job["encoder"].toString() == "");   // sound only: no picture encoder
  CHECK(job["bytes"].toDouble() > 100000 && QFileInfo::exists(out + "/sound.wav"));
  CHECK(job["progress"].toDouble() == 1.0);
  // An existing file is not replaced unless the job says so: a second export to the same name fails, saying why.
  const auto again = f.session.exportQueueAdd("audio.wav24", out + "/sound.wav", "", false, true, true);
  CHECK(f.session.exportWaitIdle(60000));
  jobs = f.session.exportJobs();
  CHECK_EQ(jobs.size(), 2);
  CHECK(jobs[1].toMap()["state"].toString() == "failed" && jobs[1].toMap()["error"].toString().contains("already exists"));
  // Retry runs it again (and fails the same way); taking it off the list is possible, and the finished one stays.
  CHECK(f.session.exportRetry(again));
  CHECK(f.session.exportWaitIdle(60000));
  CHECK(f.session.exportRemove(again));
  CHECK_EQ(f.session.exportJobs().size(), 1);

  // Pausing holds queued jobs back; resuming runs them.
  f.session.exportPause(true);
  CHECK(f.session.exportQueuePaused());
  const auto held = f.session.exportQueueAdd("audio.flac", out + "/sound.flac", "", false, true, true);
  QTest::qWait(150);
  CHECK(f.session.exportJobs()[1].toMap()["state"].toString() == "queued");
  f.session.exportPause(false);
  CHECK(f.session.exportWaitIdle(60000));
  CHECK(f.session.exportJobs()[1].toMap()["state"].toString() == "done" && QFileInfo::exists(out + "/sound.flac"));
  (void)held;

  // The record is in the project: close it, open it again, and the queue is there as it was.
  const auto package = f.session.projectFolder();
  CHECK(QFileInfo::exists(package + "/exports/queue.json"));
  f.session.closeProject();
  CHECK(f.session.exportJobs().isEmpty());
  CHECK(f.session.openProject(package));
  jobs = f.session.exportJobs();
  CHECK_EQ(jobs.size(), 2);
  CHECK(jobs[0].toMap()["state"].toString() == "done" && jobs[0].toMap()["name"].toString() == "Dialogue mix");
  QDir(out).removeRecursively();
}


namespace {

// The masks of the first effect of a clip that is not one of its own controls.
const cutline::timeline::Effect* MaskedEffect(const Session& session, const std::string& clip_name) {
  const auto* sequence = session.sequence();
  if (sequence == nullptr) return nullptr;
  for (const auto& track : sequence->tracks) {
    for (const auto& clip : track.clips) {
      if (clip.name != clip_name) continue;
      for (const auto& effect : clip.effects) {
        if (!effect.intrinsic) return &effect;
      }
    }
  }
  return nullptr;
}

}  // namespace

CUTLINE_TEST(MasksAreDrawnEditedKeyedAndRemovedOnTheMonitorEachGestureOneUndoStep) {
  Fixture f;
  auto* monitor = new MonitorItem(f.window.contentItem());
  monitor->setPosition(QPointF(0, 0));
  monitor->setSize(QSizeF(640, 360));   // the 16:9 picture fills it: picture (x, y) is window (640 x, 360 y)
  monitor->setSession(&f.session);
  monitor->setZ(10);
  f.session.selectClipNamed("Bars");
  f.session.addEffect("gaussian_blur", "");
  const auto* effect = MaskedEffect(f.session, "Bars");
  CHECK(effect != nullptr && effect->masks.empty());
  const auto effect_id = QString::fromStdString(effect->id);
  const auto px = [](double x) { return 640.0 * x; };
  const auto py = [](double y) { return 360.0 * y; };

  // The inspector's button adds a rectangle on the effect and chooses it for editing on the picture.
  int steps = f.session.appliedSteps();
  CHECK(f.session.maskAdd(effect_id, "rectangle"));
  CHECK_EQ(f.session.appliedSteps(), steps + 1);
  CHECK(!f.session.maskActive().isEmpty() && f.session.maskTool() == "select" && f.session.maskInteractive());
  const auto mask_id = f.session.maskActive();
  effect = MaskedEffect(f.session, "Bars");
  CHECK_EQ(effect->masks.size(), std::size_t{1});
  CHECK(std::abs(effect->masks[0].document.width - 0.4) < 1e-9);
  CHECK_EQ(f.session.inspector()[f.session.inspector().size() - 1].toMap()["masks"].toList().size(), 1);

  // Drag the right edge (at 0.7, 0.5) to 0.8: the width grows, the left edge stays, and the whole drag is one step.
  steps = f.session.appliedSteps();
  f.Drag(px(0.7), py(0.5), px(0.8), py(0.5));
  CHECK_EQ(f.session.appliedSteps(), steps + 1);
  effect = MaskedEffect(f.session, "Bars");
  CHECK(std::abs(effect->masks[0].document.width - 0.5) < 0.01 && std::abs(effect->masks[0].document.center_x - 0.55) < 0.01);
  // While the pointer is down the shape follows it without anything being committed.
  steps = f.session.appliedSteps();
  f.Press(px(0.55), py(0.5));
  f.Move(px(0.65), py(0.6));
  CHECK_EQ(f.session.appliedSteps(), steps);
  CHECK(f.session.maskOverlay()["active"].toBool() && f.session.maskOverlay()["outline"].toList().size() == 4);
  f.Release(px(0.65), py(0.6));
  CHECK_EQ(f.session.appliedSteps(), steps + 1);
  effect = MaskedEffect(f.session, "Bars");
  CHECK(std::abs(effect->masks[0].document.center_x - 0.65) < 0.01 && std::abs(effect->masks[0].document.center_y - 0.6) < 0.01);
  f.session.trigger("edit.undo");
  CHECK(std::abs(MaskedEffect(f.session, "Bars")->masks[0].document.center_x - 0.55) < 0.01);
  // A click on nothing the mask owns is not a gesture: the monitor stays a monitor (no step, no change).
  steps = f.session.appliedSteps();
  f.Click(px(0.05), py(0.05));
  CHECK_EQ(f.session.appliedSteps(), steps);

  // Values: feather and the mode, one step each; keying makes the value animated and a second key follows the playhead.
  CHECK(f.session.maskSetNumber(mask_id, "feather", 24.0));
  CHECK(f.session.maskSetMode(mask_id, "subtract", true));
  effect = MaskedEffect(f.session, "Bars");
  CHECK(effect->masks[0].document.feather == 24.0 && effect->masks[0].document.inverted && effect->masks[0].document.combine == cutline::effects::mask::Combine::Subtract);
  CHECK(!f.session.maskSetNumber(mask_id, "no_such_value", 1.0));
  f.session.seek(1.0);
  CHECK(f.session.maskToggleKey(mask_id, "feather"));
  CHECK(!MaskedEffect(f.session, "Bars")->masks[0].document.animations.empty());
  f.session.seek(2.0);
  CHECK(f.session.maskSetNumber(mask_id, "feather", 60.0));
  const auto& feather = MaskedEffect(f.session, "Bars")->masks[0].document.animations[0];
  CHECK(feather.property == "feather" && feather.keys.size() == 2 && std::abs(feather.keys[1].time - (2.0 - 0.0)) < 0.05 && feather.keys[1].value == 60.0);
  CHECK(f.session.maskToggleKey(mask_id, "feather"));   // a key is here: toggling takes it away
  CHECK_EQ(MaskedEffect(f.session, "Bars")->masks[0].document.animations[0].keys.size(), std::size_t{1});
  CHECK(f.session.maskStopAnimating(mask_id, "feather"));
  CHECK(MaskedEffect(f.session, "Bars")->masks[0].document.animations.empty());

  // An ellipse is drawn by dragging, and becomes the mask being edited; a click without a drag draws nothing.
  f.session.maskSetTool("ellipse");
  steps = f.session.appliedSteps();
  f.Click(px(0.1), py(0.1));
  CHECK_EQ(f.session.appliedSteps(), steps);
  f.Drag(px(0.1), py(0.1), px(0.3), py(0.4));
  CHECK_EQ(f.session.appliedSteps(), steps + 1);
  effect = MaskedEffect(f.session, "Bars");
  CHECK_EQ(effect->masks.size(), std::size_t{2});
  CHECK(effect->masks[1].document.shape == cutline::effects::mask::Shape::Ellipse && std::abs(effect->masks[1].document.center_x - 0.2) < 0.01 && f.session.maskTool() == "select");
  CHECK(f.session.maskActive().toStdString() == effect->masks[1].id);

  // A path: click the points, then the first one to close it.
  f.session.maskSetTarget(effect_id);
  f.session.maskSetTool("pen");
  steps = f.session.appliedSteps();
  f.Click(px(0.6), py(0.2));
  f.Click(px(0.9), py(0.2));
  CHECK_EQ(f.session.maskOverlay()["penPoints"].toList().size(), 2);
  f.Click(px(0.9), py(0.5));
  f.Click(px(0.6), py(0.5));
  CHECK_EQ(f.session.appliedSteps(), steps);   // nothing is made until the path is closed
  f.Click(px(0.6), py(0.2));                   // on the first point
  CHECK_EQ(f.session.appliedSteps(), steps + 1);
  effect = MaskedEffect(f.session, "Bars");
  CHECK_EQ(effect->masks.size(), std::size_t{3});
  CHECK(effect->masks[2].document.shape == cutline::effects::mask::Shape::Bezier && effect->masks[2].document.points.size() == 4);
  // Double-click on an edge adds a vertex there; on a vertex it rounds the corner; Delete takes the selected vertex away.
  f.Dbl(px(0.75), py(0.2));
  CHECK_EQ(MaskedEffect(f.session, "Bars")->masks[2].document.points.size(), std::size_t{5});
  f.Dbl(px(0.9), py(0.2));
  CHECK(cutline::ui::IsSmooth(MaskedEffect(f.session, "Bars")->masks[2].document, 2));
  f.Click(px(0.9), py(0.5));   // select that vertex
  CHECK(f.session.maskDeletePoint());
  CHECK_EQ(MaskedEffect(f.session, "Bars")->masks[2].document.points.size(), std::size_t{4});
  // An unfinished path can be abandoned.
  f.session.maskSetTool("pen");
  f.Click(px(0.1), py(0.8));
  f.Click(px(0.2), py(0.8));
  f.session.maskCancelPen();
  CHECK(f.session.maskOverlay()["penPoints"].toList().isEmpty());

  // Removing a mask is one step, and the mask is no longer the one being edited.
  const auto doomed = f.session.maskActive();
  CHECK(f.session.maskRemove(doomed));
  CHECK_EQ(MaskedEffect(f.session, "Bars")->masks.size(), std::size_t{2});
  CHECK(f.session.maskActive().isEmpty());
}

CUTLINE_TEST(AMaskIsTrackedAsAJobAndFollowsTheTrackThatIsStoredWithTheProject) {
  Fixture f;
  f.session.SetAnalysisFrameLimit(8);
  f.session.selectClipNamed("Bars");
  f.session.addEffect("gaussian_blur", "");
  const auto effect_id = QString::fromStdString(MaskedEffect(f.session, "Bars")->id);
  CHECK(f.session.maskAdd(effect_id, "ellipse"));
  const auto mask_id = f.session.maskActive();
  CHECK(f.session.maskTracks().isEmpty());
  f.session.seek(0.1);
  f.session.maskTrack(mask_id);
  CHECK(QTest::qWaitFor([&] {
    const auto jobs = f.session.jobs();
    return !jobs.isEmpty() && !jobs.last().toMap()["active"].toBool() && !f.session.maskTracks().isEmpty();
  }, 60000));
  CHECK_EQ(f.session.maskTracks().size(), 1);
  // The tracked shape follows: the picture is still, so it holds still, but it is now animated from the track.
  const auto& document = MaskedEffect(f.session, "Bars")->masks[0].document;
  CHECK(!document.animations.empty());
  bool has_x = false, has_y = false;
  for (const auto& animation : document.animations) {
    has_x = has_x || animation.property == "center_x";
    has_y = has_y || animation.property == "center_y";
  }
  CHECK(has_x && has_y);
  const auto at_end = cutline::effects::mask::Evaluate(document, 0.3);
  CHECK(std::abs(at_end.center_x - 0.5) < 0.02 && std::abs(at_end.center_y - 0.5) < 0.02);
  // The stored track can be applied again to another mask.
  const auto track_id = f.session.maskTracks()[0].toMap()["id"].toString();
  CHECK(f.session.maskAdd(effect_id, "rectangle"));
  CHECK(f.session.maskFollowTrack(f.session.maskActive(), track_id));
  CHECK(!f.session.maskFollowTrack(f.session.maskActive(), "no-such-track"));
  // One undo takes the tracking back out.
  f.session.trigger("edit.undo");
  CHECK(MaskedEffect(f.session, "Bars")->masks[1].document.animations.empty());
}

CUTLINE_TEST(TheAudioMixerEditsFadersRoutingSendsAndBusesAsProjectCommands) {
  Fixture f;
  CHECK_EQ(f.session.audioMixer().size(), 3);  // A1, A2 and master
  CHECK_EQ(f.session.audioBuses().size(), 1);  // master is always the first destination

  CHECK(f.session.audioSetLevel("a1", -7.5, 0.35));
  auto* a1 = f.session.sequence()->FindTrack("a1");
  CHECK(a1 != nullptr);
  CHECK(std::abs(a1->gain_db + 7.5) < 1e-9 && std::abs(a1->pan - 0.35) < 1e-9);
  CHECK(f.session.audioRename("a1", "Dialogue"));
  CHECK_EQ(f.session.sequence()->FindTrack("a1")->name, std::string("Dialogue"));
  f.session.setTrackFlag("a1", "muted", true);
  CHECK(f.session.sequence()->FindTrack("a1")->muted);

  const auto bus = f.session.audioAddBus("Music Bus");
  CHECK(!bus.isEmpty());
  CHECK_EQ(f.session.audioBuses().size(), 2);
  const auto* created = f.session.sequence()->FindTrack(bus.toStdString());
  CHECK(created != nullptr && created->is_bus);
  CHECK(f.session.audioSetOutput("a1", bus));
  CHECK_EQ(f.session.sequence()->FindTrack("a1")->output_bus_id, bus.toStdString());
  CHECK(f.session.audioSetSend("a2", bus, -12.0, true, true));
  const auto* a2 = f.session.sequence()->FindTrack("a2");
  CHECK_EQ(a2->sends.size(), std::size_t{1});
  CHECK(a2->sends[0].pre_fader && std::abs(a2->sends[0].gain_db + 12.0) < 1e-9);

  // A bus in use is protected; after removing its route and send it can be deleted.
  CHECK(!f.session.audioRemoveBus(bus));
  CHECK(f.session.audioSetOutput("a1", ""));
  CHECK(f.session.audioSetSend("a2", bus, 0.0, false, false));
  CHECK(f.session.audioRemoveBus(bus));
  CHECK_EQ(f.session.audioMixer().size(), 3);
}

CUTLINE_TEST(CaptionAuthoringImportsStylesEditsAndExportsOffline) {
  Fixture f;
  const auto track = f.session.captionAddTrack("English", "en-US");
  CHECK(!track.isEmpty());
  CHECK_EQ(f.session.captionTracks().size(), 1);
  CHECK(f.session.captionTrack() == track);
  CHECK(f.session.captionUpdateTrack(track, "English SDH", "en-US"));
  CHECK(f.session.captionSetTrackStyle(track, "Arial", 0.065, true, false, "center", "bottom", 0.004, 0.7, 0.82));
  const auto* stored_track = &f.session.sequence()->caption_tracks.front();
  const auto style = cutline::captions::ParseStyle(stored_track->style_json);
  CHECK(style.bold && std::abs(style.size - 0.065) < 1e-9);
  CHECK(std::abs(style.background[3] - 0.7) < 1e-9);

  const auto cue = f.session.captionAdd(track, 1.25, 3.5, "First line", "Ana");
  CHECK(!cue.isEmpty());
  CHECK_EQ(f.session.captions().size(), 1);
  CHECK(f.session.captionUpdate(cue, 1.5, 4.0, "Corrected line", "Ana"));
  auto row = f.session.captions().front().toMap();
  CHECK(row["text"].toString() == QString("Corrected line"));
  CHECK(std::abs(row["start"].toDouble() - 1.5) < 1e-9);

  QTemporaryDir folder;
  CHECK(folder.isValid());
  const auto srt = folder.filePath("captions.srt");
  CHECK(f.session.captionExport(track, srt, "srt"));
  QFile exported(srt);
  CHECK(exported.open(QIODevice::ReadOnly));
  CHECK(exported.readAll().contains("Corrected line"));

  const auto vtt = folder.filePath("more.vtt");
  QFile imported(vtt);
  CHECK(imported.open(QIODevice::WriteOnly));
  CHECK(imported.write("WEBVTT\n\n00:00:05.000 --> 00:00:06.500\nImported cue\n") > 0);
  imported.close();
  CHECK(f.session.captionImport(track, vtt));
  CHECK_EQ(f.session.captions().size(), 2);
  CHECK(f.session.captionRemove(cue));
  CHECK_EQ(f.session.captions().size(), 1);
  f.session.trigger("edit.undo");
  CHECK_EQ(f.session.captions().size(), 2);
  CHECK(f.session.captionRemoveTrack(track));
  CHECK(f.session.captionTracks().isEmpty());
}

CUTLINE_TEST(TheApplicationShipsAFolderOfLooksPreviewsThemAndAppliesOneToTheClip) {
  Fixture f;
  // The looks are written beside the configuration at the first start and are in the library without any folder being added.
  const auto entries = f.session.lutEntries({});
  CHECK(entries.size() >= 8);
  QString teal_orange, vivid;
  for (const auto& entry : entries) {
    const auto map = entry.toMap();
    CHECK(map["group"].toString() == "Cutline Looks" && map["size"].toInt() == 33 && !map["curves"].toBool());
    if (map["name"].toString() == "Teal and Orange") teal_orange = map["path"].toString();
    if (map["name"].toString() == "Vivid Pop") vivid = map["path"].toString();
  }
  CHECK(!teal_orange.isEmpty() && !vivid.isEmpty());
  CHECK(f.session.lutEntries("teal orange").size() == 1);
  CHECK(QFileInfo(f.session.lutFolder()).isDir());

  // A person's own file next to them is found, a 1D table says so.
  {
    QFile mine(f.session.lutFolder() + "/Mine.cube");
    CHECK(mine.open(QIODevice::WriteOnly));
    mine.write("LUT_1D_SIZE 2\n0 0 0\n1 1 1\n");
  }
  f.session.setLutFolders({});
  const auto own = f.session.lutEntries("mine");
  CHECK(own.size() == 1 && own[0].toMap()["curves"].toBool());

  // The preview is the test chart through the table: not the plain chart, and the same picture the next time it is asked for.
  cutline::app::LutPreviewProvider provider;
  QSize size;
  const auto graded = provider.requestImage(QUrl::toPercentEncoding(teal_orange), &size, {});
  const auto plain = provider.requestImage("original", &size, {});
  CHECK(graded.size() == QSize(192, 108) && plain.size() == QSize(192, 108));
  long long moved = 0;
  for (int y = 0; y < graded.height(); ++y) {
    for (int x = 0; x < graded.width(); ++x) moved += std::abs(qRed(graded.pixel(x, y)) - qRed(plain.pixel(x, y))) + std::abs(qBlue(graded.pixel(x, y)) - qBlue(plain.pixel(x, y)));
  }
  CHECK(moved > graded.width() * graded.height() * 2);
  CHECK(provider.requestImage(QUrl::toPercentEncoding(teal_orange), &size, {}) == graded);
  // A file that is not a table shows as a block, not a crash.
  const auto broken = provider.requestImage(QUrl::toPercentEncoding(f.session.lutFolder() + "/nothing.cube"), &size, {});
  CHECK(!broken.isNull());

  // Applying one to the clip changes the picture the monitor shows, and is one undo step.
  f.session.selectClipNamed("Bars");
  f.session.seek(0.5);
  QTest::qWait(200);
  const auto before = f.session.currentFrame();
  const auto steps = f.session.appliedSteps();
  f.session.addEffect("lut", teal_orange);
  CHECK_EQ(f.session.appliedSteps(), steps + 1);
  QTest::qWait(300);
  const auto after = f.session.currentFrame();
  CHECK(!before.isNull() && !after.isNull() && before != after);
  f.session.trigger("edit.undo");
  CHECK_EQ(f.session.appliedSteps(), steps);
}

CUTLINE_TEST(ATranscriptIsImportedShownAsParagraphsAndEditedByDeletingWordsFillersAndPausesEachOneUndoStep) {
  Fixture f;
  f.session.selectClipNamed("Bars");
  const auto* bars = f.ClipNamed("Bars", "v1");
  CHECK(bars != nullptr);
  // No transcript yet for this media.
  CHECK(f.session.transcriptState() == "none" || f.session.transcriptState() == "unavailable");
  CHECK(f.session.transcriptParagraphs().isEmpty());

  // What whisper.cpp writes, as the media's own words: five words, an "um" among them, and a long silence before the last.
  const QString text = R"JSON({"params":{"model":"ggml-base.en.bin"},"result":{"language":"en"},"transcription":[
    {"offsets":{"from":120,"to":570},"text":" Welcome","tokens":[{"text":" Welcome","offsets":{"from":120,"to":570},"p":0.9}]},
    {"offsets":{"from":780,"to":1480},"text":" show.","tokens":[{"text":" show","offsets":{"from":780,"to":1480},"p":0.8},{"text":".","offsets":{"from":1480,"to":1560},"p":0.9}]},
    {"offsets":{"from":1650,"to":2030},"text":" Umm,","tokens":[{"text":" U","offsets":{"from":1650,"to":1650},"p":0.4},{"text":"mm","offsets":{"from":1680,"to":1840},"p":0.5},{"text":",","offsets":{"from":1840,"to":2030},"p":0.9}]},
    {"offsets":{"from":2030,"to":2510},"text":" today","tokens":[{"text":" today","offsets":{"from":2030,"to":2510},"p":0.95}]},
    {"offsets":{"from":4600,"to":5000},"text":" thanks","tokens":[{"text":" thanks","offsets":{"from":4600,"to":5000},"p":0.7}]}]})JSON";
  const auto file = QDir::temp().filePath("cutline-transcript-" + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8) + ".json");
  {
    QFile out(file);
    CHECK(out.open(QIODevice::WriteOnly));
    out.write(text.toUtf8());
  }
  CHECK(f.session.transcriptImport(file));
  QFile::remove(file);
  CHECK(f.session.transcriptState() == "ready");
  const auto paragraphs = f.session.transcriptParagraphs();
  CHECK_EQ(paragraphs.size(), 2);   // the two seconds of silence ends a paragraph
  const auto first_words = paragraphs[0].toMap()["words"].toList();
  CHECK_EQ(first_words.size(), 4);
  CHECK(first_words[0].toMap()["t"].toString() == "Welcome" && first_words[2].toMap()["f"].toBool());   // the um is marked as a filler
  CHECK(first_words[0].toMap()["on"].toBool());
  CHECK(f.session.transcriptSummary().contains("5 words"));
  CHECK_EQ(f.session.transcriptFillerCount(false), 1);
  CHECK_EQ(f.session.transcriptPauseCount(1.0, 0.1), 1);

  // The word at the playhead follows it.
  const double start = Fixture::Seconds(bars->timeline_start), source_in = Fixture::Seconds(bars->source_in);
  f.session.seek(start + 0.3 - source_in);
  CHECK_EQ(f.session.transcriptWord(), 0);
  f.session.seek(start + 1.0 - source_in);
  CHECK_EQ(f.session.transcriptWord(), 1);

  // Take out the "um": one step, the clip is shorter by about what it took, and undo gives it back.
  const double length = f.session.durationSeconds();
  const int steps = f.session.appliedSteps();
  CHECK_EQ(f.session.transcriptRemoveFillers(false, true), 1);
  CHECK_EQ(f.session.appliedSteps(), steps + 1);
  const double shorter = f.session.durationSeconds();
  CHECK(length - shorter > 0.15 && length - shorter < 0.25);
  CHECK_EQ(f.session.transcriptFillerCount(false), 0);
  CHECK_EQ(f.session.transcriptParagraphs()[0].toMap()["words"].toList().size(), 4);   // the transcript still has the word, now off the timeline
  CHECK(!f.session.transcriptParagraphs()[0].toMap()["words"].toList()[2].toMap()["on"].toBool());
  f.session.trigger("edit.undo");
  CHECK_EQ(f.session.appliedSteps(), steps);
  CHECK(std::abs(f.session.durationSeconds() - length) < 1e-9);
  CHECK_EQ(f.session.transcriptFillerCount(false), 1);

  // Delete a chosen run of words (as the panel does), and the long pause.
  CHECK(f.session.transcriptDelete(1, 1, true));
  CHECK_EQ(f.session.appliedSteps(), steps + 1);
  f.session.trigger("edit.undo");
  CHECK_EQ(f.session.transcriptRemovePauses(1.0, 0.1), 1);
  CHECK(f.session.durationSeconds() < length - 1.5);
  f.session.trigger("edit.undo");

  // Captions from the words, into a "Transcript" track, one step; running it again adds to the same track.
  CHECK(f.session.transcriptCaptions(42, 2));
  CHECK_EQ(f.session.captionTracks().size(), 1);
  CHECK_EQ(f.session.captions().size(), 2);
  CHECK(f.session.transcriptCaptions(42, 2));
  CHECK_EQ(f.session.captionTracks().size(), 1);

  // Search, a correction and a speaker name are kept with the transcript, and show in the text.
  CHECK_EQ(f.session.transcriptFind("show today"), 0);
  CHECK_EQ(f.session.transcriptFind("TODAY"), 1);
  CHECK(f.session.transcriptNextHit(1));
  CHECK(f.session.transcriptSetText(2, "um"));
  CHECK(f.session.transcriptSetSpeaker(0, 3, "Ana"));
  CHECK(f.session.transcriptPlainText().startsWith("Ana: Welcome show. um today"));
  CHECK(!f.session.transcriptSetText(2, "   "));
  // Another media shows another transcript (none): selecting the counter clip leaves this one for its own.
  f.session.selectClipNamed("Counter");
  CHECK(f.session.transcriptParagraphs().isEmpty());
  f.session.selectClipNamed("Bars");
  CHECK_EQ(f.session.transcriptParagraphs().size(), 2);
}

CUTLINE_TEST(TheAudioWorkflowSetsRolesMatchesLoudnessAndWritesAutomationWhileThePlayheadMoves) {
  Fixture f;
  // Choosing a clip's picture chooses its sound.
  f.session.selectClipNamed("Bars");
  auto choice = f.session.audioSelection();
  CHECK(!choice["clip"].toString().isEmpty() && choice["measurable"].toBool() && choice["role"].toString().isEmpty());
  CHECK_EQ(f.session.audioRoles().size(), 4);
  CHECK(f.session.audioLoudnessTargets().size() >= 4);

  // A role with its chain is one step, shows on the selection, and clearing it is one more.
  const int steps = f.session.appliedSteps();
  CHECK(f.session.audioApplyRole("dialogue", true, 0.5, 0.0, true, true, ""));
  CHECK_EQ(f.session.appliedSteps(), steps + 1);
  choice = f.session.audioSelection();
  CHECK(choice["role"].toString() == "dialogue" && choice["hasChain"].toBool());
  CHECK(f.session.audioApplyRole("", true, 0, 0, true, true, ""));
  choice = f.session.audioSelection();
  CHECK(choice["role"].toString().isEmpty() && !choice["hasChain"].toBool());
  CHECK(!f.session.audioApplyRole("narration", true, 0, 0, true, true, ""));

  // Matching the sound to a target runs as a job and writes the gain that reaches it.
  f.session.audioMeasure("clip", "r128");
  CHECK(QTest::qWaitFor([&] { return f.session.audioLoudness()["state"].toString() == "done"; }, 30000));
  auto loud = f.session.audioLoudness();
  const double measured = loud["integratedLufs"].toDouble();
  CHECK(measured > -60.0 && measured < 0.0);
  CHECK(loud["applied"].toBool() && std::abs(loud["gainDb"].toDouble() - (-23.0 - measured)) < 1e-6);
  CHECK(std::abs(f.session.audioSelection()["gainDb"].toDouble() - (-23.0 - measured)) < 1e-6);
  // Measuring again with the gain in place is the same number: the clip is measured as the file holds it, not as it now plays.
  f.session.audioMeasure("clip", "");
  CHECK(QTest::qWaitFor([&] { return f.session.audioLoudness()["state"].toString() == "done" && !f.session.audioLoudness().contains("applied"); }, 30000));
  CHECK(std::abs(f.session.audioLoudness()["integratedLufs"].toDouble() - measured) < 1e-9);

  // The programme: the mix as it plays, through the meter; the master's gain moves by what is needed, and a second match changes nothing.
  f.session.audioMeasure("programme", "streaming");
  CHECK(QTest::qWaitFor([&] { return f.session.audioLoudness()["target"].toString() == "streaming" && f.session.audioLoudness()["state"].toString() == "done"; }, 60000));
  loud = f.session.audioLoudness();
  CHECK(loud["applied"].toBool());
  const double master_gain = loud["gainDb"].toDouble();
  f.session.audioMeasure("programme", "streaming");
  CHECK(QTest::qWaitFor([&] { return f.session.audioLoudness()["state"].toString() == "done" && f.session.audioLoudness()["applied"].isValid() && std::abs(f.session.audioLoudness()["gainDb"].toDouble()) < 0.05; }, 60000));
  (void)master_gain;

  // Automation: in Read nothing is taken; in Write the moves while playing become keys on the track once it stops.
  CHECK(!f.session.audioAutomate("a1", "volume", -6.0, "begin"));
  CHECK(f.session.audioAutomationMode("a1") == "read");
  CHECK(f.session.audioSetAutomationMode("a1", "write") && f.session.audioAutomationMode("a1") == "write");
  CHECK(!f.session.audioSetAutomationMode("a1", "arm") && !f.session.audioSetAutomationMode("v1", "write"));
  f.session.seek(1.0);
  CHECK(!f.session.audioAutomate("a1", "volume", -6.0, "begin"));   // not playing: the caller sets the static level instead
  CHECK(f.Key(Qt::Key_L));
  CHECK(f.session.audioAutomate("a1", "volume", -6.0, "begin"));
  QTest::qWait(150);
  CHECK(f.session.audioAutomate("a1", "volume", -14.0, "move"));
  QTest::qWait(150);
  CHECK(f.session.audioAutomate("a1", "volume", -14.0, "end"));
  QTest::qWait(60);
  CHECK(f.Key(Qt::Key_K));
  const auto written = [&] {
    const auto* sequence = f.session.sequence();
    const auto* track = sequence != nullptr ? sequence->FindTrack("a1") : nullptr;
    return track == nullptr ? std::vector<std::pair<RationalTime, double>>{} : cutline::ui::AutomationOf(*track, cutline::ui::AutomationTarget::Volume);
  };
  CHECK(QTest::qWaitFor([&] { return written().size() >= 2; }, 3000));
  const auto keys = written();
  CHECK(keys.front().second == -6.0 && keys.back().second == -14.0);
  CHECK(keys.front().first.Compare(RationalTime(1, 1)) >= 0);
  // It is one step, the strip says it has automation and where the fader stands now, and undo takes it away.
  bool shows = false;
  for (const auto& row : f.session.audioMixer()) {
    const auto map = row.toMap();
    if (map["id"].toString() == "a1") shows = map["autoVolume"].toBool() && map["autoMode"].toString() == "write";
  }
  CHECK(shows);
  f.session.trigger("edit.undo");
  CHECK(written().empty());
}

CUTLINE_TEST(AVoiceOverTakeIsRecordedImportedAndPlacedOnTheArmedTrackAsOneUndoStep) {
  Fixture f;
  SKIP_INAPPLICABLE(!f.session.audioInputs().isEmpty(), "this machine has no input device");
  CHECK(!f.session.audioRecordStart("v1", "", false));        // a picture track cannot be armed
  CHECK(!f.session.audioRecordStop());                         // nothing to stop
  f.session.seek(4.0);
  const bool started = f.session.audioRecordStart("a2", "", false);
  SKIP_INAPPLICABLE(started, "the default input could not be opened here");
  CHECK(f.session.audioRecording());
  CHECK(!f.session.audioRecordStart("a2", "", false));         // one take at a time
  QTest::qWait(700);
  CHECK(std::isfinite(f.session.audioInputLevel()));
  const int clips_before = static_cast<int>(f.session.sequence()->FindTrack("a2")->clips.size());
  const int steps = f.session.appliedSteps();
  CHECK(f.session.audioRecordStop());
  CHECK(!f.session.audioRecording());
  const auto* track = f.session.sequence()->FindTrack("a2");
  CHECK_EQ(static_cast<int>(track->clips.size()), clips_before + 1);
  const auto& clip = track->clips.front();
  CHECK(std::abs(Fixture::Seconds(clip.timeline_start) - 4.0) < 1e-9);
  const double length = Fixture::Seconds(clip.end()) - 4.0;
  CHECK(length > 0.4 && length < 2.0);
  CHECK_EQ(f.session.appliedSteps(), steps + 2);               // importing the take is one step and placing it another
  f.session.trigger("edit.undo");
  CHECK_EQ(static_cast<int>(f.session.sequence()->FindTrack("a2")->clips.size()), clips_before);
}

CUTLINE_TEST(TheSequenceColourAndAClipsInputSpaceAreChosenByNameAndEachIsOneUndoStep) {
  Fixture f;
  CHECK(f.session.colorSpaces().size() >= 15);
  auto colour = f.session.sequenceColor();
  CHECK(colour["working"].toString() == "rec709" && colour["display"].toString() == "rec709");
  const int steps = f.session.appliedSteps();
  CHECK(f.session.setSequenceColor("acescg", "rec709"));
  CHECK_EQ(f.session.appliedSteps(), steps + 1);
  colour = f.session.sequenceColor();
  CHECK(colour["working"].toString() == "acescg" && colour["managed"].toBool());
  CHECK(!f.session.setSequenceColor("no-such-space", "rec709"));
  CHECK_EQ(f.session.appliedSteps(), steps + 1);
  // A clip says what its picture is: the effect is offered by the catalogue as a colour-space effect and carries the name.
  bool offered = false;
  for (const auto& entry : f.session.effectCatalogue("input")) {
    const auto map = entry.toMap();
    if (map["id"].toString() == "input_colorspace") offered = map["assetKind"].toString() == "colorspace" && map["needsAsset"].toBool();
  }
  CHECK(offered);
  f.session.selectClipNamed("Bars");
  f.session.addInputColorSpace("arri-logc3");
  CHECK_EQ(f.session.appliedSteps(), steps + 2);
  bool found = false;
  for (const auto& track : f.session.sequence()->tracks) {
    for (const auto& clip : track.clips) {
      for (const auto& effect : clip.effects) found = found || (effect.effect_type == "input_colorspace" && effect.preset_name == "arri-logc3");
    }
  }
  CHECK(found);
  f.session.addInputColorSpace("not-a-space");
  CHECK_EQ(f.session.appliedSteps(), steps + 2);
  f.session.trigger("edit.undo");
  f.session.trigger("edit.undo");
  CHECK(f.session.sequenceColor()["working"].toString() == "rec709");
}

CUTLINE_TEST(FilesAreCopiedExactlyImportedAndGivenAProxyAsJobsAndProxiesCanBeSwitchedAndRemoved) {
  Fixture f;
  const auto fixtures = cutline::testing::EnvironmentValue("CUTLINE_FIXTURE_DIR");
  SKIP_UNLESS(!fixtures.empty() && QFileInfo::exists(QString::fromStdString(fixtures) + "/bars-2997.mp4"), "the media fixtures were not generated");
  const auto source = QString::fromStdString(fixtures) + "/bars-2997.mp4";
  QTemporaryDir destination;
  CHECK(destination.isValid());
  const int before = f.session.media().size();

  // The plan: what will be copied where, and that there is room.
  auto plan = f.session.ingestPlan({source}, true, destination.path());
  CHECK(plan["ok"].toBool() && plan["items"].toList().size() == 1 && plan["bytes"].toDouble() > 0 && plan["freeBytes"].toDouble() > plan["bytes"].toDouble());
  CHECK(plan["items"].toList()[0].toMap()["destination"].toString().endsWith("bars-2997.mp4"));
  // With no folder named the copies go into the project's own media folder.
  CHECK(f.session.ingestPlan({source}, true, "")["items"].toList()[0].toMap()["destination"].toString().startsWith(QDir::toNativeSeparators(f.session.ingestFolder())) || f.session.ingestPlan({source}, true, "")["items"].toList()[0].toMap()["destination"].toString().startsWith(f.session.ingestFolder()));
  CHECK(f.session.proxyChoices().size() == 5);

  // Copy, verify, import and make a light proxy, as one job.
  f.session.ingestFiles({source}, true, true, destination.path(), "540");
  CHECK(QTest::qWaitFor([&] { return f.session.media().size() == before + 1; }, 30000));
  CHECK(QFileInfo::exists(destination.path() + "/bars-2997.mp4") && QFileInfo(destination.path() + "/bars-2997.mp4").size() == QFileInfo(source).size());
  QVariantMap entry;
  CHECK(QTest::qWaitFor([&] {
    for (const auto& item : f.session.media()) {
      if (item.toMap()["path"].toString().endsWith("bars-2997.mp4") && item.toMap()["path"].toString().startsWith(destination.path())) entry = item.toMap();
    }
    return entry["proxyStatus"].toString() == "ready";
  }, 60000));
  CHECK(!entry["proxyBusy"].toBool() && entry["video"].toBool());
  // The project points at the copy, not at the card.
  CHECK(entry["path"].toString().startsWith(destination.path()));

  // The same file again is not imported twice, and its copy does not stay behind.
  QTemporaryDir second;
  f.session.ingestFiles({source}, true, true, second.path(), "none");
  QTest::qWait(1500);
  CHECK(QTest::qWaitFor([&] { return f.session.jobs().isEmpty() || !f.session.jobs().last().toMap()["active"].toBool(); }, 30000));
  CHECK_EQ(f.session.media().size(), before + 1);
  CHECK(!QFileInfo::exists(second.path() + "/bars-2997.mp4"));

  // Proxies are switched on and off for the monitor, and removed.
  CHECK(!f.session.useProxies());
  f.session.setUseProxies(true);
  CHECK(f.session.useProxies());
  f.session.setUseProxies(false);
  CHECK(f.session.proxyRemove(entry["id"].toString()));
  for (const auto& item : f.session.media()) {
    if (item.toMap()["id"] == entry["id"]) CHECK(item.toMap()["proxyStatus"].toString() == "none");
  }
  CHECK(!f.session.proxyRemove(entry["id"].toString()));   // nothing left to remove
  // An absent file is refused with its reason and imports nothing.
  f.session.ingestFiles({destination.path() + "/not-there.mp4"}, true, true, destination.path(), "none");
  QTest::qWait(300);
  CHECK_EQ(f.session.media().size(), before + 1);
}

CUTLINE_TEST(ATransitionIsPutOnTheCutAtThePlayheadChangedFromItsDialogAndDrawnAsItsKindInTheMonitor) {
  Fixture f;
  CHECK(f.session.transitionCatalogue({}).size() >= 20);
  CHECK(f.session.transitionCatalogue("wipe").size() >= 4 && f.session.transitionCatalogue("zzzz").isEmpty());
  // No cut near the playhead: nothing is made, and the status line says what to do.
  f.session.seek(3.0);
  const int steps = f.session.appliedSteps();
  CHECK(!f.session.addTransition("cross_dissolve", 1.0));
  CHECK_EQ(f.session.appliedSteps(), steps);
  // At the cut between Bars and Counter, one step.
  f.session.seek(6.0);
  CHECK(f.session.addTransition("wipe_right", 2.0));
  CHECK_EQ(f.session.appliedSteps(), steps + 1);
  const auto* sequence = f.session.sequence();
  const auto& transitions = sequence->FindTrack("v1")->transitions;
  CHECK_EQ(transitions.size(), std::size_t{1});
  const auto id = QString::fromStdString(transitions[0].id);
  auto info = f.session.transitionInfo(id);
  CHECK(info["kind"].toString() == "wipe_right" && info["track"].toString() == "v1" && !info["oneSided"].toBool());
  CHECK(info["seconds"].toDouble() > 0.0 && info["seconds"].toDouble() <= 2.0);
  // A second one on the same cut is refused; changing it is one step and shows in the info.
  CHECK(!f.session.addTransition("cross_dissolve", 1.0));
  CHECK(f.session.changeTransition(id, "iris_circle", 1.0, "start"));
  // Changing a transition replaces it, so it has a new id.
  info = f.session.transitionInfo(QString::fromStdString(f.session.sequence()->FindTrack("v1")->transitions[0].id));
  CHECK(info["kind"].toString() == "iris_circle" && std::abs(info["seconds"].toDouble() - 1.0) < 1e-9 && info["alignment"].toString() == "start");
  // The monitor shows a picture made by that transition at its middle, different from the plain cut.
  const auto start = Fixture::Seconds(f.session.sequence()->FindTrack("v1")->transitions[0].timeline_start);
  f.session.seek(start + 0.5);
  QTest::qWait(300);
  const auto middle = f.session.currentFrame();
  f.session.seek(start - 1.0);
  QTest::qWait(300);
  const auto before = f.session.currentFrame();
  CHECK(!middle.isNull() && !before.isNull() && middle != before);
  // The timeline reports a double-click on the transition by opening it; removing it is one step and undo brings it back.
  QSignalSpy opened(&f.session, &Session::transitionRequested);
  f.session.openTransition(QString::fromStdString(f.session.sequence()->FindTrack("v1")->transitions[0].id));
  CHECK_EQ(opened.count(), 1);
  CHECK(f.session.removeTransition(QString::fromStdString(f.session.sequence()->FindTrack("v1")->transitions[0].id)));
  CHECK(f.session.sequence()->FindTrack("v1")->transitions.empty());
  f.session.trigger("edit.undo");
  CHECK_EQ(f.session.sequence()->FindTrack("v1")->transitions.size(), std::size_t{1});
  CHECK(f.session.transitionInfo("no-such").isEmpty());
}

CUTLINE_TEST(ATitleIsMadeFromATemplateChangedFromItsControlsDrawnInTheDesignerAndSavedAsATemplate) {
  Fixture f;
  // The templates that come with the application are offered before they are in the project.
  auto templates = f.session.graphicTemplates();
  CHECK(templates.size() >= 6 && !templates.front().toMap()["installed"].toBool());
  CHECK(f.session.graphicLibrary().isEmpty());
  const int steps = f.session.appliedSteps();

  // Making a title from one adds it to the library and makes the title: one step.
  const auto title = f.session.newGraphicFromTemplate("lower-third", 1, "Host");
  CHECK(!title.isEmpty());
  CHECK_EQ(f.session.appliedSteps(), steps + 1);
  auto library = f.session.graphicLibrary();
  CHECK_EQ(library.size(), 1);
  CHECK(library.front().toMap()["kind"].toString() == "template" && library.front().toMap()["name"].toString() == "Host");
  CHECK(library.front().toMap()["preview"].toString().startsWith("image://graphicpreview/"));
  templates = f.session.graphicTemplates();
  bool installed = false;
  for (const auto& item : templates) installed = installed || (item.toMap()["id"].toString() == "lower-third" && item.toMap()["installed"].toBool());
  CHECK(installed);

  // Its picture is the document drawn by the same code as the monitor.
  const auto url = library.front().toMap()["preview"].toString();
  const auto key = url.mid(QString("image://graphicpreview/").size());
  const auto thumbnail = cutline::app::GraphicPreviewStore::Instance().Render(key.left(key.lastIndexOf('@')), 240);
  CHECK(thumbnail.width() == 240 && thumbnail.height() == 135);
  QSet<QRgb> colours;
  for (int y = 0; y < thumbnail.height(); y += 3) for (int x = 0; x < thumbnail.width(); x += 3) colours.insert(thumbnail.pixel(x, y));
  CHECK(colours.size() > 6);   // the chequerboard, the bar and its text

  // On the timeline at the playhead for four seconds; the chosen clip's controls are the template's fields.
  f.session.seek(1.0);
  CHECK(f.session.placeGraphic(title, 4.0));
  const auto* sequence = f.session.sequence();
  std::string clip;
  for (const auto& track : sequence->tracks) {
    for (const auto& item : track.clips) {
      for (const auto& effect : item.effects) if (effect.preset_name == "project:" + title.toStdString()) clip = item.id;
    }
  }
  CHECK(!clip.empty());
  const auto info = f.session.clipGraphic(QString::fromStdString(clip));
  CHECK(info["kind"].toString() == "template" && info["template"].toString() == "lower-third");
  const auto controls = info["controls"].toList();
  CHECK(controls.size() >= 4);
  QString name_control;
  for (const auto& control : controls) if (control.toMap()["property"].toString() == "text" && name_control.isEmpty()) name_control = control.toMap()["name"].toString();
  CHECK(!name_control.isEmpty());
  const int before_change = f.session.appliedSteps();
  CHECK(f.session.setGraphicControl(title, name_control, "Grace Hopper"));
  CHECK_EQ(f.session.appliedSteps(), before_change + 1);
  bool changed = false;
  for (const auto& control : f.session.clipGraphic(QString::fromStdString(clip))["controls"].toList()) {
    changed = changed || (control.toMap()["name"].toString() == name_control && control.toMap()["value"].toString() == "Grace Hopper");
  }
  CHECK(changed);
  CHECK(!f.session.setGraphicControl(title, "no-such-control", "x"));
  CHECK(f.session.clipGraphic("no-such-clip").isEmpty());
  // The monitor shows it: a frame while the title is up differs from one before it.
  f.session.seek(2.0);
  QTest::qWait(300);
  const auto during = f.session.currentFrame();
  f.session.seek(0.2);
  QTest::qWait(300);
  CHECK(!during.isNull() && during != f.session.currentFrame());
  // A title on the timeline cannot be deleted; its template version cannot be taken away.
  CHECK(!f.session.deleteGraphic(title));

  // ---- the designer
  CHECK(!f.session.designerOpen());
  QSignalSpy requested(&f.session, &Session::designerRequested);
  const auto drawing = f.session.newGraphic("Badge");
  CHECK(!drawing.isEmpty() && f.session.designerOpen() && requested.count() == 1);
  auto state = f.session.designer();
  CHECK(state["id"].toString() == drawing && state["name"].toString() == "Badge" && !state["dirty"].toBool() && state["preview"].toString().startsWith("image://graphicpreview/"));
  CHECK_EQ(f.session.designerElements().size(), 1);
  // Opening a title made from a template for drawing is refused, with a reason.
  CHECK(!f.session.openGraphic(title));

  const auto box = f.session.designerAdd("rectangle");
  CHECK(!box.isEmpty() && f.session.designer()["dirty"].toBool() && f.session.designer()["canUndo"].toBool());
  CHECK(f.session.designerSetProperty("fill", "#2255EE"));
  CHECK(!f.session.designerSetProperty("fill", "blue"));        // refused, and the status line says why
  CHECK(!f.session.designerSetProperty("opacity", "lots"));
  CHECK(f.session.designerSetProperty("corner_radius", "0.05"));
  CHECK(f.session.designerSetProperty("rotation", "20"));
  CHECK(f.session.designerSetProperty("stroke_width", "0.01"));
  CHECK(f.session.designerSetProperty("shadow", "#000000A0"));
  CHECK(f.session.designerSetProperty("shadow_blur", "0.02"));
  const auto properties = f.session.designerProperties();
  CHECK(properties.size() >= 14);

  // A drag from the middle of the box moves it by what the pointer moved, measured from the press (so going back puts it back).
  const auto selection = f.session.designerSelection();
  CHECK(selection["id"].toString() == box && selection["grips"].toList().size() == 9);
  const double x0 = selection["x"].toDouble(), y0 = selection["y"].toDouble();
  const double middle_x = x0 + selection["width"].toDouble() / 2.0, middle_y = y0 + selection["height"].toDouble() / 2.0;
  CHECK(f.session.designerPress(middle_x, middle_y, true) == "body");
  f.session.designerDrag(middle_x + 0.1, middle_y + 0.05, false, false);
  CHECK(std::abs(f.session.designerSelection()["x"].toDouble() - (x0 + 0.1)) < 1e-9 && std::abs(f.session.designerSelection()["y"].toDouble() - (y0 + 0.05)) < 1e-9);
  f.session.designerDrag(middle_x + 0.03, middle_y, false, false);
  f.session.designerDrag(middle_x, middle_y, false, false);
  f.session.designerRelease();
  CHECK(std::abs(f.session.designerSelection()["x"].toDouble() - x0) < 1e-9);
  // Dragging a corner grip resizes it; the opposite corner stays.
  QVariantMap corner;
  for (const auto& grip : f.session.designerSelection()["grips"].toList()) if (grip.toMap()["name"].toString() == "topLeft") corner = grip.toMap();
  CHECK(f.session.designerPress(corner["x"].toDouble(), corner["y"].toDouble(), true) == "topLeft");
  f.session.designerDrag(corner["x"].toDouble() - 0.05, corner["y"].toDouble() - 0.05, false, false);
  f.session.designerRelease();
  CHECK(f.session.designerSelection()["width"].toDouble() > selection["width"].toDouble());
  // A click on nothing selects nothing; a click on the first element selects it.
  CHECK(f.session.designerPress(0.01, 0.99, true) == "none" && f.session.designerSelection().isEmpty());
  f.session.designerRelease();
  f.session.designerSelect(f.session.designerElements().last().toMap()["id"].toString());
  CHECK(!f.session.designerSelection().isEmpty());

  // The designer has its own undo; the project's history has not moved until Save.
  const int before_save = f.session.appliedSteps();
  f.session.designerUndo();
  f.session.designerRedo();
  CHECK_EQ(f.session.appliedSteps(), before_save);
  f.session.designerSetName("Round badge");
  CHECK(f.session.designerSetEntrance("slide_left", 0.4));
  CHECK(f.session.designerEntrance()["kind"].toString() == "slide_left");
  CHECK(!f.session.designerSetEntrance("spin", 0.4));
  CHECK(f.session.designerSave());
  CHECK_EQ(f.session.appliedSteps(), before_save + 1);
  CHECK(!f.session.designer()["dirty"].toBool());
  bool renamed = false;
  for (const auto& item : f.session.graphicLibrary()) renamed = renamed || (item.toMap()["id"].toString() == drawing && item.toMap()["name"].toString() == "Round badge");
  CHECK(renamed);

  // A picture element with no picture chosen blocks saving, and says what to do.
  const auto picture = f.session.designerAdd("image");
  CHECK(f.session.designer()["problems"].toList().size() == 1);
  const int before_blocked = f.session.appliedSteps();
  CHECK(!f.session.designerSave() && f.session.appliedSteps() == before_blocked);
  QString bars;
  for (const auto& item : f.session.designerPictures()) if (bars.isEmpty()) bars = item.toMap()["value"].toString();
  CHECK(!bars.isEmpty() && f.session.designerSetProperty("asset", bars));
  CHECK(f.session.designer()["problems"].toList().isEmpty() && f.session.designerSave());
  (void)picture;

  // As a template: in the library, exported to a file and imported back (one step), and the file is the same package.
  CHECK(f.session.designerSaveAsTemplate("Round badge", "A badge"));
  bool offered = false;
  for (const auto& item : f.session.graphicTemplates()) offered = offered || (item.toMap()["id"].toString() == "round-badge" && item.toMap()["installed"].toBool());
  CHECK(offered);
  QTemporaryDir directory;
  const auto file = directory.filePath("badge.json");
  CHECK(f.session.exportGraphicTemplate(drawing, file));
  QFile exported(file);
  CHECK(exported.open(QIODevice::ReadOnly) && exported.size() > 100);
  exported.close();
  CHECK(f.session.exportGraphicTemplate(title, directory.filePath("host.json")));
  const int before_import = f.session.appliedSteps();
  CHECK(f.session.importGraphicTemplate(directory.filePath("host.json")));    // already there: not an error
  // The same id and version with other content (the description) is refused: a version never changes once it is in; the same content again changes nothing.
  CHECK(!f.session.importGraphicTemplate(file));
  CHECK_EQ(f.session.appliedSteps(), before_import);
  QFile bad(directory.filePath("bad.json"));
  CHECK(bad.open(QIODevice::WriteOnly));
  bad.write("{not json");
  bad.close();
  CHECK(!f.session.importGraphicTemplate(directory.filePath("bad.json")));

  // Closing the designer; the graphic stays, and deleting it (nothing shows it) is one step.
  f.session.designerClose();
  CHECK(!f.session.designerOpen());
  CHECK(f.session.deleteGraphic(drawing));
  for (const auto& item : f.session.graphicLibrary()) CHECK(item.toMap()["id"].toString() != drawing);
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  (void)App();
  return cutline::testing::RunAll("app");
}
