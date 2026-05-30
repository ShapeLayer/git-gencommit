#include "git.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

namespace ggc {
namespace fs = std::filesystem;
namespace {

std::string git_command_cached(const std::string& args,
                               const std::string& index_path = std::string()) {
  if (index_path.empty()) {
    return "git diff --cached " + args;
  }
  return "GIT_INDEX_FILE='" + shell_escape_single(index_path) + "' git diff --cached " + args;
}

std::string git_add_all_command(const std::string& index_path = std::string()) {
  if (index_path.empty()) {
    return "git add .";
  }
  return "GIT_INDEX_FILE='" + shell_escape_single(index_path) + "' git add .";
}

std::string run_or_throw(const std::string& command, const std::string& error_message) {
  const CommandResult rc = run_command_capture(command);
  if (rc.exit_code != 0) {
    throw std::runtime_error(error_message);
  }
  return rc.output;
}

std::string build_name_status_from_numstat(const std::string& numstat) {
  std::istringstream iss(numstat);
  std::string line;
  std::string out;
  while (std::getline(iss, line)) {
    const size_t first_tab = line.find('\t');
    if (first_tab == std::string::npos) {
      continue;
    }
    const size_t second_tab = line.find('\t', first_tab + 1);
    if (second_tab == std::string::npos || second_tab + 1 >= line.size()) {
      continue;
    }
    const std::string path = line.substr(second_tab + 1);
    out += "M\t" + path + "\n";
  }
  return out;
}

StagedChangeContext collect_staged_context(const std::string& index_path = std::string()) {
  StagedChangeContext out;
  out.numstat = run_or_throw(git_command_cached("--numstat --find-renames", index_path),
                             "git diff --cached --numstat failed");
  out.name_status = build_name_status_from_numstat(out.numstat);
  out.patch = run_or_throw(git_command_cached("--find-renames", index_path),
                           "git diff --cached failed");
  return out;
}

}  // namespace

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
  return git_staged_context().patch;
}

std::string git_virtual_staged_diff_all() {
  return git_virtual_staged_context_all().patch;
}

StagedChangeContext git_staged_context() {
  return collect_staged_context();
}

StagedChangeContext git_virtual_staged_context_all() {
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

    const std::string add_cmd = git_add_all_command(index_path);
    const CommandResult add_rc = run_command_capture(add_cmd);
    if (add_rc.exit_code != 0) {
      throw std::runtime_error("failed to stage in virtual index for dry-run");
    }

    const StagedChangeContext context = collect_staged_context(index_path);

    fs::remove(index_path);
    return context;
  } catch (...) {
    fs::remove(index_path);
    throw;
  }
}

void git_add_all() {
  const int rc = run_command_passthrough(git_add_all_command());
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
