#pragma once

// The project store: the only thing in Cutline that may change a project.
//
// Every mutation arrives as a CommandEnvelope, is validated against both its own
// invariants and current project state, applied inside one transaction, and
// recorded in the journal together with the row-level changeset needed to undo
// it. Nothing else writes to the database.

#include "core/commands/Command.h"
#include "core/db/ChangeSet.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace cutline::project {

// What a commit promises about power loss. A crash of the application is survived
// either way: SQLite has the commit in its write-ahead log before Execute returns.
enum class Durability {
  // synchronous=NORMAL. The log is not forced to disk at every commit, so a power
  // failure or operating-system crash can lose the last few commits (never corrupt
  // the project). Fastest, and what an editor that saves after every gesture wants.
  Normal,
  // synchronous=FULL. Every commit is forced to disk before Execute returns: nothing
  // acknowledged is lost, at the price of a disk flush per edit.
  Full,
};

struct SnapshotPolicy final {
  // A full database snapshot every N revisions, as a recovery floor independent
  // of the journal. Undo does not use these.
  std::int64_t every_revisions{250};
  // How many automatic snapshots to keep; older ones are deleted when a new one is
  // written, together with the journal files that predate the oldest kept. Without
  // a limit a long session fills the disk with copies of the whole project.
  std::size_t keep_snapshots{8};
  Durability durability{Durability::Normal};
  // Undo depth in steps (an edit made of many commands is one step). Premiere's default is 32;
  // changesets are small enough that a deeper stack is affordable.
  std::size_t history_limit{256};
};

struct CommandResult final {
  std::int64_t revision{};
  std::string summary;
  bool idempotent{};
  // Things that went wrong *after* the command was committed: the copy of its journal
  // record in the package, or an automatic snapshot, could not be written (disk full,
  // permissions). The command succeeded and is in the database; these are reported
  // rather than thrown because throwing would tell the caller an edit failed that
  // had in fact been applied.
  std::vector<std::string> warnings;
};

// One entry on the undo stack.
struct HistoryEntry final {
  std::string command_id;
  std::string label;
  db::ChangeSet changeset;
  // Entries made by one ExecuteGroup share a group and are undone and redone together; empty for a
  // command executed alone.
  std::string group;
};

// What is wrong with a package, and what can be done about it. See
// ProjectStore::InspectPackage.
struct RecoveryReport final {
  // The package's database opens, migrates and validates.
  bool database_healthy{false};
  std::string database_problem;
  std::int64_t database_revision{-1};
  // Revisions of the snapshots that open and validate, ascending.
  std::vector<std::int64_t> snapshots;
  // The highest revision up to which readable journal records form an unbroken
  // chain from the best snapshot (or from the database, when it is healthy).
  std::int64_t reachable_revision{-1};
  // Set by RecoverPackage.
  std::int64_t snapshot_used{-1};
  std::int64_t recovered_revision{-1};
  int records_replayed{0};
  std::filesystem::path quarantine;
  // Plain statements of what was found and done, for a recovery dialog or a log.
  std::vector<std::string> notes;
};

struct RecoveryOptions final {
  // When the database is healthy but journal records continue past its revision (a
  // database restored from an older copy, say), replay them onto it.
  bool roll_forward{false};
  // Work out what would be recovered and change nothing.
  bool dry_run{false};
};

class ProjectStore final {
 public:
  explicit ProjectStore(std::string database_path, SnapshotPolicy policy = {});
  ~ProjectStore();
  ProjectStore(const ProjectStore&) = delete;
  ProjectStore& operator=(const ProjectStore&) = delete;

  [[nodiscard]] static std::unique_ptr<ProjectStore> CreatePackage(const std::filesystem::path& package_path,
                                                                  const commands::CommandEnvelope& create_project,
                                                                  SnapshotPolicy policy = {});
  [[nodiscard]] static std::unique_ptr<ProjectStore> OpenPackage(const std::filesystem::path& package_path,
                                                                 SnapshotPolicy policy = {});
  [[nodiscard]] static std::string GenerateProjectUuid();

  // Crash recovery.
  //
  // The database is the record, and SQLite makes each commit atomic, so a crash
  // leaves it at a consistent revision. What recovery is for is the other failures:
  // a database that will not open or no longer validates, or one restored from an
  // older copy. The package keeps, besides the database, snapshots (whole-database
  // copies) and a journal file per revision holding the row-level changeset that
  // revision applied; from a valid snapshot and an unbroken run of those records
  // the database can be rebuilt exactly.
  //
  // InspectPackage only looks (and does what opening a project always does,
  // including migrating an old one). RecoverPackage acts: a healthy package is
  // simply opened; otherwise it rebuilds from the snapshot that reaches furthest,
  // moves everything it replaced into a quarantine folder inside the package, and
  // opens the result. Replay is idempotent: a record already applied is checked
  // against the journal and skipped, never applied twice. The undo history is in
  // memory and does not survive; a recovered project starts with none.
  [[nodiscard]] static RecoveryReport InspectPackage(const std::filesystem::path& package_path);
  [[nodiscard]] static std::unique_ptr<ProjectStore> RecoverPackage(const std::filesystem::path& package_path,
                                                                    RecoveryOptions options = {},
                                                                    RecoveryReport* report = nullptr);

