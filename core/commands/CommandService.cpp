#include "core/commands/CommandService.h"

#include "core/project/ProjectStore.h"

namespace cutline::commands {

project::CommandResult CommandService::Submit(const CommandEnvelope& command) {
  Validate(command);
  return project_store_.Execute(command);
}

project::CommandResult CommandService::Undo(const std::string& author_id, const std::string& timestamp_utc) { return project_store_.Undo(author_id, timestamp_utc); }
project::CommandResult CommandService::Redo(const std::string& author_id, const std::string& timestamp_utc) { return project_store_.Redo(author_id, timestamp_utc); }

}  // namespace cutline::commands
