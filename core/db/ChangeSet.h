#pragma once

// Row-level undo/redo built on the SQLite session extension.
//
// A Recorder watches the content tables while a command runs and produces a
// ChangeSet: the exact rows the command inserted, deleted, or updated, with
// their before and after values. Applying the inverse of that changeset undoes
// the command.
//
// This replaces snapshotting the project database per command. The cost of an
// edit's history is now proportional to the rows it touched, so dragging a clip
// costs a few hundred bytes instead of a full copy of the project, and the
// history is bounded by edit volume rather than by project size.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_session;

namespace cutline::db {

// An opaque, serialisable record of row changes. Cheap to move, and safe to
// persist: the blob format is stable across SQLite versions.
class ChangeSet final {
 public:
  ChangeSet() = default;
  explicit ChangeSet(std::vector<std::byte> bytes) : bytes_(std::move(bytes)) {}

  [[nodiscard]] bool empty() const noexcept { return bytes_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
  [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return bytes_; }

  // Returns a changeset that reverses this one. Inserts become deletes, deletes
  // become inserts, and updates swap their before and after values.
  [[nodiscard]] ChangeSet Inverted() const;

  // Applies the changeset to `database`. Must run inside a transaction so a
  // conflict leaves nothing behind.
  //
  // Any conflict aborts: a changeset is only ever applied to the state it was
  // recorded against, so a conflict means the caller's bookkeeping is wrong and
  // silently merging would corrupt the project.
  void ApplyTo(sqlite3* database) const;

 private:
  std::vector<std::byte> bytes_;
};

// Records changes for the duration of one command. Attaches only the content
// tables: project_meta and command_journal are excluded so that undoing an edit
// does not also roll back the revision counter or erase the audit trail.
class Recorder final {
 public:
  explicit Recorder(sqlite3* database);
  ~Recorder();
  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;

  // Stops recording and returns what was captured. Safe to call once.
  [[nodiscard]] ChangeSet Take();

  // The tables a Recorder watches. Exposed so schema changes and this list
  // cannot drift apart unnoticed -- ProjectStore asserts they match.
  [[nodiscard]] static const std::vector<std::string>& RecordedTables();

 private:
  sqlite3* database_{nullptr};
  sqlite3_session* session_{nullptr};
};

}  // namespace cutline::db
