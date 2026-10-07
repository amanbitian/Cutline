// Text-based editing in the application: finding the words of the media being edited, making a transcript of it as a
// background job, showing the words as paragraphs, and turning what is done to them (delete these words, take out the
// "um"s, close the long pauses, make captions) into one undoable edit each. The mapping from words to timeline times and
// the edits themselves are ui/TextEdit.h; the words and what is found in them are speech/Transcript.h.

#include "app/Session.h"

#include "core/db/Sql.h"
#include "media/Source.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace cutline::app {

using time::RationalTime;

namespace {

double SecondsOf(const RationalTime& t) { return static_cast<double>(t.numerator()) / static_cast<double>(std::max<std::int64_t>(1, t.denominator())); }

std::string SafeName(const std::string& id) {
  std::string out = id;
  for (auto& c : out) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '-' || c == '_')) c = '_';
  }
  return out;
}

bool EndsSentence(const std::string& text) { return !text.empty() && (text.back() == '.' || text.back() == '?' || text.back() == '!'); }

}  // namespace

// ------------------------------------------------------------------------------- what is shown ----

Session::MediaFacts Session::MediaFactsOf(const std::string& media_id) const {
  MediaFacts facts;
  if (store_ == nullptr || media_id.empty()) return facts;
  const std::lock_guard<std::mutex> lock(store_->mutex());
  db::Statement statement(store_->connection(), R"sql(
    SELECT m.display_name, m.original_path, m.fingerprint, m.duration_num, m.duration_den,
           EXISTS(SELECT 1 FROM media_streams s WHERE s.media_id = m.id AND s.kind = 'audio')
      FROM media m WHERE m.id = ?;
  )sql");
  statement.Bind(1, media_id);
  if (!statement.Step()) return facts;
  facts.name = statement.ColumnText(0);
  facts.path = statement.ColumnText(1);
  facts.fingerprint = statement.ColumnText(2);
  facts.duration = static_cast<double>(statement.ColumnInt(3)) / static_cast<double>(std::max<std::int64_t>(1, statement.ColumnInt(4)));
  facts.audio = statement.ColumnInt(5) != 0;
  return facts;
}

std::string Session::TranscriptTarget() const {
  const auto* sequence = graph_.root();
  if (sequence == nullptr) return {};
  const auto media_of = [&](const std::string& clip_id) -> std::string {
    for (const auto& track : sequence->tracks) {
      for (const auto& clip : track.clips) {
        if (clip.id == clip_id && clip.source_kind == model::SourceKind::Media) return clip.source_id;
      }
    }
    return {};
  };
  if (const auto selected = PrimaryClip()) {
    if (auto media = media_of(*selected); !media.empty()) return media;
  }
  // Nothing chosen: what is being heard at the playhead, sound before picture.
  const auto at = transport_.position();
  for (const auto kind : {model::TrackKind::Audio, model::TrackKind::Video}) {
    for (const auto& track : sequence->tracks) {
      if (track.kind != kind || track.is_bus) continue;
      for (const auto& clip : track.clips) {
        if (clip.source_kind == model::SourceKind::Media && clip.timeline_start.Compare(at) <= 0 && clip.end().Compare(at) > 0) return clip.source_id;
      }
    }
  }
  // Nothing there either (the playhead is in a gap): the one that was being shown stays.
  return tx_media_;
}

std::string Session::TranscriptFile(const std::string& media_id) const {
  if (package_folder_.empty() || media_id.empty()) return {};
  return package_folder_ + "/transcripts/" + SafeName(media_id) + ".json";
}

const speech::Transcript* Session::TranscriptFor(const std::string& media_id) {
  if (media_id.empty()) return nullptr;
  if (const auto found = tx_cache_.find(media_id); found != tx_cache_.end()) return &found->second;
  const auto file = TranscriptFile(media_id);
  if (file.empty()) return nullptr;
  auto loaded = speech::Load(file);
  if (!loaded) return nullptr;
  // A transcript of another file (the media was replaced under the same name) is not this media's words.
  const auto facts = MediaFactsOf(media_id);
  if (!loaded->fingerprint.empty() && !facts.fingerprint.empty() && loaded->fingerprint != facts.fingerprint) return nullptr;
  return &tx_cache_.emplace(media_id, std::move(*loaded)).first->second;
}

std::optional<speech::WhisperTools> Session::FindEngine() const {
  std::vector<std::string> roots;
  for (QDir directory(QCoreApplication::applicationDirPath()); roots.size() < 7; ) {
    roots.push_back(directory.absolutePath().toStdString());
    if (!directory.cdUp()) break;
  }
  roots.push_back(QDir::currentPath().toStdString());
  return speech::FindWhisper(roots, prefs_.GetText("speech.whisper_path"), prefs_.GetText("speech.model_path"));
}

