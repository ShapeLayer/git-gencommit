#pragma once

#include <string>
#include <vector>

namespace ggc {

struct CommandResult {
  int exit_code = -1;
  std::string output;
};

CommandResult run_command_capture(const std::string& command);
int run_command_passthrough(const std::string& command);

std::string git_staged_diff();
std::string git_virtual_staged_diff_all();
void git_add_all();
void git_commit(const std::string& message);
void git_push();

std::string shell_escape_single(const std::string& input);

}  // namespace ggc
