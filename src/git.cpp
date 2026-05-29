#include "git.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

namespace ggc {
namespace fs = std::filesystem;

CommandResult run_command_capture(const std::string& command) {
  CommandResult out;
  std::array<char, 4096> buf{};
  FILE* pipe = popen(command.c_str(), "r");
  if (!pipe) {
    throw std::runtime_error("failed to execute command: " + command);
  }
  while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
    out.output += buf.data();
  }
  const int status = pclose(pipe);
  if (WIFEXITED(status)) {
    out.exit_code = WEXITSTATUS(status);
  } else {
    out.exit_code = -1;
  }
  return out;
}

int run_command_passthrough(const std::string& command) {
  const int rc = std::system(command.c_str());
  if (WIFEXITED(rc)) {
    return WEXITSTATUS(rc);
  }
  return -1;
}

std::string git_staged_diff() {
  CommandResult rc = run_command_capture("git diff --cached");
  if (rc.exit_code != 0) {
    throw std::runtime_error("git diff --cached failed");
  }
  return rc.output;
}

std::string git_virtual_staged_diff_all() {
  char tmpl[] = "/tmp/git-gencommit-index-XXXXXX";
  const int fd = mkstemp(tmpl);
  if (fd < 0) {
    throw std::runtime_error("failed to create temporary index file");
  }
  close(fd);
  const std::string index_path = tmpl;

  try {
    const CommandResult copy_rc = run_command_capture(
        "cp .git/index '" + shell_escape_single(index_path) + "'");
    if (copy_rc.exit_code != 0) {
      throw std::runtime_error("failed to clone git index for dry-run");
    }

    const std::string add_cmd =
        "GIT_INDEX_FILE='" + shell_escape_single(index_path) + "' git add .";
    const CommandResult add_rc = run_command_capture(add_cmd);
    if (add_rc.exit_code != 0) {
      throw std::runtime_error("failed to stage in virtual index for dry-run");
    }

    const std::string diff_cmd =
        "GIT_INDEX_FILE='" + shell_escape_single(index_path) +
        "' git diff --cached";
    const CommandResult diff_rc = run_command_capture(diff_cmd);
    if (diff_rc.exit_code != 0) {
      throw std::runtime_error("failed to get virtual staged diff");
    }

    fs::remove(index_path);
    return diff_rc.output;
  } catch (...) {
    fs::remove(index_path);
    throw;
  }
}

void git_add_all() {
  const int rc = run_command_passthrough("git add .");
  if (rc != 0) {
    throw std::runtime_error("git add . failed");
  }
}

std::string shell_escape_single(const std::string& input) {
  std::string out;
  out.reserve(input.size() + 16);
  for (char c : input) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

void git_commit(const std::string& message) {
  std::string cmd = "git commit -m '" + shell_escape_single(message) + "'";
  const int rc = run_command_passthrough(cmd);
  if (rc != 0) {
    throw std::runtime_error("git commit failed");
  }
}

void git_push() {
  const int rc = run_command_passthrough("git push");
  if (rc != 0) {
    throw std::runtime_error("git push failed");
  }
}

}  // namespace ggc
