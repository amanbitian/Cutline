// The audio workflow in the application: choosing what a sound is for, matching it to a loudness target, and writing
// automation from a fader moved while the sequence plays. The rules are ui/AudioWorkflow.h and audio/Dsp.h; this holds
// the passes in progress, runs the measurement as a job and turns each result into one undoable edit.

#include "app/Session.h"

#include "audio/Dsp.h"
#include "core/db/Sql.h"
#include "media/Ingest.h"
#include "media/Source.h"
#include "playback/PlaybackEngine.h"
#include "timeline/SequenceLoader.h"

#include <QDateTime>
#include <QDir>
#include <QMetaObject>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace cutline::app {

using time::RationalTime;

namespace {

double SecondsOf(const RationalTime& t) { return static_cast<double>(t.numerator()) / static_cast<double>(std::max<std::int64_t>(1, t.denominator())); }

const timeline::Clip* FindClipIn(const timeline::Sequence& sequence, const std::string& id, const timeline::Track** track = nullptr) {
  for (const auto& t : sequence.tracks) {
    for (const auto& clip : t.clips) {
      if (clip.id == id) {
        if (track != nullptr) *track = &t;
        return &clip;
      }
    }
  }
  return nullptr;
}

bool HasChain(const timeline::Clip& clip) {
  return std::any_of(clip.effects.begin(), clip.effects.end(), [](const timeline::Effect& e) { return e.id.rfind("role-", 0) == 0; });
}

double ConstantLevel(const std::vector<timeline::Effect>& effects) {
  for (const auto& effect : effects) {
    if (effect.effect_type != "volume" || effect.id.rfind("auto-", 0) == 0 || effect.id.rfind("role-", 0) == 0) continue;
    for (const auto& parameter : effect.parameters) {
      if (parameter.name == "level") return parameter.value.animated() ? parameter.value.keyframes().front().value.scalar() : parameter.value.constant().scalar();
    }
  }
  return 0.0;
}

}  // namespace

// ------------------------------------------------------------------------------- what is chosen ----

std::set<std::string> Session::AudioClipsOfSelection() const {
  std::set<std::string> found;
  const auto* sequence = graph_.root();
  if (sequence == nullptr) return found;
  std::set<std::string> groups;
  for (const auto& track : sequence->tracks) {
    for (const auto& clip : track.clips) {
      if (!selection_.Contains(clip.id)) continue;
      if (track.kind == model::TrackKind::Audio) found.insert(clip.id);
      else if (!clip.linked_group.empty()) groups.insert(clip.linked_group);
    }
  }
  // The sound of a chosen picture: the clip linked with it.
  for (const auto& track : sequence->tracks) {
    if (track.kind != model::TrackKind::Audio) continue;
    for (const auto& clip : track.clips) {
      if (!clip.linked_group.empty() && groups.count(clip.linked_group) != 0) found.insert(clip.id);
    }
  }
  return found;
}

QVariantMap Session::audioSelection() const {
  const auto* sequence = graph_.root();
  QVariantMap map;
  if (sequence == nullptr) return map;
  const auto clips = AudioClipsOfSelection();
  if (clips.empty()) return map;
  const timeline::Track* track = nullptr;
  const auto* clip = FindClipIn(*sequence, *clips.begin(), &track);
  if (clip == nullptr) return map;
  const bool plain = clip->source_kind == model::SourceKind::Media && clip->playback_rate.Compare(RationalTime(1, 1)) == 0 && !clip->reversed &&
                     std::none_of(clip->effects.begin(), clip->effects.end(), [](const timeline::Effect& e) { return e.effect_type == "time_remap"; });
  map["clip"] = QString::fromStdString(clip->id);
  map["name"] = QString::fromStdString(clip->name.empty() ? clip->id : clip->name);
  map["count"] = static_cast<int>(clips.size());
  map["role"] = QString::fromStdString(clip->audio_role);
  map["track"] = QString::fromStdString(track->id);
  map["gainDb"] = ConstantLevel(clip->effects);
  map["hasChain"] = HasChain(*clip);
  map["measurable"] = plain;
  return map;
}

