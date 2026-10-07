#include "exporter/ExportQueue.h"

#include "core/util/Json.h"
#include "core/util/JsonParse.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace cutline::exporter {
namespace {

std::string NowUtc() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &time);
#else
  gmtime_r(&time, &utc);
#endif
  std::ostringstream out;
  out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

std::string TimeJson(const time::RationalTime& t) {
  return json::Object().Add("n", t.numerator()).Add("d", t.denominator()).Build();
}

time::RationalTime ReadTime(const json::Value& value) {
  return time::RationalTime(value.Integer("n"), std::max<std::int64_t>(1, value.Integer("d")));
}

std::string Strings(const std::vector<std::string>& list) {
  std::vector<std::string> quoted;
  for (const auto& item : list) quoted.push_back("\"" + json::Escape(item) + "\"");
  return json::Array(quoted);
}

std::vector<std::string> ReadStrings(const json::Value* value) {
  std::vector<std::string> list;
  if (value == nullptr || !value->is_array()) return list;
  for (const auto& item : value->items) {
    if (item.is_string()) list.push_back(item.text);
  }
  return list;
}

// Half-written exports of a job that did not finish: the writer removes its own, but a crash cannot.
void RemoveStaleParts(const std::string& output_path) {
  if (output_path.empty()) return;
  std::error_code error;
  const std::filesystem::path path(output_path);
  const auto folder = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
  const auto prefix = path.filename().string() + ".cutline-";
  for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
    const auto name = entry.path().filename().string();
    if (name.rfind(prefix, 0) == 0 && entry.path().extension() == ".part") std::filesystem::remove(entry.path(), error);
  }
}

}  // namespace

const char* ToString(JobState state) {
  switch (state) {
    case JobState::Queued: return "queued";
    case JobState::Running: return "running";
    case JobState::Done: return "done";
    case JobState::Failed: return "failed";
    case JobState::Cancelled: return "cancelled";
    case JobState::Interrupted: return "interrupted";
  }
  return "queued";
}

std::optional<JobState> ParseJobState(const std::string& text) {
  for (const auto state : {JobState::Queued, JobState::Running, JobState::Done, JobState::Failed, JobState::Cancelled, JobState::Interrupted}) {
    if (text == ToString(state)) return state;
  }
  return std::nullopt;
}

ExportQueue::ExportQueue(std::string state_file, JobExecutor executor, bool start_paused)
    : state_file_(std::move(state_file)), executor_(std::move(executor)), paused_(start_paused) {
  Load();
  worker_ = std::thread([this] { Run(); });
}

ExportQueue::~ExportQueue() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    cancel_running_ = true;
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

ExportJob* ExportQueue::FindUnlocked(const std::string& id) {
  for (auto& job : jobs_) {
    if (job.id == id) return &job;
  }
  return nullptr;
}

std::string ExportQueue::Add(ExportJob job) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (job.id.empty()) {
    std::ostringstream id;
    id << "export-" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() << "-" << ++counter_;
    job.id = id.str();
  }
  if (job.created.empty()) job.created = NowUtc();
  job.state = JobState::Queued;
  job.frames_done = 0;
  job.error.clear();
  const auto id = job.id;
  jobs_.push_back(std::move(job));
  Changed();
  return id;
}

bool ExportQueue::Cancel(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto* job = FindUnlocked(id);
  if (job == nullptr) return false;
  if (job->state == JobState::Queued || job->state == JobState::Interrupted) {
    job->state = JobState::Cancelled;
    job->finished = NowUtc();
    Changed();
    return true;
  }
  if (job->state == JobState::Running) {
    cancel_running_ = true;
    Changed();
    return true;
  }
  return false;
}

bool ExportQueue::Remove(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = std::find_if(jobs_.begin(), jobs_.end(), [&](const ExportJob& job) { return job.id == id; });
  if (found == jobs_.end() || found->state == JobState::Running) return false;
  jobs_.erase(found);
  Changed();
  return true;
}

bool ExportQueue::Retry(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto* job = FindUnlocked(id);
  if (job == nullptr || !(job->state == JobState::Failed || job->state == JobState::Cancelled || job->state == JobState::Interrupted)) return false;
  // To the end of the queue: a retried job does not jump the ones that were waiting.
  ExportJob copy = *job;
  copy.state = JobState::Queued;
  copy.frames_done = 0;
  copy.error.clear();
  copy.problems.clear();
  copy.finished.clear();
  copy.started.clear();
  jobs_.erase(std::find_if(jobs_.begin(), jobs_.end(), [&](const ExportJob& candidate) { return candidate.id == id; }));
  jobs_.push_back(std::move(copy));
  Changed();
  return true;
}

