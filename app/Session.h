#pragma once

// The application's one live editing session: the open project, the sequence being edited, the playback engine and
// the monitor's presenter, the transport and selection, the preferences, shortcuts and jobs. It is the only thing
// the interface talks to. Each action arrives as an application command id (the ones in ui/Shortcuts.h) or as a
// call from a panel, becomes an EditPlan from the editing rules, and is run as one undoable group on the project.
//
// Qt appears here and in the items, and nowhere in the logic underneath, which is tested without a window.

#include "audio/AudioCapture.h"
#include "audio/AudioSink.h"
#include "core/project/ProjectStore.h"
#include "playback/PlaybackEngine.h"
#include "render/FlowCache.h"
#include "render/Scopes.h"
#include "timeline/Sequence.h"
#include "ui/EditPlanner.h"
#include "ui/Inspector.h"
#include "ui/Jobs.h"
#include "exporter/ExportPresets.h"
#include "exporter/ExportQueue.h"
#include "timeline/Multicam.h"
#include "ui/Layout.h"
#include "ui/MaskEditor.h"
#include "ui/LookPack.h"
#include "ui/LutLibrary.h"
#include "ui/Monitor.h"
#include "ui/Multicam.h"
#include "ui/Preferences.h"
#include "ui/Ramp.h"
#include "speech/Transcript.h"
#include "speech/Whisper.h"
#include "ui/AudioWorkflow.h"
#include "ui/Shortcuts.h"
#include "ui/TextEdit.h"
#include "ui/TimelineView.h"
#include "ui/Transport.h"

#include <QImage>
#include <QObject>
#include <QtQml/qqmlregistration.h>
#include <QString>
#include <QStringList>
#include <QElapsedTimer>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace cutline::app {

