#include "core/db/ChangeSet.h"

#include "core/db/Sql.h"

#include "sqlite3.h"

#include <cstring>
#include <stdexcept>

namespace cutline::db {
namespace {

// Frees a buffer handed back by the session API, which allocates with
// sqlite3_malloc rather than new.
struct SqliteBuffer final {
  void* data{nullptr};
  int size{0};
  ~SqliteBuffer() { sqlite3_free(data); }
  SqliteBuffer() = default;
  SqliteBuffer(const SqliteBuffer&) = delete;
  SqliteBuffer& operator=(const SqliteBuffer&) = delete;
};

std::vector<std::byte> ToBytes(const void* data, int size) {
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  if (size > 0) std::memcpy(bytes.data(), data, static_cast<std::size_t>(size));
  return bytes;
}

// Aborts on any conflict. See the header for why merging would be wrong.
int RejectConflict(void*, int, sqlite3_changeset_iter*) { return SQLITE_CHANGESET_ABORT; }

int RejectFilter(void*, const char*) { return 1; }

}  // namespace

ChangeSet ChangeSet::Inverted() const {
  if (bytes_.empty()) return {};
  SqliteBuffer inverted;
  const auto result = sqlite3changeset_invert(static_cast<int>(bytes_.size()), bytes_.data(), &inverted.size,
                                              &inverted.data);
  if (result != SQLITE_OK) throw SqlError(result, "Unable to invert changeset for undo");
  return ChangeSet(ToBytes(inverted.data, inverted.size));
}

void ChangeSet::ApplyTo(sqlite3* database) const {
  if (bytes_.empty()) return;

  // FKNOACTION makes every foreign key behave as NO ACTION for the duration of
  // the apply. A changeset already records the rows a cascade removed, so
  // letting the cascade run again would delete them a second time and the
  // changeset's own delete would then find nothing and report a conflict.
  //
  // IGNORENOOP covers the same collision from the trigger side: a delete whose
  // row has already gone is not treated as a conflict. It only suppresses
  // changes that would not alter the database anyway, so a genuine mismatch --
  // a row present with different values -- still reaches the handler and aborts.
  constexpr int kFlags = SQLITE_CHANGESETAPPLY_FKNOACTION | SQLITE_CHANGESETAPPLY_IGNORENOOP;
  const auto result = sqlite3changeset_apply_v2(
      database, static_cast<int>(bytes_.size()), const_cast<void*>(static_cast<const void*>(bytes_.data())),
      RejectFilter, RejectConflict, nullptr, nullptr, nullptr, kFlags);
  if (result != SQLITE_OK) {
    throw SqlError(result, "Unable to apply changeset: the project state does not match what this edit recorded");
  }
}

const std::vector<std::string>& Recorder::RecordedTables() {
  // Content tables only. project_meta carries the revision counter and
  // command_journal is the audit trail; neither may be rolled back by an undo.
  static const std::vector<std::string> tables{
      "project_settings", "bins",        "media",             "media_streams", "media_proxies", "sequences",
      "tracks",           "clips",       "transitions",       "effects",       "effect_parameters", "effect_masks",
      "keyframes",        "markers",          "track_sends",        "tracking_data",    "caption_tracks",   "captions",
      "multicam_groups", "multicam_angles", "multicam_switches", "graphic_templates", "graphics",
  };
  return tables;
}

Recorder::Recorder(sqlite3* database) : database_(database) {
  const auto result = sqlite3session_create(database_, "main", &session_);
  if (result != SQLITE_OK) {
    session_ = nullptr;
    throw SqlError(result, "Unable to start recording project changes");
  }
  try {
    for (const auto& table : RecordedTables()) {
      // A table that does not exist yet is attached anyway; the session simply
      // records nothing for it, which keeps this list independent of migration
      // order.
      Check(database_, sqlite3session_attach(session_, table.c_str()), "Unable to watch table " + table);
    }
  } catch (...) {
    sqlite3session_delete(session_);
    session_ = nullptr;
    throw;
  }
}

Recorder::~Recorder() {
  if (session_ != nullptr) sqlite3session_delete(session_);
}

ChangeSet Recorder::Take() {
  if (session_ == nullptr) throw std::logic_error("Recorder has already been taken");
  SqliteBuffer captured;
  const auto result = sqlite3session_changeset(session_, &captured.size, &captured.data);
  sqlite3session_delete(session_);
  session_ = nullptr;
  if (result != SQLITE_OK) throw SqlError(result, "Unable to capture project changes for undo");
  return ChangeSet(ToBytes(captured.data, captured.size));
}

}  // namespace cutline::db