QVariantList Session::audioRoles() const {
  QVariantList list;
  for (const auto role : {ui::AudioRole::Dialogue, ui::AudioRole::Music, ui::AudioRole::Effects, ui::AudioRole::Ambience}) {
    list << QVariantMap{{"id", QString::fromStdString(ui::ToString(role))}, {"label", QString::fromStdString(ui::RoleLabel(role))}};
  }
  return list;
}

QVariantList Session::audioLoudnessTargets() const {
  QVariantList list;
  for (const auto& target : ui::LoudnessTargets()) {
    list << QVariantMap{{"id", QString::fromStdString(target.id)}, {"name", QString::fromStdString(target.name)}, {"lufs", target.lufs},
                        {"truePeakDb", target.true_peak_db}, {"note", QString::fromStdString(target.note)}};
  }
  return list;
}

bool Session::audioApplyRole(const QString& role_name, bool chain, double repair, double dereverb, bool tone, bool dynamics, const QString& duck_under_track) {
  const auto clips = AudioClipsOfSelection();
  const auto role = ui::ParseAudioRole(role_name.toStdString());
  if (!role) return false;
  ui::RoleChainOptions options;
  options.repair = repair;
  options.dereverb = dereverb;
  options.tone = tone;
  options.dynamics = dynamics;
  options.duck_under_track = duck_under_track.toStdString();
  if (*role == ui::AudioRole::None) return Apply(ui::PlanClearRole(EditContextFor(), clips));
  return Apply(ui::PlanApplyRole(EditContextFor(), clips, *role, chain, options));
}

// ------------------------------------------------------------------------------------- loudness ----