  void Initialize();

  [[nodiscard]] CommandResult Execute(const commands::CommandEnvelope& command);
  // Executes several commands as one edit: they are undone and redone together, under one label in
  // HistorySteps. Each command's base revision is taken to be the project's revision at the moment it
  // runs, except the first, which is checked against the caller's as for any command. If one fails,
  // the ones before it are taken back, left out of the history, and the failure is thrown, so a group
  // either happens whole or not at all. The result is the last command's.
  [[nodiscard]] CommandResult ExecuteGroup(std::vector<commands::CommandEnvelope> commands, const std::string& label);
  [[nodiscard]] CommandResult Undo(const std::string& author_id, const std::string& timestamp_utc);
  [[nodiscard]] CommandResult Redo(const std::string& author_id, const std::string& timestamp_utc);

  // Labels for a history panel, oldest first. The applied prefix is
  // [0, applied_history_count).
  [[nodiscard]] std::vector<std::string> HistoryLabels() const;
  [[nodiscard]] std::size_t AppliedHistoryCount() const;
  // The same history as undo steps: a group is one step, labelled as the group was.
  [[nodiscard]] std::vector<std::string> HistorySteps() const;
  [[nodiscard]] std::size_t AppliedStepCount() const;
  // Forgets the undo stack. A new project's own setup (its sequence and tracks) is not an edit anyone should be able to undo.
  void ClearHistory();
  [[nodiscard]] bool CanUndo() const;
  [[nodiscard]] bool CanRedo() const;

  void CreateSnapshot();
  void ValidateDatabase() const;

  [[nodiscard]] std::int64_t CurrentRevision() const;
  [[nodiscard]] std::string JournalMode() const;
  [[nodiscard]] std::filesystem::path PackagePath() const { return package_path_; }
  [[nodiscard]] std::int64_t SnapshotCount() const;
  // Every post-commit failure since the project was opened (newest last, capped).
  [[nodiscard]] std::vector<std::string> PersistenceWarnings() const;
  [[nodiscard]] std::int64_t JournalCount() const;
  [[nodiscard]] std::string ProjectName() const;
  [[nodiscard]] std::string ProjectId() const;

  // Total bytes held by the undo stack. Exposed because the whole point of
  // changeset undo is that this stays small, and a test asserts it.
  [[nodiscard]] std::size_t HistoryBytes() const;

  // Raw handle for read models (the timeline compiler loads through this). The
  // store owns it; readers must not mutate.
  [[nodiscard]] sqlite3* connection() const noexcept { return database_; }
  [[nodiscard]] std::mutex& mutex() const noexcept { return mutex_; }

 private:
  void Open();
  void ConfigureConnection() const;
  void MigrateIfNeeded();
  void ApplyMutation(const commands::CommandEnvelope& command);
  void BackupDatabase(const std::filesystem::path& destination) const;
  void WriteJournalFile(std::int64_t revision, const std::string& command_id, const std::string& record) const;

  // What has to happen after a command commits and the store's lock is released.
  struct PostCommit final {
    std::int64_t revision{0};
    std::string command_id;
    std::string journal_record;
    bool snapshot_due{false};
  };
  [[nodiscard]] CommandResult ExecuteLocked(const commands::CommandEnvelope& command, PostCommit& post);
  [[nodiscard]] CommandResult UndoLocked(const std::string& author_id, const std::string& timestamp_utc,
                                         PostCommit& post);
  [[nodiscard]] CommandResult RedoLocked(const std::string& author_id, const std::string& timestamp_utc,
                                         PostCommit& post);
  // Applies one journal record (parsed) at the next revision. Returns false when the
  // record was already applied, which is checked rather than assumed.
  bool ReplayRecordLocked(const std::string& record_text);
  void FinishPersistence(const PostCommit& post, CommandResult& result);
  // Copies the database through a connection of its own, so the store is not held
  // while a large project is copied. Returns the revision the copy is of.
  std::int64_t WriteSnapshotCopy(const std::filesystem::path& directory) const;
  void PruneSnapshots() const;
  void RecordWarning(const std::string& warning);
  void SetRevision(std::int64_t revision, const std::string& timestamp_utc) const;
  [[nodiscard]] std::int64_t RevisionLocked() const;
  [[nodiscard]] std::string ProjectIdLocked() const;
  void TrimHistory();

  sqlite3* database_{nullptr};
  std::filesystem::path database_path_;
  std::filesystem::path package_path_;
  SnapshotPolicy snapshot_policy_;

  // Serialises all access. The store is shared by the UI thread, background
  // workers, and the MCP bridge, and a bare sqlite3* is not safe across them.
  mutable std::mutex mutex_;

  // The undo stack lives in memory, like every NLE's: history does not survive
  // reopening a project. The durable record is command_journal, which keeps each
  // changeset for audit and for future crash replay.
  std::vector<HistoryEntry> history_;
  std::size_t applied_history_{0};
  // The group commands executed now belong to (empty outside ExecuteGroup), and the next group's number.
  std::string current_group_;
  std::string current_group_label_;
  std::uint64_t group_counter_{0};

  mutable std::mutex warning_mutex_;
  std::vector<std::string> persistence_warnings_;
};

}  // namespace cutline::project