QString Session::transcriptEngine() const {
  const auto tools = FindEngine();
  if (!tools) return {};
  return QString("whisper.cpp ") + QString::fromStdString(std::filesystem::path(tools->model).stem().string());
}

QString Session::transcriptName() const { return QString::fromStdString(MediaFactsOf(tx_media_).name); }

QString Session::transcriptState() const {
  if (tx_media_.empty()) return "none";
  if (tx_working_.count(tx_media_) != 0) return "working";
  if (tx_cache_.count(tx_media_) != 0) return "ready";
  return FindEngine() ? "none" : "unavailable";
}

QVariantList Session::transcriptSpeakers() const {
  QVariantList list;
  if (const auto found = tx_cache_.find(tx_media_); found != tx_cache_.end()) {
    for (const auto& name : found->second.speakers) list << QString::fromStdString(name);
  }
  return list;
}

void Session::RefreshTranscript(bool force) {
  if (store_ == nullptr || graph_.root() == nullptr) {
    if (!tx_media_.empty() || !tx_paragraphs_.isEmpty()) {
      tx_media_.clear();
      tx_map_ = {};
      tx_paragraphs_.clear();
      tx_summary_.clear();
      tx_signature_.clear();
      emit transcriptChanged();
    }
    return;
  }
  const auto target = TranscriptTarget();
  const auto* sequence = graph_.root();
  const auto signature = target + "|" + std::to_string(tx_revision_) + "|" + std::to_string(sequence->source_revision) + "|" + std::to_string(tx_working_.size());
  if (!force && signature == tx_signature_) return;
  tx_signature_ = signature;
  if (target != tx_media_) {
    tx_hits_.clear();
    tx_hit_ = -1;
    emit transcriptHitsChanged();
  }
  tx_media_ = target;
  tx_map_ = {};
  if (const auto* transcript = TranscriptFor(tx_media_)) tx_map_ = ui::MapWords(*sequence, *transcript, tx_media_);
  BuildTranscriptParagraphs();
  emit transcriptChanged();
  UpdateTranscriptWord();
}

void Session::BuildTranscriptParagraphs() {
  tx_paragraphs_.clear();
  tx_summary_.clear();
  const auto found = tx_cache_.find(tx_media_);
  if (found == tx_cache_.end()) return;
  const auto& transcript = found->second;
  // 1 = heard on the timeline, 2 = only part of the word is.
  std::vector<std::uint8_t> heard(transcript.words.size(), 0);
  for (const auto& word : tx_map_.words) {
    if (!word.partial) heard[word.word] = 1;
    else if (heard[word.word] == 0) heard[word.word] = 2;
  }
  std::vector<std::uint8_t> filler(transcript.words.size(), 0);
  const auto fillers = speech::FindFillers(transcript);
  for (const auto& range : fillers) {
    for (std::size_t i = range.first; i < range.first + range.count && i < filler.size(); ++i) filler[i] = 1;
  }

  QVariantList words;
  double paragraph_start = 0.0;
  int paragraph_speaker = -2;
  const auto flush = [&] {
    if (words.isEmpty()) return;
    QVariantMap paragraph;
    paragraph["start"] = paragraph_start;
    paragraph["speaker"] = paragraph_speaker >= 0 && paragraph_speaker < static_cast<int>(transcript.speakers.size()) ? QString::fromStdString(transcript.speakers[static_cast<std::size_t>(paragraph_speaker)]) : QString();
    paragraph["words"] = words;
    tx_paragraphs_ << paragraph;
    words.clear();
  };
  for (std::size_t i = 0; i < transcript.words.size(); ++i) {
    const auto& word = transcript.words[i];
    const bool speaker_changed = !words.isEmpty() && word.speaker != paragraph_speaker;
    const bool pause = i > 0 && word.start - transcript.words[i - 1].end >= 1.5;
    const bool sentence = i > 0 && EndsSentence(transcript.words[i - 1].text) && words.size() >= 18;
    if (!words.isEmpty() && (speaker_changed || pause || sentence || words.size() >= 60)) flush();
    if (words.isEmpty()) {
      paragraph_start = word.start;
      paragraph_speaker = word.speaker;
    }
    words << QVariantMap{{"i", static_cast<int>(i)},
                         {"t", QString::fromStdString(word.text)},
                         {"f", filler[i] != 0},
                         {"on", heard[i] != 0},
                         {"part", heard[i] == 2},
                         {"low", word.confidence < 0.4f}};
  }
  flush();

  const auto frame = tx_map_.words.empty() || graph_.root() == nullptr ? RationalTime(1, 25) : RationalTime(graph_.root()->frame_rate.denominator, graph_.root()->frame_rate.numerator);
  const auto pauses = ui::PauseRanges(tx_map_, frame, 0.8, 0.0).size();
  tx_summary_ = tr("%1 words, %2 on the timeline, %3 fillers, %4 pauses of 0.8 s or more").arg(transcript.words.size()).arg(tx_map_.words.size()).arg(fillers.size()).arg(pauses);
  if (!transcript.engine.empty()) tx_summary_ += "  ·  " + QString::fromStdString(transcript.engine);
}