void Session::audioMeasure(const QString& scope, const QString& target_id) {
  const auto* sequence = graph_.root();
  if (sequence == nullptr || store_ == nullptr) return;
  const auto* target = ui::FindLoudnessTarget(target_id.toStdString());
  const auto revision = sequence->source_revision;
  const bool clip_scope = scope == "clip";

  struct Job final {
    std::string clip_id, clip_name, path;
    time::RationalTime source_in, source_out, range_in, range_out;
    timeline::SequenceGraph graph;
    std::int64_t rate{48000};
    int channels{2};
  } job;

  if (clip_scope) {
    const auto clips = AudioClipsOfSelection();
    if (clips.empty()) {
      ShowStatus(tr("Choose the sound of a clip first"));
      return;
    }
    const timeline::Track* track = nullptr;
    const auto* clip = FindClipIn(*sequence, *clips.begin(), &track);
    if (clip == nullptr || clip->source_kind != model::SourceKind::Media) {
      ShowStatus(tr("Only a clip of media can be measured"));
      return;
    }
    if (clip->playback_rate.Compare(RationalTime(1, 1)) != 0 || clip->reversed || std::any_of(clip->effects.begin(), clip->effects.end(), [](const timeline::Effect& e) { return e.effect_type == "time_remap"; })) {
      ShowStatus(tr("A clip at another speed, reversed or with a speed ramp is matched through the programme instead"));
      return;
    }
    const auto facts = MediaFactsOf(clip->source_id);
    if (facts.path.empty()) {
      ShowStatus(tr("The media is offline"));
      return;
    }
    job.clip_id = clip->id;
    job.clip_name = clip->name.empty() ? clip->id : clip->name;
    job.path = facts.path;
    job.source_in = clip->source_in;
    job.source_out = clip->source_out;
  } else {
    job.graph = graph_;
    job.range_in = mark_in_.value_or(RationalTime(0, 1));
    job.range_out = mark_out_.value_or(sequence->Duration());
    if (job.range_out.Compare(job.range_in) <= 0) {
      ShowStatus(tr("There is nothing in the sequence to measure"));
      return;
    }
    job.rate = sequence->sample_rate > 0 ? sequence->sample_rate : 48000;
    job.channels = static_cast<int>(media::ChannelCountForLayout(sequence->channel_layout));
  }
  audio_loudness_ = QVariantMap{{"state", "measuring"}, {"scope", scope}};
  emit audioLoudnessChanged();

  auto* store = store_.get();
  const auto target_copy = target != nullptr ? std::optional<ui::LoudnessTarget>(*target) : std::nullopt;
  const auto title = std::string(clip_scope ? "Measuring loudness of " + job.clip_name : std::string("Measuring programme loudness"));
  runner_->Run("loudness", title, [this, store, job = std::move(job), clip_scope, target_copy, revision, scope](ui::JobContext& context) mutable {
    audio::dsp::LoudnessMeter meter(job.rate, job.channels);
    double seconds = 0.0;
    if (clip_scope) {
      auto source = media::SourceRegistry::Instance().Open(job.path);
      if (source == nullptr) throw std::runtime_error("The media could not be opened");
      const auto length = job.source_out.Subtract(job.source_in);
      const auto total = std::max<std::int64_t>(1, length.Rescale(job.rate, time::RoundingMode::Nearest));
      for (std::int64_t done = 0; done < total && !context.cancelled(); done += job.rate) {
        const auto frames = std::min<std::int64_t>(job.rate, total - done);
        const auto block = source->ReadAudio(job.source_in.Add(RationalTime(done, job.rate)), job.rate, job.channels, frames);
        if (!block) throw std::runtime_error("The sound of the media could not be read");
        meter.Push(*block);
        context.Progress(static_cast<std::uint64_t>(done + frames), static_cast<std::uint64_t>(total));
      }
      seconds = static_cast<double>(total) / static_cast<double>(job.rate);
    } else {
      playback::EngineConfig config;
      config.sample_rate = job.rate;
      config.channels = job.channels;
      config.decode_workers = 0;
      config.read_ahead_frames = 0;
      config.use_gpu = false;
      const auto lookup = [store](const std::string& id) {
        const std::lock_guard<std::mutex> lock(store->mutex());
        db::Statement statement(store->connection(), "SELECT original_path FROM media WHERE id = ?;");
        statement.Bind(1, id);
        return statement.Step() ? statement.ColumnText(0) : std::string();
      };
      playback::PlaybackEngine engine(std::move(job.graph), lookup, config);
      timeline::CompileOptions options;
      options.high_quality_audio = true;
      const auto first = job.range_in.Rescale(job.rate, time::RoundingMode::Nearest);
      const auto total = std::max<std::int64_t>(1, job.range_out.Rescale(job.rate, time::RoundingMode::Nearest) - first);
      for (std::int64_t done = 0; done < total && !context.cancelled(); done += job.rate) {
        const auto frames = std::min<std::int64_t>(job.rate, total - done);
        meter.Push(engine.RenderAudioAt(first + done, frames, options));
        context.Progress(static_cast<std::uint64_t>(done + frames), static_cast<std::uint64_t>(total));
      }
      seconds = static_cast<double>(total) / static_cast<double>(job.rate);
    }
    if (context.cancelled()) {
      QMetaObject::invokeMethod(this, [this] { audio_loudness_.clear(); emit audioLoudnessChanged(); }, Qt::QueuedConnection);
      return;
    }
    const auto measured = meter.Result();
    QMetaObject::invokeMethod(this, [this, measured, seconds, clip_scope, target_copy, revision, scope, clip_id = job.clip_id]() {
      QVariantMap result{{"state", "done"}, {"scope", scope}, {"seconds", seconds}, {"integratedLufs", measured.integrated_lufs}, {"truePeakDb", measured.true_peak_db},
                         {"samplePeakDb", measured.sample_peak_db}, {"loudestMomentaryLufs", measured.loudest_momentary_lufs}, {"shortTermLufs", measured.short_term_lufs}};
      if (target_copy) {
        const auto advice = ui::AdviseLoudness(measured, *target_copy);
        result["target"] = QString::fromStdString(target_copy->id);
        result["gainDb"] = advice.gain_db;
        result["overCeiling"] = advice.over_ceiling;
        result["message"] = QString::fromStdString(advice.message);
        const auto* sequence_now = graph_.root();
        if (!advice.measurable) {
          ShowStatus(QString::fromStdString(advice.message));
        } else if (sequence_now == nullptr || sequence_now->source_revision != revision) {
          result["message"] = tr("The sequence changed while it was measured: measure again to apply a gain");
          ShowStatus(result["message"].toString());
        } else if (clip_scope) {
          // The clip as the file holds it: its gain is the whole of what reaches the target.
          result["applied"] = Apply(ui::PlanSetClipGain(EditContextFor(), clip_id, advice.gain_db, "Match Clip Loudness"));
        } else {
          // The programme as it plays now, master gain included: the master moves by the difference.
          result["applied"] = Apply(ui::PlanSetMasterGain(EditContextFor(), ConstantLevel(sequence_now->effects) + advice.gain_db, "Match Programme Loudness"));
        }
      }
      audio_loudness_ = result;
      emit audioLoudnessChanged();
    }, Qt::QueuedConnection);
  });
}