bool ExportQueue::Move(const std::string& id, int position) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = std::find_if(jobs_.begin(), jobs_.end(), [&](const ExportJob& job) { return job.id == id; });
  if (found == jobs_.end() || found->state != JobState::Queued) return false;
  // Positions count the waiting jobs only; the finished ones keep their place in the list around them.
  std::vector<std::size_t> waiting;
  for (std::size_t i = 0; i < jobs_.size(); ++i) {
    if (jobs_[i].state == JobState::Queued) waiting.push_back(i);
  }
  const auto from = static_cast<std::size_t>(found - jobs_.begin());
  const auto target = waiting[static_cast<std::size_t>(std::clamp(position, 0, static_cast<int>(waiting.size()) - 1))];
  if (from == target) return true;
  ExportJob moved = std::move(*found);
  jobs_.erase(found);
  const auto index = from < target ? target : target;
  jobs_.insert(jobs_.begin() + static_cast<std::ptrdiff_t>(index), std::move(moved));
  Changed();
  return true;
}

void ExportQueue::Pause() {
  std::lock_guard<std::mutex> lock(mutex_);
  paused_ = true;
  Changed();
}

void ExportQueue::Resume() {
  std::lock_guard<std::mutex> lock(mutex_);
  paused_ = false;
  Changed();
}

bool ExportQueue::paused() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return paused_;
}

void ExportQueue::ClearFinished() {
  std::lock_guard<std::mutex> lock(mutex_);
  jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [](const ExportJob& job) { return job.finished_state(); }), jobs_.end());
  Changed();
}

std::vector<ExportJob> ExportQueue::Jobs() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return jobs_;
}

std::optional<ExportJob> ExportQueue::Find(const std::string& id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& job : jobs_) {
    if (job.id == id) return job;
  }
  return std::nullopt;
}

void ExportQueue::SetListener(std::function<void()> listener) {
  std::lock_guard<std::mutex> lock(mutex_);
  listener_ = std::move(listener);
}

bool ExportQueue::WaitIdle(std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lock(mutex_);
  const auto busy = [this] {
    return std::any_of(jobs_.begin(), jobs_.end(), [](const ExportJob& job) { return job.state == JobState::Running || job.state == JobState::Queued; });
  };
  // A paused queue with waiting jobs is not going to become idle by waiting.
  return idle_.wait_for(lock, timeout, [&] { return !busy() || (paused_ && running_id_.empty()); }) && !busy();
}

void ExportQueue::Changed() {
  Save();
  wake_.notify_all();
  idle_.notify_all();
  if (listener_) {
    // The listener may call back into the queue (to read the jobs); it is called without the lock held.
    auto listener = listener_;
    mutex_.unlock();
    try {
      listener();
    } catch (...) {
    }
    mutex_.lock();
  }
}

void ExportQueue::Run() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (true) {
    wake_.wait(lock, [this] {
      return stopping_ || (!paused_ && std::any_of(jobs_.begin(), jobs_.end(), [](const ExportJob& job) { return job.state == JobState::Queued; }));
    });
    if (stopping_) return;
    auto* next = static_cast<ExportJob*>(nullptr);
    for (auto& job : jobs_) {
      if (job.state == JobState::Queued) {
        next = &job;
        break;
      }
    }
    if (next == nullptr) continue;
    next->state = JobState::Running;
    next->started = NowUtc();
    next->finished.clear();
    next->frames_done = 0;
    running_id_ = next->id;
    cancel_running_ = false;
    const auto id = next->id;
    const ExportJob snapshot = *next;
    Changed();

    JobOutcome outcome;
    std::string failure;
    bool cancelled = false;
    const auto started = std::chrono::steady_clock::now();
    lock.unlock();
    try {
      outcome = executor_(snapshot, [this, &id](std::int64_t done, std::int64_t total) {
        std::lock_guard<std::mutex> inner(mutex_);
        if (auto* job = FindUnlocked(id)) {
          job->frames_done = done;
          job->frames_total = total;
        }
        const bool keep_going = !cancel_running_;
        // Progress is not written to disk on every frame; a change of state is.
        if (listener_) {
          auto listener = listener_;
          mutex_.unlock();
          try {
            listener();
          } catch (...) {
          }
          mutex_.lock();
        }
        return keep_going;
      });
    } catch (const std::exception& error) {
      failure = error.what();
    }
    lock.lock();

    cancelled = cancel_running_;
    auto* job = FindUnlocked(id);
    if (job != nullptr) {
      job->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
      job->finished = NowUtc();
      job->encoder = outcome.encoder.empty() ? job->encoder : outcome.encoder;
      job->hardware = outcome.hardware;
      job->notes = outcome.notes;
      job->problems = outcome.problems;
      job->output_bytes = outcome.output_bytes;
      if (!failure.empty()) {
        job->state = JobState::Failed;
        job->error = failure;
      } else if (cancelled && stopping_) {
        // The application is closing: this job is to be run again, not forgotten.
        job->state = JobState::Interrupted;
        job->error = "The application closed while this was exporting.";
        RemoveStaleParts(job->output_path);
      } else if (cancelled) {
        job->state = JobState::Cancelled;
        job->error.clear();
      } else {
        job->state = JobState::Done;
        job->frames_done = std::max(job->frames_done, outcome.frames);
        if (job->frames_total <= 0) job->frames_total = job->frames_done;
        job->error.clear();
      }
    }
    running_id_.clear();
    cancel_running_ = false;
    Changed();
    if (stopping_) return;
  }
}

