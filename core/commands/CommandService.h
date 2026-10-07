#pragma once

#include "core/commands/Command.h"
#include "core/project/ProjectStore.h"

#include <cstdint>

namespace cutline::commands {

class CommandService final {
 public:
  explicit CommandService(project::ProjectStore& project_store) : project_store_(project_store) {}

  [[nodiscard]] project::CommandResult Submit(const CommandEnvelope& command);
  [[nodiscard]] project::CommandResult Undo(const std::string& author_id, const std::string& timestamp_utc);
  [[nodiscard]] project::CommandResult Redo(const std::string& author_id, const std::string& timestamp_utc);

 private:
  project::ProjectStore& project_store_;
};

}  // namespace cutline::commands