void Session::UpdateTranscriptWord() {
  int word = -1;
  if (!tx_map_.words.empty()) {
    const auto at = transport_.position();
    if (const auto position = ui::WordAt(tx_map_, at)) {
      const auto& mapped = tx_map_.words[*position];
      // The word that has just been said stays lit for a moment; after that nothing is being said.
      if (at.Compare(mapped.end.Add(RationalTime(1, 2))) < 0) word = static_cast<int>(mapped.word);
    }
  }
  if (word != tx_word_) {
    tx_word_ = word;
    emit transcriptWordChanged();
  }
}

bool Session::SaveTranscript(const std::string& media_id) {
  const auto found = tx_cache_.find(media_id);
  if (found == tx_cache_.end()) return false;
  ++tx_revision_;
  const auto file = TranscriptFile(media_id);
  const bool saved = file.empty() ? true : speech::Save(file, found->second);
  if (!saved) ShowStatus(tr("The transcript could not be saved next to the project"));
  return saved;
}

// ------------------------------------------------------------------------------------ making ----

void Session::transcribe() {
  if (store_ == nullptr || tx_media_.empty()) {
    ShowStatus(tr("Select a clip to transcribe its media"));
    return;
  }
  const auto media = tx_media_;
  if (tx_working_.count(media) != 0) return;
  const auto facts = MediaFactsOf(media);
  if (!facts.audio) {
    ShowStatus(tr("%1 has no sound to transcribe").arg(QString::fromStdString(facts.name)));
    return;
  }
  if (facts.path.empty() || !std::filesystem::exists(facts.path)) {
    ShowStatus(tr("The media is offline"));
    return;
  }
  const auto tools = FindEngine();
  if (!tools) {
    ShowStatus(tr("The speech engine was not found. Put whisper.cpp's whisper-cli and a ggml model in a \"whisper\" folder beside the application, or set their paths in Preferences."));
    return;
  }
  speech::WhisperOptions options;
  options.language = prefs_.GetText("speech.language");
  options.threads = static_cast<int>(prefs_.GetInt("speech.threads"));
  const auto sidecar = TranscriptFile(media);
  const auto scratch = (std::filesystem::temp_directory_path() / ("cutline-transcribe-" + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8).toStdString())).string();
  tx_working_.insert(media);
  RefreshTranscript(true);
  const auto title = std::string("Transcribing ") + facts.name;
  runner_->Run("transcribe", title, [this, media, facts, tools = *tools, options, sidecar, scratch](ui::JobContext& context) {
    const auto finish = [this, media](std::function<void()> then) {
      QMetaObject::invokeMethod(this, [this, media, then = std::move(then)]() {
        tx_working_.erase(media);
        if (then) then();
        RefreshTranscript(true);
      }, Qt::QueuedConnection);
    };
    std::error_code ignored;
    std::filesystem::create_directories(scratch, ignored);
    try {
      auto source = media::SourceRegistry::Instance().Open(facts.path);
      if (source == nullptr) throw std::runtime_error("The media could not be opened");
      const auto wav = scratch + "/speech.wav";
      context.Progress(0, 100, "Reading the sound");
      const auto written = speech::WriteSpeechWav(*source, RationalTime(0, 1), facts.duration, wav, context.cancel_token(),
                                                  [&](double done) { context.Progress(static_cast<std::uint64_t>(done * 8.0), 100, "Reading the sound"); });
      if (!written) {
        std::filesystem::remove_all(scratch, ignored);
        finish({});
        return;
      }
      source.reset();
      auto result = speech::Transcribe(tools, wav, scratch + "/words", options, context.cancel_token(),
                                       [&](double done) { context.Progress(8 + static_cast<std::uint64_t>(done * 92.0), 100, "Listening"); });
      std::filesystem::remove_all(scratch, ignored);
      if (result.cancelled || context.cancelled()) {
        finish({});
        return;
      }
      auto transcript = std::move(result.transcript);
      transcript.media_id = media;
      transcript.fingerprint = facts.fingerprint;
      transcript.duration = std::max(transcript.duration, facts.duration);
      if (!sidecar.empty() && !speech::Save(sidecar, transcript)) throw std::runtime_error("The transcript could not be saved next to the project");
      const auto count = transcript.words.size();
      finish([this, media, transcript = std::move(transcript), count]() mutable {
        tx_cache_[media] = std::move(transcript);
        ++tx_revision_;
        ShowStatus(tr("Transcript ready: %1 words").arg(count));
      });
    } catch (...) {
      std::filesystem::remove_all(scratch, ignored);
      finish({});
      throw;
    }
  });
}

