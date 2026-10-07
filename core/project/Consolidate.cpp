#include "core/project/Consolidate.h"

#include "core/db/Sql.h"
#include "core/util/Json.h"
#include "core/util/JsonParse.h"
#include "core/util/Sha256.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace cutline::project {
namespace fs = std::filesystem;
using time::RationalTime;

namespace {

constexpr std::size_t kChunk = 1u << 20;

std::string SafeName(const std::string& text) {
  std::string out;
  for (const char c : text) out.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_' ? c : '_');
  return out.empty() ? "media" : out;
}

// Copies and hashes in one pass.
bool CopyAndHash(const fs::path& from, const fs::path& to, std::string& hash, std::uint64_t& bytes, std::string& error) {
  std::ifstream in(from, std::ios::binary);
  if (!in) {
    error = "cannot be read";
    return false;
  }
  fs::create_directories(to.parent_path());
  std::ofstream out(to, std::ios::binary | std::ios::trunc);
  if (!out) {
    error = "the copy cannot be written";
    return false;
  }
  util::Sha256 sha;
  std::vector<char> buffer(kChunk);
  bytes = 0;
  while (in) {
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto got = static_cast<std::size_t>(in.gcount());
    if (got == 0) break;
    sha.Update(reinterpret_cast<const std::byte*>(buffer.data()), got);
    out.write(buffer.data(), static_cast<std::streamsize>(got));
    if (!out) {
      error = "the copy ran out of room or failed to write";
      return false;
    }
    bytes += got;
  }
  out.flush();
  if (!out) {
    error = "the copy failed to write";
    return false;
  }
  if (in.bad()) {
    error = "reading stopped partway";
    return false;
  }
  hash = sha.HexDigest();
  return true;
}

}  // namespace

std::string HashFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  util::Sha256 sha;
  std::vector<char> buffer(kChunk);
  while (in) {
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto got = static_cast<std::size_t>(in.gcount());
    if (got == 0) break;
    sha.Update(reinterpret_cast<const std::byte*>(buffer.data()), got);
  }
  if (in.bad()) return {};
  return sha.HexDigest();
}

std::vector<MediaUse> PlanConsolidation(const ProjectStore& store, const std::vector<std::string>& sequence_ids) {
  const std::lock_guard<std::mutex> lock(store.mutex());
  auto* database = store.connection();

  // The sequences chosen, and every sequence they nest, to any depth.
  std::set<std::string> selected;
  std::vector<std::string> pending;
  if (sequence_ids.empty()) {
    db::Statement all(database, "SELECT id FROM sequences ORDER BY id;");
    while (all.Step()) pending.push_back(all.ColumnText(0));
  } else {
    pending = sequence_ids;
  }
  while (!pending.empty()) {
    const auto id = pending.back();
    pending.pop_back();
    if (!selected.insert(id).second) continue;
    db::Statement nested(database, R"sql(
      SELECT DISTINCT c.nested_sequence_id FROM clips c JOIN tracks t ON t.id = c.track_id
       WHERE t.sequence_id = ? AND c.nested_sequence_id IS NOT NULL;
    )sql");
    nested.Bind(1, id);
    while (nested.Step()) pending.push_back(nested.ColumnText(0));
  }

  std::map<std::string, RationalTime> used;
  for (const auto& id : selected) {
    db::Statement clips(database, R"sql(
      SELECT c.media_id, c.source_in_num, c.source_in_den, c.source_out_num, c.source_out_den
        FROM clips c JOIN tracks t ON t.id = c.track_id
       WHERE t.sequence_id = ? AND c.media_id IS NOT NULL;
    )sql");
    clips.Bind(1, id);
    while (clips.Step()) {
      const auto length = RationalTime(clips.ColumnInt(3), clips.ColumnInt(4)).Subtract(RationalTime(clips.ColumnInt(1), clips.ColumnInt(2)));
      const auto media_id = clips.ColumnText(0);
      const auto found = used.find(media_id);
      used[media_id] = found == used.end() ? length : found->second.Add(length);
    }
  }

  std::vector<MediaUse> plan;
  db::Statement media(database, "SELECT id, display_name, original_path, fingerprint, duration_num, duration_den FROM media ORDER BY id;");
  while (media.Step()) {
    MediaUse use;
    use.id = media.ColumnText(0);
    use.name = media.ColumnText(1);
    use.source = fs::path(media.ColumnText(2));
    use.fingerprint = media.ColumnText(3);
    use.duration = RationalTime(media.ColumnInt(4), media.ColumnInt(5));
    const auto found = used.find(use.id);
    use.used_by_selection = found != used.end();
    if (use.used_by_selection) use.used = found->second;
    plan.push_back(std::move(use));
  }
  return plan;
}