// ----------------------------------------------------------------------------------- automation ----

QString Session::audioAutomationMode(const QString& track_id) const {
  const auto found = automation_modes_.find(track_id.toStdString());
  return QString::fromStdString(ui::ToString(found == automation_modes_.end() ? ui::AutomationMode::Read : found->second));
}

bool Session::audioSetAutomationMode(const QString& track_id, const QString& mode_name) {
  const auto mode = ui::ParseAutomationMode(mode_name.toStdString());
  const auto* sequence = graph_.root();
  const auto* track = sequence != nullptr ? sequence->FindTrack(track_id.toStdString()) : nullptr;
  if (!mode || track == nullptr || track->kind != model::TrackKind::Audio) return false;
  FlushAutomation(transport_.position());
  automation_modes_[track->id] = *mode;
  emit audioMixerChanged();
  return true;
}

bool Session::audioAutomate(const QString& track_id, const QString& control, double value, const QString& phase) {
  const auto track = track_id.toStdString();
  const auto found = automation_modes_.find(track);
  const auto mode = found == automation_modes_.end() ? ui::AutomationMode::Read : found->second;
  if (mode == ui::AutomationMode::Read || !transport_.playing()) return false;
  const auto target = control == "pan" ? ui::AutomationTarget::Pan : ui::AutomationTarget::Volume;
  auto it = std::find_if(automation_passes_.begin(), automation_passes_.end(), [&](const AutomationPass& p) { return p.track == track && p.target == target; });
  if (it == automation_passes_.end()) {
    // A pass starts with the hand going down; a move or a release with no pass begun is not part of one.
    if (phase != "begin") return false;
    automation_passes_.push_back({track, mode, target, {}});
    it = std::prev(automation_passes_.end());
  }
  const bool hand_on = phase != "end";
  it->samples.push_back({transport_.position(), value, hand_on});
  // Touch stops writing at the release; write and latch go on until the transport stops.
  if (!hand_on && mode == ui::AutomationMode::Touch) FlushAutomation(transport_.position());
  return true;
}

void Session::FlushAutomation(const RationalTime& stop) {
  if (automation_passes_.empty()) return;
  auto passes = std::move(automation_passes_);
  automation_passes_.clear();
  const auto* sequence = graph_.root();
  for (const auto& pass : passes) {
    if (sequence == nullptr) break;
    const auto* track = sequence->FindTrack(pass.track);
    if (track == nullptr) continue;
    const auto keys = ui::AutomationKeys(pass.mode, pass.samples, stop, ui::AutomationOf(*track, pass.target));
    if (!keys.any) continue;
    (void)Apply(ui::PlanWriteAutomation(EditContextFor(), pass.track, pass.target, keys));
    sequence = graph_.root();   // the edit reloaded the snapshot
  }
}