bool Session::transcriptImport(const QString& path_or_url) {
  if (store_ == nullptr || tx_media_.empty()) {
    ShowStatus(tr("Select a clip to attach the transcript to its media"));
    return false;
  }
  const QUrl url(path_or_url);
  const auto path = url.isLocalFile() ? url.toLocalFile() : path_or_url;
  std::ifstream in(path.toStdString(), std::ios::binary);
  if (!in) {
    ShowStatus(tr("The transcript file could not be read"));
    return false;
  }
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  speech::Transcript transcript;
  try {
    transcript = speech::FromJson(text);
  } catch (const std::exception&) {
    try {
      transcript = speech::ParseWhisperJson(text);
    } catch (const std::exception& error) {
      ShowStatus(tr("That is not a transcript Cutline can read: %1").arg(QString::fromStdString(error.what())));
      return false;
    }
  }
  const auto facts = MediaFactsOf(tx_media_);
  transcript.media_id = tx_media_;
  transcript.fingerprint = facts.fingerprint;
  if (transcript.words.empty()) {
    ShowStatus(tr("The transcript has no words"));
    return false;
  }
  tx_cache_[tx_media_] = std::move(transcript);
  (void)SaveTranscript(tx_media_);
  RefreshTranscript(true);
  ShowStatus(tr("Transcript imported: %1 words").arg(tx_cache_[tx_media_].words.size()));
  return true;
}

// ------------------------------------------------------------------------------------ editing ----

std::vector<std::size_t> Session::PositionsOf(int first, int last) const {
  std::vector<std::size_t> positions;
  if (first > last) std::swap(first, last);
  for (std::size_t p = 0; p < tx_map_.words.size(); ++p) {
    const auto word = static_cast<int>(tx_map_.words[p].word);
    if (word >= first && word <= last) positions.push_back(p);
  }
  return positions;
}

bool Session::transcriptDelete(int first, int last, bool ripple) {
  const auto positions = PositionsOf(first, last);
  if (positions.empty()) {
    ShowStatus(tr("Those words are not on the timeline"));
    return false;
  }
  ui::DeleteWordsOptions options;
  options.ripple = ripple;
  options.keep_seconds = prefs_.GetReal("speech.cut_margin");
  return Apply(ui::PlanDeleteWords(EditContextFor(), tx_map_, positions, options));
}

int Session::transcriptFillerCount(bool phrases) const {
  const auto found = tx_cache_.find(tx_media_);
  if (found == tx_cache_.end()) return 0;
  speech::FillerOptions options;
  options.phrases = phrases;
  return static_cast<int>(ui::MappedIndices(tx_map_, speech::FindFillers(found->second, options)).size());
}

int Session::transcriptRemoveFillers(bool phrases, bool ripple) {
  const auto found = tx_cache_.find(tx_media_);
  if (found == tx_cache_.end()) return 0;
  speech::FillerOptions fillers;
  fillers.phrases = phrases;
  const auto positions = ui::MappedIndices(tx_map_, speech::FindFillers(found->second, fillers));
  if (positions.empty()) {
    ShowStatus(tr("No hesitation sounds were found on the timeline"));
    return 0;
  }
  ui::DeleteWordsOptions options;
  options.ripple = ripple;
  options.keep_seconds = prefs_.GetReal("speech.cut_margin");
  auto plan = ui::PlanDeleteWords(EditContextFor(), tx_map_, positions, options);
  plan.label = "Remove Fillers";
  return Apply(plan) ? static_cast<int>(positions.size()) : 0;
}

int Session::transcriptPauseCount(double minimum, double keep) const {
  const auto* sequence = graph_.root();
  if (sequence == nullptr) return 0;
  return static_cast<int>(ui::PauseRanges(tx_map_, RationalTime(sequence->frame_rate.denominator, sequence->frame_rate.numerator), minimum, keep).size());
}