std::string ConsolidationReport::ToText() const {
  std::ostringstream out;
  out << media.size() << " media item" << (media.size() == 1 ? "" : "s") << " collected, " << bytes_copied << " bytes written.\n";
  for (const auto& entry : media) {
    out << "  " << entry.id << " (" << entry.name << "): " << entry.status;
    if (!entry.sha256.empty()) out << ", sha256 " << entry.sha256.substr(0, 16) << "...";
    out << "\n";
  }
  for (const auto& id : missing) out << "MISSING: " << id << " could not be found, so it was not collected\n";
  for (const auto& id : failed) out << "FAILED: " << id << " could not be copied exactly\n";
  for (const auto& id : not_copied) out << "left out: " << id << " is not used by the chosen sequences\n";
  for (const auto& note : notes) out << "note: " << note << "\n";
  if (cancelled) out << "CANCELLED before everything was collected\n";
  out << (complete() ? "Every media item the project uses is in the folder and verified.\n" : "The folder is NOT a complete copy of the project.\n");
  return out.str();
}

ConsolidationReport Consolidate(ProjectStore& store, const ConsolidateOptions& options) {
  ConsolidationReport report;
  if (options.destination.empty()) throw std::invalid_argument("Consolidation needs a destination");
  const auto plan = PlanConsolidation(store, options.sequence_ids);
  fs::create_directories(options.destination);
  const auto destination = fs::absolute(options.destination);

  std::vector<const MediaUse*> to_copy;
  for (const auto& use : plan) {
    if (use.used_by_selection || options.include_unused_media) to_copy.push_back(&use);
    else report.not_copied.push_back(use.id);
  }

  std::size_t done = 0;
  for (const auto* use : to_copy) {
    if (options.cancel && options.cancel()) {
      report.cancelled = true;
      break;
    }
    ConsolidatedMedia entry;
    entry.id = use->id;
    entry.name = use->name;
    entry.source = use->source;
    entry.used = use->used_by_selection;
    entry.copy = fs::path(options.media_directory) / (SafeName(use->id) + "-" + SafeName(use->source.filename().string()));
    const auto target = destination / entry.copy;

    std::error_code ignore;
    if (!fs::exists(use->source, ignore)) {
      entry.status = "the file is not there";
      report.missing.push_back(use->id);
      report.media.push_back(std::move(entry));
      if (options.progress) options.progress(++done, to_copy.size());
      continue;
    }

    std::string reason;
    if (options.transcode) {
      fs::create_directories(target.parent_path());
      if (options.transcode(*use, target, reason)) {
        entry.transcoded = true;
        entry.sha256 = HashFile(target);
        entry.bytes = fs::exists(target, ignore) ? static_cast<std::uint64_t>(fs::file_size(target, ignore)) : 0;
        entry.verified = !entry.sha256.empty();
        entry.status = entry.verified ? "transcoded" : "the transcoded file could not be read back";
        if (entry.verified) report.bytes_copied += entry.bytes;
        else report.failed.push_back(use->id);
        report.media.push_back(std::move(entry));
        if (options.progress) options.progress(++done, to_copy.size());
        continue;
      }
      // Declined: an item the callback does not handle is copied as it is.
      if (!reason.empty()) {
        entry.status = "transcoding failed: " + reason;
        fs::remove(target, ignore);
        report.failed.push_back(use->id);
        report.media.push_back(std::move(entry));
        if (options.progress) options.progress(++done, to_copy.size());
        continue;
      }
    }

    // A copy already there from an earlier run counts if it hashes the same as the source.
    std::string source_hash;
    bool reused = false;
    if (fs::exists(target, ignore) && fs::file_size(target, ignore) == fs::file_size(use->source, ignore)) {
      source_hash = HashFile(use->source);
      reused = !source_hash.empty() && HashFile(target) == source_hash;
      if (reused) entry.bytes = static_cast<std::uint64_t>(fs::file_size(target, ignore));
    }
    if (!reused) {
      std::uint64_t bytes = 0;
      if (!CopyAndHash(use->source, target, source_hash, bytes, reason)) {
        fs::remove(target, ignore);
        entry.status = reason;
        report.failed.push_back(use->id);
        report.media.push_back(std::move(entry));
        if (options.progress) options.progress(++done, to_copy.size());
        continue;
      }
      entry.bytes = bytes;
      // The copy is read back: what is on disk, not what was written, is what has to match.
      if (HashFile(target) != source_hash) {
        fs::remove(target, ignore);
        entry.status = "the copy does not match the original";
        report.failed.push_back(use->id);
        report.media.push_back(std::move(entry));
        if (options.progress) options.progress(++done, to_copy.size());
        continue;
      }
      report.bytes_copied += bytes;
    }
    entry.sha256 = source_hash;
    entry.verified = true;
    entry.status = reused ? "already there, verified" : "copied, verified";
    report.media.push_back(std::move(entry));
    if (options.progress) options.progress(++done, to_copy.size());
  }

  // The manifest: what is in the folder, where, and what it hashes to.
  {
    std::vector<std::string> items;
    for (const auto& entry : report.media) {
      if (!entry.verified) continue;
      items.push_back(json::Object()
                          .Add("id", entry.id)
                          .Add("name", entry.name)
                          .Add("path", entry.copy.generic_string())
                          .Add("bytes", static_cast<std::int64_t>(entry.bytes))
                          .Add("sha256", entry.sha256)
                          .Add("transcoded", entry.transcoded)
                          .Build());
    }
    report.manifest = destination / "manifest.json";
    std::ofstream file(report.manifest, std::ios::binary | std::ios::trunc);
    file << json::Object().Add("version", static_cast<std::int64_t>(1)).Add("project", store.ProjectId()).AddRaw("media", json::Array(items)).Build();
  }

  // The project itself, with its media pointed at the copies.
  if (options.copy_package && !report.cancelled) {
    const auto package = store.PackagePath();
    if (package.empty() || !fs::exists(package / "project.db")) {
      report.notes.push_back("the project has no package on disk to copy; only the media was collected");
    } else {
      const auto copy = destination / "project.cutline";
      {
        // Nothing may commit while the files are copied, and the write-ahead log is folded into the
        // database first so the database file on its own is the whole project.
        const std::lock_guard<std::mutex> lock(store.mutex());
        db::Execute(store.connection(), "PRAGMA wal_checkpoint(TRUNCATE);");
        std::error_code error;
        fs::remove_all(copy, error);
        fs::create_directories(copy);
        fs::copy_file(package / "project.db", copy / "project.db", fs::copy_options::overwrite_existing);
        for (const char* folder : {"journal", "snapshots"}) {
          if (fs::exists(package / folder)) fs::copy(package / folder, copy / folder, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        }
      }
      auto collected = ProjectStore::OpenPackage(copy);
      int counter = 0;
      for (const auto& entry : report.media) {
        if (!entry.verified) continue;
        commands::RelinkMediaPayload relink;
        relink.id = entry.id;
        relink.original_path = (destination / entry.copy).string();
        relink.missing = false;
        commands::CommandEnvelope command;
        command.command_id = "consolidate-" + std::to_string(++counter) + "-" + ProjectStore::GenerateProjectUuid();
        command.project_id = collected->ProjectId();
        command.author_id = "consolidate";
        command.base_revision = collected->CurrentRevision();
        command.timestamp_utc = "2026-01-01T00:00:00Z";
        command.type = commands::CommandType::RelinkMedia;
        command.payload = relink;
        command.idempotency_key = command.command_id;
        (void)collected->Execute(command);
      }
      report.package = copy;
    }
  }
  if (!report.complete() && !report.cancelled) report.notes.push_back("some media could not be collected; the copied project still points at the original files for those");
  return report;
}

ManifestCheck VerifyManifest(const fs::path& manifest, const fs::path& root) {
  ManifestCheck check;
  std::ifstream in(manifest, std::ios::binary);
  if (!in) throw std::runtime_error("The manifest " + manifest.string() + " cannot be read");
  std::stringstream text;
  text << in.rdbuf();
  const auto value = json::Parse(text.str());
  for (const auto& item : value.Require("media").items) {
    ++check.checked;
    const auto path = root / fs::path(item.String("path"));
    if (!fs::exists(path)) {
      check.missing.push_back(item.String("id"));
      continue;
    }
    if (HashFile(path) != item.String("sha256")) check.mismatched.push_back(item.String("id"));
  }
  return check;
}

int RelinkFromManifest(ProjectStore& store, const fs::path& manifest, const fs::path& root) {
  std::ifstream in(manifest, std::ios::binary);
  if (!in) throw std::runtime_error("The manifest " + manifest.string() + " cannot be read");
  std::stringstream text;
  text << in.rdbuf();
  const auto value = json::Parse(text.str());
  int relinked = 0, counter = 0;
  for (const auto& item : value.Require("media").items) {
    const auto path = fs::absolute(root / fs::path(item.String("path")));
    commands::RelinkMediaPayload relink;
    relink.id = item.String("id");
    relink.original_path = path.string();
    relink.missing = !fs::exists(path);
    commands::CommandEnvelope command;
    command.command_id = "relink-" + std::to_string(++counter) + "-" + ProjectStore::GenerateProjectUuid();
    command.project_id = store.ProjectId();
    command.author_id = "consolidate";
    command.base_revision = store.CurrentRevision();
    command.timestamp_utc = "2026-01-01T00:00:00Z";
    command.type = commands::CommandType::RelinkMedia;
    command.payload = relink;
    command.idempotency_key = command.command_id;
    try {
      (void)store.Execute(command);
      if (!relink.missing) ++relinked;
    } catch (const std::exception&) {
      // A manifest entry for media the project does not have: nothing to relink.
    }
  }
  return relinked;
}

}  // namespace cutline::project
