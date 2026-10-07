#pragma once

// Bringing a file into a project.
//
// Ingest is the step between "a path on disk" and "media a sequence can cut":
// probe it, fingerprint it, and record both as commands so the import is in the
// journal and is undoable like any other edit.
//
// The fingerprint is sampled rather than whole-file. Hashing every byte of a
// 200 GB camera card to notice a file has moved is not a trade anyone wants;
// hashing the size plus three spans is enough to tell two files apart while
// staying constant-time in the file's length.

#include "core/project/ProjectStore.h"
#include "media/Source.h"

#include <cstdint>
#include <optional>
#include <string>

namespace cutline::media {

struct FingerprintOptions final {
  // Bytes read from the head, the middle, and the tail.
  std::size_t span_bytes{64 * 1024};
};

// Hashes the file's size and three sampled spans. Throws if the file cannot be
// read, because an unreadable file should not be imported as if it were fine.
[[nodiscard]] std::string FingerprintFile(const std::string& path, FingerprintOptions options = {});

struct IngestRequest final {
  std::string project_id;
  std::string author_id;
  std::string timestamp_utc;
  // Identity to give the media. The caller supplies it so the command is
  // reproducible and the id can be chosen to match an external asset system.
  std::string media_id;
  std::optional<std::string> bin_id;
  std::string path;
  // Display name; the file's own name is used when this is empty.
  std::string display_name;
  // Base revision for the first command; later ones follow on from it.
  std::int64_t base_revision{0};
};

struct IngestResult final {
  std::string media_id;
  std::string fingerprint;
  Probe probe;
  std::int64_t revision{0};
  // True when the fingerprint matched media already in the project, in which
  // case nothing was imported and `media_id` names the existing item. Importing
  // the same file twice should not produce two library entries.
  bool already_present{false};
  // True when an existing item found by fingerprint had no stream records and
  // they were written now.
  bool repaired{false};
};

// Probes, fingerprints, and records the media in one command, streams included,
// so it is journalled and undone as a unit and cannot be left half-imported.
[[nodiscard]] IngestResult IngestFile(project::ProjectStore& store, const IngestRequest& request);

// Looks for media in the project whose fingerprint matches the file, which is
// how relink finds the right item after footage has moved.
[[nodiscard]] std::optional<std::string> FindByFingerprint(const project::ProjectStore& store,
                                                           const std::string& fingerprint);

}  // namespace cutline::media