int Session::transcriptRemovePauses(double minimum, double keep) {
  const auto count = transcriptPauseCount(minimum, keep);
  if (count == 0) {
    ShowStatus(tr("There is no pause that long between words"));
    return 0;
  }
  return Apply(ui::PlanRemovePauses(EditContextFor(), tx_map_, minimum, keep)) ? count : 0;
}

bool Session::transcriptCaptions(int max_line_chars, int max_lines) {
  const auto found = tx_cache_.find(tx_media_);
  const auto* sequence = graph_.root();
  if (found == tx_cache_.end() || sequence == nullptr) {
    ShowStatus(tr("There is no transcript to make captions from"));
    return false;
  }
  ui::CaptionPlanOptions options;
  options.grouping.max_line_chars = std::clamp(max_line_chars, 10, 80);
  options.grouping.max_lines = std::clamp(max_lines, 1, 3);
  for (const auto& track : sequence->caption_tracks) {
    if (track.name == options.track_name) options.track_id = track.id;
  }
  return Apply(ui::PlanCaptions(EditContextFor(), tx_map_, found->second, options));
}

// -------------------------------------------------------------------------- finding, correcting ----

int Session::transcriptFind(const QString& query) {
  tx_hits_.clear();
  tx_hit_ = -1;
  if (const auto found = tx_cache_.find(tx_media_); found != tx_cache_.end()) {
    for (const auto& range : speech::Find(found->second, query.toStdString())) tx_hits_ << QVariantMap{{"first", static_cast<int>(range.first)}, {"count", static_cast<int>(range.count)}};
  }
  emit transcriptHitsChanged();
  return tx_hits_.size();
}

void Session::transcriptClearFind() { (void)transcriptFind({}); }

bool Session::transcriptSeek(int word) {
  const auto at = transport_.position();
  const ui::SequenceWord* chosen = nullptr;
  for (const auto& mapped : tx_map_.words) {
    if (static_cast<int>(mapped.word) != word) continue;
    if (chosen == nullptr) chosen = &mapped;
    if (mapped.start.Compare(at) > 0) {
      chosen = &mapped;
      break;
    }
  }
  if (chosen == nullptr) {
    ShowStatus(tr("That word is not on the timeline"));
    return false;
  }
  seek(SecondsOf(chosen->start));
  return true;
}

bool Session::transcriptNextHit(int direction) {
  if (tx_hits_.isEmpty()) return false;
  tx_hit_ = (tx_hit_ + (direction >= 0 ? 1 : -1) + tx_hits_.size()) % tx_hits_.size();
  if (tx_hit_ < 0) tx_hit_ = 0;
  return transcriptSeek(tx_hits_[tx_hit_].toMap()["first"].toInt());
}

bool Session::transcriptSetText(int word, const QString& text) {
  const auto found = tx_cache_.find(tx_media_);
  const auto trimmed = text.trimmed();
  if (found == tx_cache_.end() || word < 0 || word >= static_cast<int>(found->second.words.size()) || trimmed.isEmpty()) return false;
  found->second.words[static_cast<std::size_t>(word)].text = trimmed.toStdString();
  (void)SaveTranscript(tx_media_);
  RefreshTranscript(true);
  return true;
}

bool Session::transcriptSetSpeaker(int first, int last, const QString& name) {
  const auto found = tx_cache_.find(tx_media_);
  if (found == tx_cache_.end() || found->second.words.empty()) return false;
  auto& transcript = found->second;
  if (first > last) std::swap(first, last);
  first = std::max(first, 0);
  last = std::min(last, static_cast<int>(transcript.words.size()) - 1);
  if (first > last) return false;
  int index = -1;
  const auto speaker = name.trimmed().toStdString();
  if (!speaker.empty()) {
    const auto it = std::find(transcript.speakers.begin(), transcript.speakers.end(), speaker);
    index = static_cast<int>(std::distance(transcript.speakers.begin(), it));
    if (it == transcript.speakers.end()) transcript.speakers.push_back(speaker);
  }
  for (int i = first; i <= last; ++i) transcript.words[static_cast<std::size_t>(i)].speaker = index;
  (void)SaveTranscript(tx_media_);
  RefreshTranscript(true);
  return true;
}

QString Session::transcriptPlainText() const {
  QString text;
  for (const auto& paragraph : tx_paragraphs_) {
    const auto map = paragraph.toMap();
    QStringList words;
    for (const auto& word : map["words"].toList()) words << word.toMap()["t"].toString();
    if (!map["speaker"].toString().isEmpty()) text += map["speaker"].toString() + ": ";
    text += words.join(' ') + "\n\n";
  }
  return text.trimmed();
}

void Session::showTranscript() { emit workspaceCommand("panelshow.transcript"); }

}  // namespace cutline::app