// ------------------------------------------------------------------------------------ recording ----

QVariantList Session::audioInputs() const {
  QVariantList list;
  for (const auto& device : audio::ListInputDevices()) {
    list << QVariantMap{{"id", QString::fromStdString(device.id)}, {"name", QString::fromStdString(device.name)}, {"isDefault", device.is_default}};
  }
  return list;
}

bool Session::audioRecordStart(const QString& track_id, const QString& device_id, bool play) {
  const auto* sequence = graph_.root();
  const auto* track = sequence != nullptr ? sequence->FindTrack(track_id.toStdString()) : nullptr;
  if (store_ == nullptr || sequence == nullptr) return false;
  if (recorder_ != nullptr) {
    ShowStatus(tr("A take is already being recorded"));
    return false;
  }
  if (track == nullptr || track->kind != model::TrackKind::Audio || track->is_bus) {
    ShowStatus(tr("Arm an audio track to record onto"));
    return false;
  }
  if (track->locked) {
    ShowStatus(tr("The track %1 is locked").arg(track_id));
    return false;
  }
  QDir folder(package_folder_.empty() ? QDir::temp().filePath("cutline-recordings") : QString::fromStdString(package_folder_) + "/recordings");
  if (!folder.mkpath(".")) {
    ShowStatus(tr("The recordings folder could not be made"));
    return false;
  }
  const auto path = folder.filePath(QString::fromStdString(NewId("take")) + ".wav").toStdString();
  std::string why;
  auto recorder = audio::Recorder::Start(device_id.toStdString(), path, sequence->sample_rate > 0 ? sequence->sample_rate : 48000, 1, &why);
  if (recorder == nullptr) {
    ShowStatus(QString::fromStdString(why));
    return false;
  }
  recorder_ = std::move(recorder);
  recording_track_ = track->id;
  recording_path_ = path;
  recording_start_ = transport_.position();
  recording_started_playback_ = false;
  if (play && !transport_.playing()) {
    trigger("transport.play_pause");
    recording_started_playback_ = transport_.playing();
  }
  emit audioMixerChanged();
  ShowStatus(tr("Recording on %1").arg(QString::fromStdString(track->name.empty() ? track->id : track->name)));
  return true;
}

bool Session::audioRecordStop() {
  if (recorder_ == nullptr) return false;
  const auto result = recorder_->Stop();
  recorder_.reset();
  if (recording_started_playback_ && transport_.playing()) trigger("transport.stop");
  recording_started_playback_ = false;
  const auto track = recording_track_;
  const auto path = recording_path_;
  const auto start = recording_start_;
  emit audioMixerChanged();
  const auto rate = graph_.root() != nullptr && graph_.root()->sample_rate > 0 ? graph_.root()->sample_rate : 48000;
  if (!result.ok || result.frames < rate / 20) {
    ShowStatus(result.ok ? tr("The take was too short to keep") : QString::fromStdString(result.error));
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return false;
  }
  media::IngestRequest request;
  request.project_id = project_id_;
  request.author_id = "user";
  request.timestamp_utc = "2026-01-01T00:00:00Z";
  request.media_id = NewId("media");
  request.path = path;
  request.display_name = "Voice-over " + QDateTime::currentDateTime().toString("HH.mm.ss").toStdString();
  request.base_revision = store_->CurrentRevision();
  std::string media_id;
  try {
    media_id = media::IngestFile(*store_, request).media_id;
  } catch (const std::exception& error) {
    ShowStatus(tr("The take could not be imported: %1").arg(error.what()));
    return false;
  }
  RefreshMedia();
  Reload();
  return Apply(InsertPlanAt(media_id, start, std::string(), track, false));
}

}  // namespace cutline::app