// ------------------------------------------------------------------- the record ----

void ExportQueue::Save() {
  if (state_file_.empty()) return;
  std::vector<std::string> jobs;
  for (const auto& job : jobs_) {
    jobs.push_back(json::Object()
                       .Add("id", job.id)
                       .Add("name", job.name)
                       .Add("sequence", job.sequence_id)
                       .Add("preset", job.preset_id)
                       .Add("output", job.output_path)
                       .Add("overwrite", job.overwrite)
                       .Add("hardware_preferred", job.prefer_hardware)
                       .AddRaw("in", TimeJson(job.in))
                       .AddRaw("out", TimeJson(job.out))
                       .Add("state", ToString(job.state))
                       .Add("frames_done", job.frames_done)
                       .Add("frames_total", job.frames_total)
                       .Add("error", job.error)
                       .Add("encoder", job.encoder)
                       .Add("hardware", job.hardware)
                       .AddRaw("notes", Strings(job.notes))
                       .AddRaw("problems", Strings(job.problems))
                       .Add("bytes", job.output_bytes)
                       .Add("seconds", job.seconds)
                       .Add("created", job.created)
                       .Add("started", job.started)
                       .Add("finished", job.finished)
                       .Build());
  }
  const auto text = json::Object().Add("version", std::int64_t{1}).AddRaw("jobs", json::Array(jobs)).Build();
  try {
    const std::filesystem::path path(state_file_);
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    const auto temporary = state_file_ + ".tmp";
    {
      std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
      out << text;
      out.flush();
      if (!out) return;
    }
    std::error_code error;
    std::filesystem::rename(temporary, state_file_, error);
  } catch (const std::exception&) {
    // A queue that cannot be saved still runs; it just will not survive a restart.
  }
}

void ExportQueue::Load() {
  if (state_file_.empty()) return;
  std::ifstream in(state_file_, std::ios::binary);
  if (!in) return;
  std::stringstream buffer;
  buffer << in.rdbuf();
  in.close();   // a file held open cannot be renamed (the damaged-record path below)
  try {
    const auto root = json::Parse(buffer.str());
    const auto* list = root.Find("jobs");
    if (list == nullptr || !list->is_array()) return;
    for (const auto& entry : list->items) {
      ExportJob job;
      job.id = entry.String("id");
      job.name = entry.String("name");
      job.sequence_id = entry.String("sequence");
      job.preset_id = entry.String("preset");
      job.output_path = entry.String("output");
      job.overwrite = entry.Bool("overwrite");
      job.prefer_hardware = entry.Bool("hardware_preferred");
      job.in = ReadTime(entry.Require("in"));
      job.out = ReadTime(entry.Require("out"));
      job.state = ParseJobState(entry.String("state")).value_or(JobState::Failed);
      job.frames_done = entry.Integer("frames_done");
      job.frames_total = entry.Integer("frames_total");
      job.error = entry.String("error");
      job.encoder = entry.String("encoder");
      job.hardware = entry.Bool("hardware");
      job.notes = ReadStrings(entry.Find("notes"));
      job.problems = ReadStrings(entry.Find("problems"));
      job.output_bytes = entry.Integer("bytes");
      job.seconds = entry.Number("seconds");
      job.created = entry.String("created");
      job.started = entry.String("started");
      job.finished = entry.String("finished");
      if (job.state == JobState::Running) {
        // The record says it was running, so the application died or was killed during it.
        job.state = JobState::Interrupted;
        job.error = "The application closed while this was exporting.";
        RemoveStaleParts(job.output_path);
      }
      jobs_.push_back(std::move(job));
    }
  } catch (const std::exception&) {
    // A damaged record is kept aside rather than overwritten, so nothing the person queued is silently lost.
    std::error_code error;
    std::filesystem::rename(state_file_, state_file_ + ".damaged", error);
    jobs_.clear();
  }
}

}  // namespace cutline::exporter
