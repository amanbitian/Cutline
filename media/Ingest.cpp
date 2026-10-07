#include "media/Ingest.h"

#include "core/db/Sql.h"
#include "core/util/Sha256.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace cutline::media {
namespace {

// Reads up to `size` bytes at `offset`, returning what was actually read.
[[nodiscard]] std::vector<std::byte> ReadSpan(std::ifstream& file, std::uintmax_t offset, std::size_t size) {
  file.clear();
  file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  std::vector<std::byte> span(size);
  file.read(reinterpret_cast<char*>(span.data()), static_cast<std::streamsize>(size));
  span.resize(static_cast<std::size_t>(file.gcount()));
  return span;
}

}  // namespace

std::string FingerprintFile(const std::string& path, FingerprintOptions options) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) throw std::runtime_error("Unable to measure " + path + ": " + error.message());

  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("Unable to read " + path);

  util::Sha256 hash;
  // The size goes in first: two files that happen to share their sampled spans
  // but differ in length must not collide.
  hash.Update("cutline-media-v1:");
  hash.Update(std::to_string(size));

  const auto span = static_cast<std::uintmax_t>(options.span_bytes);
  if (size <= span * 3) {
    // Small enough to hash whole, which is both cheaper and stronger.
    const auto whole = ReadSpan(file, 0, static_cast<std::size_t>(size));
    hash.Update(whole.data(), whole.size());
    return hash.HexDigest();
  }

  // Head, middle, and tail. The head alone is a poor discriminator: files from
  // one camera share long identical headers.
  for (const std::uintmax_t offset : {std::uintmax_t{0}, (size - span) / 2, size - span}) {
    const auto chunk = ReadSpan(file, offset, options.span_bytes);
    hash.Update(chunk.data(), chunk.size());
  }
  return hash.HexDigest();
}

[[nodiscard]] static bool HasStreams(const project::ProjectStore& store, const std::string& media_id) {
  const std::lock_guard<std::mutex> lock(store.mutex());
  db::Statement statement(store.connection(), "SELECT COUNT(*) FROM media_streams WHERE media_id = ?;");
  statement.Bind(1, media_id);
  return statement.Step() && statement.ColumnInt(0) > 0;
}

std::optional<std::string> FindByFingerprint(const project::ProjectStore& store, const std::string& fingerprint) {
  const std::lock_guard<std::mutex> lock(store.mutex());
  db::Statement statement(store.connection(), "SELECT id FROM media WHERE fingerprint = ? LIMIT 1;");
  statement.Bind(1, fingerprint);
  if (!statement.Step()) return std::nullopt;
  return statement.ColumnText(0);
}

IngestResult IngestFile(project::ProjectStore& store, const IngestRequest& request) {
  if (request.media_id.empty()) throw std::invalid_argument("Ingest needs a media id");
  if (request.path.empty()) throw std::invalid_argument("Ingest needs a path");

  IngestResult result;
  result.media_id = request.media_id;

  // Probing first means a file that cannot be read never reaches the journal.
  result.probe = SourceRegistry::Instance().ProbeFile(request.path);
  if (result.probe.duration.Compare({0, 1}) <= 0) {
    throw std::runtime_error("Refusing to import " + request.path + ": it reports no duration");
  }

  // Synthetic media has no file behind it, so its path is its identity.
  std::error_code error;
  result.fingerprint = std::filesystem::is_regular_file(request.path, error)
                           ? FingerprintFile(request.path)
                           : util::Sha256::Of("cutline-virtual-v1:" + request.path);

  if (const auto existing = FindByFingerprint(store, result.fingerprint); existing.has_value()) {
    result.media_id = *existing;
    result.already_present = true;
    result.revision = store.CurrentRevision();
    // An item imported by an older version, or interrupted between its two
    // commands, can exist with no streams. Finding it again must repair it, not
    // report success and leave it undescribable.
    if (!result.probe.streams.empty() && !HasStreams(store, *existing)) {
      commands::SetMediaStreamsPayload streams;
      streams.media_id = *existing;
      streams.streams = result.probe.streams;
      commands::CommandEnvelope repair;
      repair.command_id = "ingest-repair-" + *existing + "-" + std::to_string(result.revision);
      repair.project_id = request.project_id;
      repair.author_id = request.author_id;
      repair.base_revision = result.revision;
      repair.timestamp_utc = request.timestamp_utc;
      repair.type = commands::CommandType::SetMediaStreams;
      repair.payload = streams;
      repair.idempotency_key = repair.command_id;
      result.revision = store.Execute(repair).revision;
      result.repaired = true;
    }
    return result;
  }

  const auto name = request.display_name.empty()
                        ? std::filesystem::path(request.path).filename().string()
                        : request.display_name;

  commands::ImportMediaPayload import;
  import.id = request.media_id;
  import.bin_id = request.bin_id;
  import.display_name = name.empty() ? request.media_id : name;
  import.original_path = request.path;
  import.fingerprint = result.fingerprint;
  import.duration = result.probe.duration;
  import.start_timecode = result.probe.start_timecode;
  import.streams = result.probe.streams;

  commands::CommandEnvelope envelope;
  envelope.command_id = "ingest-" + request.media_id;
  envelope.project_id = request.project_id;
  envelope.author_id = request.author_id;
  envelope.base_revision = request.base_revision;
  envelope.timestamp_utc = request.timestamp_utc;
  envelope.type = commands::CommandType::ImportMedia;
  envelope.payload = import;
  envelope.idempotency_key = "ingest-" + result.fingerprint;
  result.revision = store.Execute(envelope).revision;

  return result;
}

}  // namespace cutline::media