class Session : public QObject {
  Q_OBJECT
  QML_NAMED_ELEMENT(Session)
  QML_UNCREATABLE("There is one session, made by the application")
  Q_PROPERTY(bool projectOpen READ projectOpen NOTIFY projectChanged)
  Q_PROPERTY(QString projectName READ projectName NOTIFY projectChanged)
  Q_PROPERTY(double playhead READ playheadSeconds NOTIFY playheadChanged)
  Q_PROPERTY(QString timecode READ timecode NOTIFY playheadChanged)
  Q_PROPERTY(double duration READ durationSeconds NOTIFY sequenceChanged)
  Q_PROPERTY(double frameRate READ frameRate NOTIFY sequenceChanged)
  Q_PROPERTY(bool playing READ playing NOTIFY transportChanged)
  Q_PROPERTY(double shuttleRate READ shuttleRate NOTIFY transportChanged)
  Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
  Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
  Q_PROPERTY(QStringList history READ history NOTIFY historyChanged)
  Q_PROPERTY(int appliedSteps READ appliedSteps NOTIFY historyChanged)
  Q_PROPERTY(QString tool READ tool WRITE setTool NOTIFY toolChanged)
  Q_PROPERTY(bool snap READ snap WRITE setSnap NOTIFY snapChanged)
  Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusChanged)
  Q_PROPERTY(QVariantList media READ media NOTIFY mediaChanged)
  Q_PROPERTY(QVariantList inspector READ inspector NOTIFY inspectorChanged)
  Q_PROPERTY(QString inspectorClip READ inspectorClip NOTIFY inspectorChanged)
  Q_PROPERTY(QVariantList jobs READ jobs NOTIFY jobsChanged)
  Q_PROPERTY(double markIn READ markIn NOTIFY marksChanged)
  Q_PROPERTY(double markOut READ markOut NOTIFY marksChanged)
  Q_PROPERTY(bool loop READ loop WRITE setLoop NOTIFY transportChanged)
  Q_PROPERTY(QString monitorQuality READ monitorQuality WRITE setMonitorQuality NOTIFY monitorChanged)
  Q_PROPERTY(bool safeMargins READ safeMargins WRITE setSafeMargins NOTIFY monitorChanged)
  Q_PROPERTY(QString renderInfo READ renderInfo NOTIFY frameReady)
  Q_PROPERTY(QString gpuInfo READ gpuInfo NOTIFY frameReady)
  Q_PROPERTY(QVariantList audioMixer READ audioMixer NOTIFY audioMixerChanged)
  Q_PROPERTY(QVariantList audioBuses READ audioBuses NOTIFY audioMixerChanged)
  Q_PROPERTY(QString scopeMode READ scopeMode WRITE setScopeMode NOTIFY scopeChanged)
  Q_PROPERTY(QVariantList exportJobs READ exportJobs NOTIFY exportsChanged)
  Q_PROPERTY(bool useProxies READ useProxies WRITE setUseProxies NOTIFY mediaChanged)
  Q_PROPERTY(QVariantMap audioSelection READ audioSelection NOTIFY selectionChanged)
  Q_PROPERTY(QVariantMap audioLoudness READ audioLoudness NOTIFY audioLoudnessChanged)
  Q_PROPERTY(bool audioRecording READ audioRecording NOTIFY audioMixerChanged)
  Q_PROPERTY(double audioInputLevel READ audioInputLevel NOTIFY audioMixerChanged)
  Q_PROPERTY(QString transcriptMedia READ transcriptMedia NOTIFY transcriptChanged)
  Q_PROPERTY(QString transcriptName READ transcriptName NOTIFY transcriptChanged)
  Q_PROPERTY(QString transcriptState READ transcriptState NOTIFY transcriptChanged)
  Q_PROPERTY(QString transcriptSummary READ transcriptSummary NOTIFY transcriptChanged)
  Q_PROPERTY(QString transcriptEngine READ transcriptEngine NOTIFY transcriptChanged)
  Q_PROPERTY(QVariantList transcriptParagraphs READ transcriptParagraphs NOTIFY transcriptChanged)
  Q_PROPERTY(QVariantList transcriptSpeakers READ transcriptSpeakers NOTIFY transcriptChanged)
  Q_PROPERTY(int transcriptWord READ transcriptWord NOTIFY transcriptWordChanged)
  Q_PROPERTY(QVariantList transcriptHits READ transcriptHits NOTIFY transcriptHitsChanged)
  Q_PROPERTY(QVariantList captionTracks READ captionTracks NOTIFY captionsChanged)
  Q_PROPERTY(QString captionTrack READ captionTrack WRITE captionSelectTrack NOTIFY captionsChanged)
  Q_PROPERTY(QVariantList captions READ captions NOTIFY captionsChanged)
  Q_PROPERTY(QString maskTool READ maskTool NOTIFY maskChanged)
  Q_PROPERTY(QString maskActive READ maskActive NOTIFY maskChanged)
  Q_PROPERTY(bool maskInteractive READ maskInteractive NOTIFY maskChanged)
  Q_PROPERTY(QVariantList maskTracks READ maskTracks NOTIFY maskChanged)
  Q_PROPERTY(bool exportQueuePaused READ exportQueuePaused NOTIFY exportsChanged)
  Q_PROPERTY(int selectionCount READ selectionCount NOTIFY selectionChanged)
  Q_PROPERTY(QVariantList multicamGroups READ multicamGroups NOTIFY multicamChanged)
  Q_PROPERTY(QString multicamGroup READ multicamGroup NOTIFY multicamChanged)
  Q_PROPERTY(QString multicamName READ multicamName NOTIFY multicamChanged)
  Q_PROPERTY(QString multicamSync READ multicamSync NOTIFY multicamChanged)
  Q_PROPERTY(QVariantList multicamAngles READ multicamAngles NOTIFY multicamPositionChanged)
  Q_PROPERTY(QVariantList multicamCuts READ multicamCuts NOTIFY multicamChanged)
  Q_PROPERTY(double multicamPosition READ multicamPosition NOTIFY multicamPositionChanged)
  Q_PROPERTY(int graphicsRevision READ graphicsRevision NOTIFY graphicsChanged)
  Q_PROPERTY(int designerRevision READ designerRevision NOTIFY designerChanged)
  Q_PROPERTY(bool designerOpen READ designerOpen NOTIFY designerChanged)
  Q_PROPERTY(double multicamDuration READ multicamDuration NOTIFY multicamChanged)
  Q_PROPERTY(bool multicamPlaying READ multicamPlaying NOTIFY multicamPositionChanged)
  Q_PROPERTY(QString rampClip READ rampClip NOTIFY rampChanged)
  Q_PROPERTY(QVariantList rampSegments READ rampSegments NOTIFY rampChanged)
  Q_PROPERTY(QVariantList rampGraph READ rampGraph NOTIFY rampChanged)
  Q_PROPERTY(double rampDuration READ rampDuration NOTIFY rampChanged)
  Q_PROPERTY(QString rampProblem READ rampProblem NOTIFY rampChanged)
  Q_PROPERTY(bool rampDirty READ rampDirty NOTIFY rampChanged)
  Q_PROPERTY(QVariantList rampExtent READ rampExtent NOTIFY rampChanged)
  Q_PROPERTY(double rampClipStart READ rampClipStart NOTIFY rampChanged)
  Q_PROPERTY(bool rampLive READ rampLive WRITE setRampLive NOTIFY rampChanged)

 public:
  explicit Session(QObject* parent = nullptr);
  ~Session() override;

  // ---- what the interface reads
  [[nodiscard]] bool projectOpen() const { return store_ != nullptr; }
  [[nodiscard]] QString projectName() const;
  [[nodiscard]] double playheadSeconds() const;
  [[nodiscard]] QString timecode() const;
  [[nodiscard]] double durationSeconds() const;
  [[nodiscard]] double frameRate() const;
  [[nodiscard]] bool playing() const { return transport_.playing(); }
  [[nodiscard]] double shuttleRate() const { return transport_.rate(); }
  [[nodiscard]] bool canUndo() const { return store_ != nullptr && store_->CanUndo(); }
  [[nodiscard]] bool canRedo() const { return store_ != nullptr && store_->CanRedo(); }
  [[nodiscard]] QStringList history() const;
  [[nodiscard]] int appliedSteps() const;
  [[nodiscard]] QString tool() const { return tool_; }
  void setTool(const QString& tool);
  [[nodiscard]] bool snap() const { return prefs_.GetBool("timeline.snap"); }
  void setSnap(bool on);
  [[nodiscard]] QString statusMessage() const { return status_; }
  [[nodiscard]] QVariantList media() const { return media_; }
  [[nodiscard]] QVariantList inspector() const { return inspector_; }
  [[nodiscard]] QString inspectorClip() const { return inspector_clip_; }
  [[nodiscard]] QVariantList jobs() const;
  [[nodiscard]] double markIn() const { return mark_in_ ? Seconds(*mark_in_) : -1.0; }
  [[nodiscard]] double markOut() const { return mark_out_ ? Seconds(*mark_out_) : -1.0; }
  [[nodiscard]] bool loop() const { return transport_.looping(); }
  void setLoop(bool on);
  [[nodiscard]] QString monitorQuality() const { return QString::fromStdString(ui::ToString(quality_)); }
  void setMonitorQuality(const QString& quality);
  [[nodiscard]] bool safeMargins() const { return prefs_.GetBool("monitor.show_safe_margins"); }
  void setSafeMargins(bool on);
  [[nodiscard]] QString renderInfo() const { return render_info_; }
  // Which adapter the monitor is composed on and how its frames have been made, or why it is software ("GPU: ...").
  [[nodiscard]] QString gpuInfo() const;
  [[nodiscard]] QVariantList audioMixer() const;
  [[nodiscard]] QVariantList audioBuses() const;
  [[nodiscard]] QString scopeMode() const { return scope_mode_; }
  void setScopeMode(const QString& mode);
  [[nodiscard]] const std::optional<render::AsyncScopeResult>& scopeResult() const { return scope_result_; }
  [[nodiscard]] int selectionCount() const { return static_cast<int>(selection_.clips().size()); }

  // ---- for the items that draw and edit (same thread: the interface's)
  [[nodiscard]] const timeline::Sequence* sequence() const { return graph_.root(); }
  [[nodiscard]] ui::Selection& selection() { return selection_; }
  [[nodiscard]] const ui::Preferences& prefs() const { return prefs_; }
  [[nodiscard]] ui::Preferences& prefs() { return prefs_; }
  [[nodiscard]] ui::Keymap& keymap() { return keymap_; }
  [[nodiscard]] const ui::CommandRegistry& commands() const { return ui::BuiltInCommands(); }
  [[nodiscard]] ui::JobTracker& job_tracker() { return jobs_; }
  [[nodiscard]] time::RationalTime playheadTime() const { return transport_.position(); }
  [[nodiscard]] ui::EditContext EditContextFor();
  [[nodiscard]] const std::vector<time::RationalTime>& markers() const { return markers_; }
  [[nodiscard]] std::optional<time::RationalTime> markInTime() const { return mark_in_; }
  [[nodiscard]] std::optional<time::RationalTime> markOutTime() const { return mark_out_; }
  [[nodiscard]] QImage currentFrame() const { return frame_; }
  [[nodiscard]] QSize currentFrameSequenceSize() const;
  // Runs a plan as one undoable step; returns whether it happened, telling the status line why not.
  bool Apply(const ui::EditPlan& plan);
  void NotifySelectionChanged();
  // The whole of a media item placed at a time on the given tracks (either may be empty).
  [[nodiscard]] ui::EditPlan InsertPlanAt(const std::string& media_id, const time::RationalTime& at, const std::string& video_track, const std::string& audio_track, bool insert);
  [[nodiscard]] ui::EditPlan InsertPlanFor(const std::string& media_id, bool insert);
  void SeekTo(const time::RationalTime& to);
  // The monitor says how large the picture is on screen, in device pixels, so Auto quality can pick a render size.
  void SetMonitorDisplay(int width, int height);
  void ShowStatus(const QString& message);

  // ---- what the interface calls
  Q_INVOKABLE bool newProject(const QString& folder_url, const QString& name);
  Q_INVOKABLE bool newDemoProject();
  Q_INVOKABLE bool openProject(const QString& folder_url);
  Q_INVOKABLE void closeProject();
  Q_INVOKABLE void trigger(const QString& command_id);
  Q_INVOKABLE void importMedia(const QStringList& urls);
  Q_INVOKABLE void seek(double seconds);
  Q_INVOKABLE void insertMedia(const QString& media_id, bool insert);
  Q_INVOKABLE void selectMedia(const QString& media_id);
  Q_INVOKABLE void setClipSpeed(double percent, bool reversed, bool maintain_pitch);
  Q_INVOKABLE void setTrackFlag(const QString& track_id, const QString& flag, bool on);
  Q_INVOKABLE bool audioSetLevel(const QString& track_id, double gain_db, double pan);
  Q_INVOKABLE bool audioRename(const QString& track_id, const QString& name);
  Q_INVOKABLE bool audioSetOutput(const QString& track_id, const QString& bus_id);
  Q_INVOKABLE bool audioSetSend(const QString& track_id, const QString& bus_id, double gain_db, bool pre_fader, bool enabled);
  Q_INVOKABLE QString audioAddBus(const QString& name);
  Q_INVOKABLE bool audioRemoveBus(const QString& track_id);
  Q_INVOKABLE QVariantList effectCatalogue(const QString& query) const;
  Q_INVOKABLE QVariantList lutEntries(const QString& query) const;
  Q_INVOKABLE void setLutFolders(const QStringList& folders);
  // Colour spaces by name, grouped for a menu; a clip's input space (over what its file says) and the sequence's working and display spaces.
  Q_INVOKABLE QVariantList colorSpaces() const;
  Q_INVOKABLE void addInputColorSpace(const QString& space);
  Q_INVOKABLE QVariantMap sequenceColor() const;
  Q_INVOKABLE bool setSequenceColor(const QString& working, const QString& display);
  // The folder of look-up tables the application keeps (with its own looks in "Cutline Looks"), empty without a configuration.
  Q_INVOKABLE QString lutFolder() const;
  Q_INVOKABLE void addEffect(const QString& effect_type, const QString& preset);
  Q_INVOKABLE void removeEffect(const QString& effect_id);
  Q_INVOKABLE void setEffectEnabled(const QString& effect_id, bool enabled);
  Q_INVOKABLE void moveEffect(const QString& effect_id, int places);
  Q_INVOKABLE void setParameter(const QString& effect_id, const QString& name, const QVariantList& components);
  Q_INVOKABLE void toggleAnimation(const QString& effect_id, const QString& name);
  Q_INVOKABLE void toggleKeyframe(const QString& effect_id, const QString& name);
  Q_INVOKABLE void resetParameter(const QString& effect_id, const QString& name);
  Q_INVOKABLE void jumpToKeyframe(const QString& effect_id, const QString& name, bool next);
  Q_INVOKABLE bool ctrlHeld() const;
  // Selects the clip with this name (and what is linked to it): for scripted scenes and tests.
  Q_INVOKABLE void selectClipNamed(const QString& name);
  Q_INVOKABLE int markerCount() const { return static_cast<int>(markers_.size()); }
  // Without audio the transport is driven by the clock alone: for tests and machines with no sound device.
  void SetAudioEnabled(bool enabled) { audio_enabled_ = enabled; }
  // Supplies a deterministic sink to playback integration tests.
  void SetAudioSinkForTest(std::unique_ptr<audio::AudioSink> sink) { sink_ = std::move(sink); }
  // The most frames an analysis looks at (a long clip is analysed from its start); tests keep it small.
  void SetAnalysisFrameLimit(std::size_t frames) { analysis_limit_ = frames; }
  // Starts analysis of the selected clip as a background job: stabilize (a keyframed correction) or optical_flow (motion kept for slow motion).
  Q_INVOKABLE void analyseClip(const QString& kind);
  Q_INVOKABLE void cancelJob(int id);
  Q_INVOKABLE void clearFinishedJobs();
  Q_INVOKABLE void undoTo(int step);
  Q_INVOKABLE QVariantList commandList() const;
  Q_INVOKABLE QVariantList searchCommands(const QString& query) const;
  Q_INVOKABLE QString shortcutText(const QString& command_id) const;
  Q_INVOKABLE bool bindShortcut(const QString& command_id, const QString& chord);
  Q_INVOKABLE void resetShortcuts();
  Q_INVOKABLE QVariantList preferenceList() const;
  Q_INVOKABLE bool setPreference(const QString& key, const QVariant& value);
  Q_INVOKABLE void resetPreference(const QString& key);
  // A key press from the window: returns whether it was a shortcut.
  Q_INVOKABLE bool handleKey(int qt_key, int modifiers, const QString& text, bool pressed, bool auto_repeat);
  Q_INVOKABLE void setRenderFrameForTest(const QString& path);
  Q_INVOKABLE bool saveScreenshot(const QString& path);
  // ---- the speed ramp editor: a working copy of the selected clip ramp, edited as a graph and applied as one step.
  [[nodiscard]] QString rampClip() const { return QString::fromStdString(ramp_clip_); }
  [[nodiscard]] double rampClipStart() const;
  [[nodiscard]] QVariantList rampSegments() const;
  [[nodiscard]] QVariantList rampGraph() const;
  [[nodiscard]] double rampDuration() const;
  [[nodiscard]] QString rampProblem() const;
  [[nodiscard]] bool rampDirty() const { return ramp_dirty_; }
  [[nodiscard]] bool rampLive() const { return ramp_live_; }
  void setRampLive(bool live);
  [[nodiscard]] QVariantList rampExtent() const;
  [[nodiscard]] ui::RampModel RampModelFor(const std::string& clip_id) const;
  bool ApplyRamp(const std::string& clip_id, const ui::RampModel& model);
  Q_INVOKABLE bool rampOpen();
  Q_INVOKABLE void rampClose();
  Q_INVOKABLE bool rampSplit(double seconds);
  Q_INVOKABLE bool rampRemoveBoundary(int boundary);
  Q_INVOKABLE bool rampMoveBoundary(int boundary, double seconds);
  Q_INVOKABLE bool rampSetBoundarySpeed(int boundary, double speed, const QString& side);
  Q_INVOKABLE bool rampSetSegmentSpeed(int segment, double start_speed, double end_speed);
  Q_INVOKABLE bool rampFreeze(int segment);
  Q_INVOKABLE bool rampReverse(int segment);
  Q_INVOKABLE bool rampEase(int segment, const QString& ease);
  Q_INVOKABLE bool rampScale(double factor);
  Q_INVOKABLE bool rampSetDuration(double seconds);
  Q_INVOKABLE bool rampApply();
  Q_INVOKABLE bool rampClear();
  Q_INVOKABLE void rampReset();
  Q_INVOKABLE void rampPreview(double ramp_seconds);
  // Between press and release of a drag, edits are held back from the project, so one gesture is one undo step.
  Q_INVOKABLE void rampDrag(bool dragging);

  // ---- multicam: the group that is open, its clock, the angle monitor and the cuts
  [[nodiscard]] QVariantList multicamGroups() const;
  [[nodiscard]] QVariantList multicamCandidates() const;
  [[nodiscard]] QString multicamGroup() const { return QString::fromStdString(mc_id_); }
  [[nodiscard]] QString multicamName() const;
  [[nodiscard]] QString multicamSync() const;
  [[nodiscard]] QVariantList multicamAngles() const;
  [[nodiscard]] QVariantList multicamCuts() const;
  [[nodiscard]] double multicamPosition() const { return Seconds(mc_position_); }
  [[nodiscard]] double multicamDuration() const;
  [[nodiscard]] bool multicamPlaying() const { return mc_playing_; }
  [[nodiscard]] QImage multicamFrame() const { return mc_frame_; }
  // Angles are {mediaId, name, offset (seconds, for manual sync), marker (seconds, for marker sync)}; sync is
  // "manual", "timecode", "marker" or "audio". Returns the new group's id, or empty (and says why in the status line).
  Q_INVOKABLE QString multicamCreate(const QVariantList& angles, const QString& name, const QString& sync, int reference);
  Q_INVOKABLE QVariantList multicamCandidatesList() const { return multicamCandidates(); }
  Q_INVOKABLE bool multicamOpen(const QString& group_id);
  Q_INVOKABLE void multicamClose();
  Q_INVOKABLE void multicamPlay(bool on);
  Q_INVOKABLE void multicamSeek(double seconds);
  Q_INVOKABLE bool multicamCut(int angle_index);
  Q_INVOKABLE bool multicamCutAtPoint(double x, double y, double width, double height);
  Q_INVOKABLE bool multicamMoveCut(int cut, double to_seconds);
  Q_INVOKABLE bool multicamNudgeCut(int cut, int frames);
  Q_INVOKABLE bool multicamChangeCut(int cut, int angle_index);
  Q_INVOKABLE bool multicamRemoveCut(int cut);
  Q_INVOKABLE bool multicamRenameAngle(int angle_index, const QString& name);
  Q_INVOKABLE bool multicamNudgeSync(int angle_index, int frames);
  Q_INVOKABLE bool multicamFlatten(const QString& video_track, const QString& audio_track, bool audio_from_one_angle, int audio_angle);
  Q_INVOKABLE bool multicamDelete();

  // ---- transitions (render/Transitions.h, ui/Transitions.h)
  Q_INVOKABLE QVariantList transitionCatalogue(const QString& query) const;
  // Puts the transition on the cut (or clip edge) nearest the playhead, on the chosen clip's track if one is chosen.
  Q_INVOKABLE bool addTransition(const QString& kind, double seconds);
  // {id, kind, seconds, alignment ("center", "start", "end"), track, oneSided}, empty if there is no such transition.
  Q_INVOKABLE QVariantMap transitionInfo(const QString& id) const;
  Q_INVOKABLE bool changeTransition(const QString& id, const QString& kind, double seconds, const QString& alignment);
  Q_INVOKABLE bool removeTransition(const QString& id);
  Q_INVOKABLE void openTransition(const QString& id);

  // ---- titles and graphics (ui/GraphicsDesigner.h)
  // Counts up whenever the project's graphics may have changed, so a list that asks for them again does so.
  [[nodiscard]] int graphicsRevision() const { return graphics_revision_; }
  // The project's graphics: [{id, name, kind ("graphic" or "template"), templateId, templateVersion, uses (clips showing it), preview (image url)}].
  Q_INVOKABLE QVariantList graphicLibrary();
  // The templates a title can be made from: [{id, version, name, description, installed, preview}], the project's own then the ones that come with the application.
  Q_INVOKABLE QVariantList graphicTemplates();
  // Makes a graphic (an empty one, or from a template with these values) and opens it in the designer when it is plain. Returns its id, or "".
  Q_INVOKABLE QString newGraphic(const QString& name);
  Q_INVOKABLE QString newGraphicFromTemplate(const QString& template_id, int version, const QString& name);
  // Puts the graphic on the timeline at the playhead for this long.
  Q_INVOKABLE bool placeGraphic(const QString& id, double seconds);
  Q_INVOKABLE bool renameGraphic(const QString& id, const QString& name);
  Q_INVOKABLE bool deleteGraphic(const QString& id);
  // Template packages as files (JSON): in, to the project's library; out, a plain graphic made into a package with a control for each thing worth changing, or a template's own package.
  Q_INVOKABLE bool importGraphicTemplate(const QString& url);
  Q_INVOKABLE bool exportGraphicTemplate(const QString& id, const QString& url);
  // The chosen clip's graphic, with its controls when it comes from a template: {graphic, name, kind, template, controls: [{name, label, property, value, minimum, maximum}]}. Empty when the clip shows no graphic.
  Q_INVOKABLE QVariantMap clipGraphic(const QString& clip_id) const;
  Q_INVOKABLE QVariantMap selectedGraphic() const;
  Q_INVOKABLE bool setGraphicControl(const QString& graphic_id, const QString& name, const QString& value);

  // The designer: a graphic open for editing, changed in memory (with its own undo) and saved to the project in one step.
  [[nodiscard]] int designerRevision() const;
  [[nodiscard]] bool designerOpen() const;
  Q_INVOKABLE bool openGraphic(const QString& id);
  Q_INVOKABLE void designerClose();
  // {id, name, width, height, dirty, selected, canUndo, canRedo, problems: [text], missingFonts: [family], preview (image url)}.
  Q_INVOKABLE QVariantMap designer();
  Q_INVOKABLE QVariantList designerElements() const;
  // [{name, label, group, kind, value, minimum, maximum, step, choices}] of the selected element.
  Q_INVOKABLE QVariantList designerProperties() const;
  // {id, x, y, width, height, rotation, grips: [{name, x, y}]} of the selected element, as fractions of the document.
  Q_INVOKABLE QVariantMap designerSelection() const;
  Q_INVOKABLE QVariantList designerPictures() const;
  Q_INVOKABLE QStringList designerFonts() const;
  Q_INVOKABLE void designerSelect(const QString& id);
  Q_INVOKABLE QString designerAdd(const QString& type);
  Q_INVOKABLE void designerRemove();
  Q_INVOKABLE void designerDuplicate();
  Q_INVOKABLE void designerRestack(const QString& where);
  Q_INVOKABLE void designerAlign(const QString& how);
  Q_INVOKABLE bool designerSetProperty(const QString& name, const QString& value);
  Q_INVOKABLE bool designerSetEntrance(const QString& kind, double seconds);
  Q_INVOKABLE QVariantMap designerEntrance() const;
  Q_INVOKABLE bool designerSetSize(int width, int height);
  Q_INVOKABLE void designerSetName(const QString& name);
  // The pointer, as fractions of the canvas: press selects (the selected element's grip first, else the element under it) and
  // starts a drag, which moves, resizes or turns from the press, never accumulating; release ends it. press returns the grip hit ("body", "right", "turn", ... or "none").
  Q_INVOKABLE QString designerPress(double x, double y, bool snap);
  Q_INVOKABLE void designerDrag(double x, double y, bool keep_proportions, bool snap);
  Q_INVOKABLE void designerRelease();
  Q_INVOKABLE QVariantList designerGuides() const;
  Q_INVOKABLE void designerUndo();
  Q_INVOKABLE void designerRedo();
  Q_INVOKABLE bool designerSave();
  // Keeps the graphic as a template in the project's library (and in a file when a url is given), with a control for each thing worth changing.
  Q_INVOKABLE bool designerSaveAsTemplate(const QString& name, const QString& description);

  // ---- ingest and proxies (media/IngestWorkflow.h)
  [[nodiscard]] bool useProxies() const { return prefs_.GetBool("media.use_proxies"); }
  void setUseProxies(bool on);
  // The folder a project keeps its copied media in, and the plan for these files: {ok, problem, bytes, freeBytes, items: [{name,
  // bytes, destination, copy, problem}]}.
  Q_INVOKABLE QString ingestFolder() const;
  Q_INVOKABLE QVariantMap ingestPlan(const QStringList& urls, bool copy, const QString& destination) const;
  // Copies (verified) and imports each file as a background job, then makes the proxy the choice names ("auto", "1080", "720",
  // "540", "none"). A file already in the project is not imported twice, and its copy is removed.
  Q_INVOKABLE void ingestFiles(const QStringList& urls, bool copy, bool verify, const QString& destination, const QString& proxy);
  Q_INVOKABLE void proxyCreate(const QString& media_id, const QString& choice);
  Q_INVOKABLE bool proxyRemove(const QString& media_id);
  Q_INVOKABLE QVariantList proxyChoices() const;

  // ---- the audio workflow: roles, loudness and automation (ui/AudioWorkflow.h)
  // The sound the controls act on: {clip, name, role, track, gainDb, hasChain, measurable}, empty when no sound is chosen.
  [[nodiscard]] QVariantMap audioSelection() const;
  [[nodiscard]] QVariantMap audioLoudness() const { return audio_loudness_; }
  Q_INVOKABLE QVariantList audioRoles() const;
  Q_INVOKABLE QVariantList audioLoudnessTargets() const;
  // Sets the role of the chosen sound (empty clears it) and, with `chain`, lays down the role's effects. One undo step.
  Q_INVOKABLE bool audioApplyRole(const QString& role, bool chain, double repair, double dereverb, bool tone, bool dynamics, const QString& duck_under_track);
  // Measures the loudness of "clip" (the chosen sound as the file holds it) or "programme" (the whole mix, or the marked
  // range) as a cancellable job; with a target id the gain that reaches it is then written (clip Volume, or the master's).
  Q_INVOKABLE void audioMeasure(const QString& scope, const QString& target_id);
  // Fader and pan gestures while the sequence plays (phase "begin", "move" or "end"). Returns whether the move was taken as
  // automation; if not, the caller sets the static level.
  Q_INVOKABLE bool audioAutomate(const QString& track_id, const QString& control, double value, const QString& phase);
  Q_INVOKABLE QString audioAutomationMode(const QString& track_id) const;
  Q_INVOKABLE bool audioSetAutomationMode(const QString& track_id, const QString& mode);

  // Voice-over: the input devices, and a take recorded to the armed track from the playhead (with the sequence playing, if
  // asked, so the performer hears the programme in headphones). Stopping ends the take, imports the file as media and places
  // it on the track as one undo step. Input monitoring and latency compensation are not done.
  [[nodiscard]] bool audioRecording() const { return recorder_ != nullptr; }
  [[nodiscard]] double audioInputLevel() const { return recorder_ != nullptr ? recorder_->level_db() : -200.0; }
  Q_INVOKABLE QVariantList audioInputs() const;
  Q_INVOKABLE bool audioRecordStart(const QString& track_id, const QString& device_id, bool play);
  Q_INVOKABLE bool audioRecordStop();

  // ---- text-based editing: the words of the media under the playhead or selection, as an editing surface
  [[nodiscard]] QString transcriptMedia() const { return QString::fromStdString(tx_media_); }
  [[nodiscard]] QString transcriptName() const;
  // "none" (no transcript yet), "ready", "working" (a job is making one), "unavailable" (no engine and no transcript).
  [[nodiscard]] QString transcriptState() const;
  [[nodiscard]] QString transcriptSummary() const { return tx_summary_; }
  // What would make a transcript: "whisper.cpp ggml-base.en", or empty when the engine is not on this machine.
  [[nodiscard]] QString transcriptEngine() const;
  // Paragraphs of {start (seconds), speaker, words: [{i, t, f (filler), on (heard on the timeline), part, low (unsure), k (speaker index)}]}.
  [[nodiscard]] QVariantList transcriptParagraphs() const { return tx_paragraphs_; }
  [[nodiscard]] QVariantList transcriptSpeakers() const;
  // The word being said at the playhead, as an index into the transcript, or -1.
  [[nodiscard]] int transcriptWord() const { return tx_word_; }
  [[nodiscard]] QVariantList transcriptHits() const { return tx_hits_; }
  // Makes the transcript of the media as a background job (cancellable in the jobs panel); the sidecar file keeps it.
  Q_INVOKABLE void transcribe();
  // Reads a transcript made elsewhere: whisper.cpp's JSON output, or a transcript file Cutline wrote.
  Q_INVOKABLE bool transcriptImport(const QString& path);
  // Deletes the words first..last (indices into the transcript) from the timeline, as one undo step; ripple closes the gap.
  Q_INVOKABLE bool transcriptDelete(int first, int last, bool ripple);
  // Deletes the sounds of hesitation (and, if asked for, the phrases that do the same job); returns how many were removed.
  Q_INVOKABLE int transcriptRemoveFillers(bool phrases, bool ripple);
  Q_INVOKABLE int transcriptFillerCount(bool phrases) const;
  // Takes out the silences between words that are at least this long, leaving `keep` seconds at each side.
  Q_INVOKABLE int transcriptRemovePauses(double minimum, double keep);
  Q_INVOKABLE int transcriptPauseCount(double minimum, double keep) const;
  // Captions from the words on the timeline, into the "Transcript" caption track.
  Q_INVOKABLE bool transcriptCaptions(int max_line_chars, int max_lines);
  Q_INVOKABLE int transcriptFind(const QString& query);
  Q_INVOKABLE void transcriptClearFind();
  // Moves the playhead to where the word is heard on the timeline (the first place, or the one after the playhead).
  Q_INVOKABLE bool transcriptSeek(int word);
  Q_INVOKABLE bool transcriptNextHit(int direction);
  // Corrections to the text and names for the speakers are kept with the transcript, not in the project's undo history.
  Q_INVOKABLE bool transcriptSetText(int word, const QString& text);
  Q_INVOKABLE bool transcriptSetSpeaker(int first, int last, const QString& name);
  Q_INVOKABLE QString transcriptPlainText() const;
  // Brings the transcript panel forward.
  Q_INVOKABLE void showTranscript();

  // ---- captions: timed-text tracks, cue authoring, styling and offline interchange
  [[nodiscard]] QVariantList captionTracks() const;
  [[nodiscard]] QString captionTrack() const { return QString::fromStdString(caption_track_); }
  [[nodiscard]] QVariantList captions() const;
  Q_INVOKABLE void captionSelectTrack(const QString& id);
  Q_INVOKABLE QString captionAddTrack(const QString& name, const QString& language);
  Q_INVOKABLE bool captionUpdateTrack(const QString& id, const QString& name, const QString& language);
  Q_INVOKABLE bool captionSetTrackStyle(const QString& id, const QString& family, double size, bool bold, bool italic,
                                        const QString& align, const QString& position, double outline_width,
                                        double background_opacity, double max_width);
  Q_INVOKABLE bool captionRemoveTrack(const QString& id);
  Q_INVOKABLE QString captionAdd(const QString& track_id, double start, double end, const QString& text, const QString& speaker);
  Q_INVOKABLE bool captionUpdate(const QString& id, double start, double end, const QString& text, const QString& speaker);
  Q_INVOKABLE bool captionRemove(const QString& id);
  Q_INVOKABLE bool captionImport(const QString& track_id, const QString& path);
  Q_INVOKABLE bool captionExport(const QString& track_id, const QString& path, const QString& format);

  // ---- exporting: presets, the plan a preset makes for this sequence, and the queue
  [[nodiscard]] QVariantList exportJobs() const;
  [[nodiscard]] bool exportQueuePaused() const;
  Q_INVOKABLE QVariantList exportPresets() const;
  // What exporting with this preset would do: {ok, refusal, encoder, hardware, notes, estimatedBytes, outputPath, summary}.
  Q_INVOKABLE QVariantMap exportPlan(const QString& preset_id, const QString& output_path, bool prefer_hardware, bool use_marks) const;
  // Queues an export of the open sequence; returns the job id, or empty (the reason is in the status line).
  Q_INVOKABLE QString exportQueueAdd(const QString& preset_id, const QString& output_path, const QString& name, bool overwrite, bool prefer_hardware, bool use_marks);
  Q_INVOKABLE bool exportCancel(const QString& id);
  Q_INVOKABLE bool exportRetry(const QString& id);
  Q_INVOKABLE bool exportRemove(const QString& id);
  Q_INVOKABLE bool exportMove(const QString& id, int position);
  Q_INVOKABLE void exportClearFinished();
  Q_INVOKABLE void exportPause(bool paused);
  Q_INVOKABLE bool exportWaitIdle(int milliseconds) const;
  Q_INVOKABLE void exportReveal(const QString& path);
  // Where an export is first offered to be saved: the Movies folder, else the home folder.
  Q_INVOKABLE QString exportFolder() const;
  // Brings the export queue panel forward.
  Q_INVOKABLE void showExports();

  // ---- masks: drawing and editing a shape on the monitor, and what the inspector says about it
  [[nodiscard]] QString maskTool() const { return mask_tool_; }
  [[nodiscard]] QString maskActive() const { return QString::fromStdString(mask_id_); }
  [[nodiscard]] bool maskInteractive() const;
  [[nodiscard]] QVariantList maskTracks() const;
  // The mask on show, as the monitor draws it: {active, outline, handles, penPoints, tool, smooth}, in picture coordinates (0 to 1).
  [[nodiscard]] QVariantMap maskOverlay() const;
  // "none" (the monitor behaves as a monitor), "select" (drag the shape the inspector chose), "rectangle", "ellipse" or "pen".
  Q_INVOKABLE void maskSetTool(const QString& tool);
  Q_INVOKABLE void maskSelect(const QString& id);
  // The effect new masks are drawn on (a clip's masks limit where one of its effects applies).
  Q_INVOKABLE void maskSetTarget(const QString& effect_id);
  Q_INVOKABLE bool maskAdd(const QString& effect_id, const QString& shape);
  Q_INVOKABLE bool maskRemove(const QString& id);
  Q_INVOKABLE bool maskSetNumber(const QString& id, const QString& property, double value);
  Q_INVOKABLE bool maskSetMode(const QString& id, const QString& combine, bool inverted);
  Q_INVOKABLE bool maskToggleKey(const QString& id, const QString& property);
  Q_INVOKABLE bool maskStopAnimating(const QString& id, const QString& property);
  // The pointer, in picture coordinates. Press returns whether the mask editor took the click.
  Q_INVOKABLE bool maskPress(double x, double y, double tolerance_pixels, int modifiers);
  Q_INVOKABLE bool maskMove(double x, double y, int modifiers);
  Q_INVOKABLE bool maskRelease(double x, double y, int modifiers);
  Q_INVOKABLE bool maskDoubleClick(double x, double y, double tolerance_pixels);
  Q_INVOKABLE bool maskDeletePoint();
  Q_INVOKABLE void maskFinishPen();
  Q_INVOKABLE void maskCancelPen();
  Q_INVOKABLE bool maskFollowTrack(const QString& mask_id, const QString& track_id);
  // Follows the middle of the shape from the playhead to the end of the clip, as a job; the result is stored and applied.
  Q_INVOKABLE void maskTrack(const QString& mask_id);
  // The folder the open project lives in (its .cutline package), empty without a project.
  Q_INVOKABLE QString projectFolder() const { return QString::fromStdString(package_folder_); }
  // A group of the first three clips of the project, lined up by hand at zero, opened: for demonstrations and tests.
  Q_INVOKABLE QString multicamDemo();
  // The ids of the sequence tracks of one kind ("video" or "audio"), top to bottom, for pickers.
  Q_INVOKABLE QStringList trackIds(const QString& kind) const;


  // Where preferences, shortcuts and workspaces are kept; overridable so tests do not touch the real ones.
  void SetConfigDirectory(const QString& directory);
  [[nodiscard]] ui::WorkspaceSet& workspaces() { return workspaces_; }
  void SaveConfiguration();

 signals:
  void projectChanged();
  void sequenceChanged();
  void playheadChanged();
  void transportChanged();
  void historyChanged();
  void toolChanged();
  void snapChanged();
  void statusChanged();
  void mediaChanged();
  void inspectorChanged();
  void jobsChanged();
  void marksChanged();
  void monitorChanged();
  void frameReady();
  void audioMixerChanged();
  void scopeChanged();
  void selectionChanged();
  void workspaceCommand(const QString& command_id);
  void timelineAction(const QString& action);
  void monitorAction(const QString& action);
  void exportRequested();
  void speedRequested();
  void rampRequested();
  void rampChanged();
  void multicamChanged();
  void multicamPositionChanged();
  void multicamFrameReady();
  void exportsChanged();
  void maskChanged();
  void audioLoudnessChanged();
  void transcriptChanged();
  void transcriptWordChanged();
  void transcriptHitsChanged();
  void captionsChanged();
  void commandPaletteRequested();
  void preferencesRequested();
  void shortcutsRequested();
  void importRequested();
  void ingestRequested();
  void transitionRequested(const QString& id);
  void graphicsChanged();
  void designerChanged();
  void designerRequested();
  void openRequested();
  void quitRequested();

 private:
  [[nodiscard]] static double Seconds(const time::RationalTime& t) { return static_cast<double>(t.numerator()) / static_cast<double>(t.denominator()); }
  bool Open(std::unique_ptr<project::ProjectStore> store);
  void Reload(bool invalidate_media = true);
  void StartEngine();
  void RequestFrame(ui::PresentationMode mode = ui::PresentationMode::LatestOnly);
  void OnTick();
  void StartPlayback();
  void StopPlayback();
  void SyncAudioToTransport();
  void RefreshMedia();
  void RefreshInspector();
  void RefreshHistory();
  [[nodiscard]] std::string NewId(const std::string& prefix) const;
  [[nodiscard]] commands::CommandEnvelope Envelope(commands::CommandType type, commands::CommandPayload payload);
  void Run(commands::CommandType type, commands::CommandPayload payload);
  [[nodiscard]] std::optional<std::string> PrimaryClip() const;
  [[nodiscard]] std::set<std::string> SelectedOrUnderPlayhead() const;
  void DoCommand(const std::string& id);
  void OnFrame(QImage image, std::uint64_t serial, const QString& info);
  void RefreshScopes();

  // multicam
  [[nodiscard]] std::shared_ptr<const timeline::multicam::Group> McGroup() const;
  [[nodiscard]] time::FrameRate McRate() const;
  bool McApply(const ui::EditPlan& plan);
  void RefreshMulticam();
  void StartMulticamMonitor();
  void RequestMulticamFrame();
  void StopMulticamClock();
  void OnMulticamTick();
  void MulticamCommand(const std::string& id);
  // export
  // ingest and proxies
  void MakeProxy(ui::JobContext& context, const std::string& media_id, const std::string& choice);
  std::map<std::string, double> proxy_progress_;   // media id -> 0..1 while a proxy is being made
  // audio workflow
  [[nodiscard]] std::set<std::string> AudioClipsOfSelection() const;
  void FlushAutomation(const time::RationalTime& stop);
  struct AutomationPass final {
    std::string track;
    ui::AutomationMode mode{ui::AutomationMode::Read};
    ui::AutomationTarget target{ui::AutomationTarget::Volume};
    std::vector<ui::AutomationSample> samples;
  };
  std::unique_ptr<audio::Recorder> recorder_;
  std::string recording_track_, recording_path_;
  bool recording_started_playback_{false};
  time::RationalTime recording_start_{0, 1};
  std::map<std::string, ui::AutomationMode> automation_modes_;
  std::vector<AutomationPass> automation_passes_;
  QVariantMap audio_loudness_;
  // transcript
  struct MediaFacts final {
    std::string name, path, fingerprint;
    double duration{0.0};
    bool audio{false};
  };
  [[nodiscard]] MediaFacts MediaFactsOf(const std::string& media_id) const;
  [[nodiscard]] std::string TranscriptTarget() const;
  [[nodiscard]] std::string TranscriptFile(const std::string& media_id) const;
  [[nodiscard]] const speech::Transcript* TranscriptFor(const std::string& media_id);
  [[nodiscard]] std::optional<speech::WhisperTools> FindEngine() const;
  void RefreshTranscript(bool force = false);
  void BuildTranscriptParagraphs();
  void UpdateTranscriptWord();
  bool SaveTranscript(const std::string& media_id);
  [[nodiscard]] std::vector<std::size_t> PositionsOf(int first, int last) const;
  // masks
  [[nodiscard]] ui::PictureSize MaskPicture() const;
  [[nodiscard]] double MaskLocalSeconds(const timeline::Clip& clip) const;
  [[nodiscard]] QVariantMap maskMap(const timeline::EffectMask& mask, const timeline::Clip& clip) const;
  [[nodiscard]] QVariantList masksOf(const std::string& effect_id) const;
  bool MaskApply(const ui::EditPlan& plan);
  bool MaskEdit(const std::string& mask_id, const std::function<ui::MaskDocument(const ui::MaskDocument&, const ui::MaskDocument&, double)>& change, const std::string& label);
  void StartExportQueue();
  [[nodiscard]] exporter::SequenceFacts ExportFacts(const time::RationalTime& in, const time::RationalTime& out) const;
  exporter::JobOutcome RunExportJob(const exporter::ExportJob& job, const exporter::JobProgress& progress);


  // titles and graphics (SessionGraphics.cpp)
  struct DesignerState;
  [[nodiscard]] std::string GraphicDocumentJson(const std::string& id) const;
  [[nodiscard]] QString PreviewKey(const std::string& document_json);
  [[nodiscard]] std::string BindPictures(const std::string& document_json) const;
  [[nodiscard]] QString DesignerPreviewUrl(int width);
  void DesignerEdited(bool undoable = true);
  void DesignerPushUndo();
  [[nodiscard]] std::optional<std::string> InstalledTemplateJson(const std::string& id, std::int64_t version) const;

  std::unique_ptr<project::ProjectStore> store_;
  int graphics_revision_{0};
  std::shared_ptr<DesignerState> designer_;
  std::string project_id_;
  std::string sequence_id_{"seq-1"};
  timeline::SequenceGraph graph_;
  std::unique_ptr<playback::PlaybackEngine> engine_;
  std::unique_ptr<audio::AudioSink> sink_;
  std::unique_ptr<ui::FramePresenter> presenter_;
  std::shared_ptr<render::FlowCache> flow_cache_;
  std::unique_ptr<render::AsyncScopes> scopes_;
  mutable std::mutex scopes_mutex_;
  std::optional<render::AsyncScopeResult> scope_result_;
  QString scope_mode_;
  std::uint64_t scope_generation_{0};
  ui::Transport transport_;
  ui::Selection selection_;
  ui::Preferences prefs_;
  ui::Keymap keymap_;
  ui::WorkspaceSet workspaces_;
  ui::JobTracker jobs_;
  std::unique_ptr<ui::JobRunner> runner_;
  ui::LutLibrary luts_;
  std::vector<ui::ClipSpec> clipboard_;
  std::optional<time::RationalTime> mark_in_, mark_out_;
  QTimer tick_;
  QString tool_{"selection"};
  QString status_;
  QVariantList media_;
  QVariantList inspector_;
  QString inspector_clip_;
  QImage frame_;
  QString render_info_;
  ui::MonitorQuality quality_{ui::MonitorQuality::Auto};
  ui::SizePx displayed_{0, 0};
  std::int64_t counter_{0};
  std::string package_folder_;
  QString config_dir_;
  bool slow_modifier_{false};
  bool audio_enabled_{true};
  std::size_t analysis_limit_{20000};
  std::vector<time::RationalTime> markers_;
  std::string source_media_;
  ui::RampModel ramp_;
  std::string ramp_clip_;
  bool ramp_dirty_{false};
  bool ramp_live_{false};
  bool ramp_dragging_{false};
  std::uint64_t latest_serial_{0};
  int audio_meter_ticks_{0};

  // masks: what is being edited, and the gesture under way (computed from where it began, so it never accumulates)
  struct MaskGesture final {
    bool active{false};
    bool drawing{false};
    bool moved{false};
    ui::MaskHandle handle;
    double press_x{0}, press_y{0}, x{0}, y{0};
    ui::MaskDocument stored, evaluated, preview_evaluated;
    std::string clip_id;
    double seconds{0};
  };
  // transcripts: kept per media, the one shown follows the selection
  std::map<std::string, speech::Transcript> tx_cache_;
  std::string tx_media_;
  std::set<std::string> tx_working_;
  ui::WordMapping tx_map_;
  QVariantList tx_paragraphs_;
  QVariantList tx_hits_;
  QString tx_summary_;
  int tx_word_{-1};
  int tx_hit_{-1};
  std::uint64_t tx_revision_{0};
  std::string tx_signature_;
  std::string caption_track_;
  std::string mask_id_;
  std::string mask_effect_;
  QString mask_tool_{"none"};
  int mask_point_{-1};
  std::vector<std::pair<double, double>> pen_points_;
  MaskGesture gesture_;
  // multicam state; the presenter is declared last so its thread ends before the group and the monitor go
  std::string mc_id_;
  time::RationalTime mc_position_{0, 1};
  bool mc_playing_{false};
  QElapsedTimer mc_clock_;
  double mc_clock_base_{0};
  QTimer mc_tick_;
  QImage mc_frame_;
  mutable std::mutex mc_mutex_;
  std::shared_ptr<const timeline::multicam::Group> mc_group_;
  std::unique_ptr<ui::AngleMonitor> mc_monitor_;
  std::unique_ptr<ui::FramePresenter> mc_presenter_;
  // Last of all: its worker reads the project and must stop before anything it reads goes away.
  std::unique_ptr<exporter::ExportQueue> export_queue_;
};

}  // namespace cutline::app
