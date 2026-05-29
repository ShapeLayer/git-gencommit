#include "cli.hpp"

#include <iostream>
#include <stdexcept>

namespace ggc {

Options parse_cli(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      opt.has_any_option = true;
      opt.help = true;
      continue;
    }
    if (arg == "--dry-run") {
      opt.has_any_option = true;
      opt.dry_run = true;
      continue;
    }
    if (arg == "--print-message") {
      opt.has_any_option = true;
      opt.print_message = true;
      continue;
    }
    if (arg == "-a" || arg == "--auto-add") {
      opt.has_any_option = true;
      opt.auto_add = true;
      continue;
    }
    if (arg == "-c" || arg == "--commit") {
      opt.has_any_option = true;
      opt.commit = true;
      continue;
    }
    if (arg == "-p" || arg == "--push") {
      opt.has_any_option = true;
      opt.push = true;
      continue;
    }
    throw std::runtime_error("unknown option: " + arg);
  }

  if (!opt.has_any_option && !opt.help) {
    opt.auto_add = true;
    opt.commit = true;
    opt.push = true;
  }

  return opt;
}

void print_help(const std::string& bin_name) {
  std::cout
      << "Usage: " << bin_name << " [options]\n"
      << "\n"
      << "Options:\n"
      << "  -a, --auto-add   Run git add .\n"
      << "  -c, --commit     Generate commit message with LLM and run git commit\n"
      << "  -p, --push       Run git push\n"
      << "      --dry-run    Print planned actions without running git add/commit/push\n"
      << "      --print-message  Print generated commit title when --commit is used\n"
      << "  -h, --help       Show this help\n"
      << "\n"
      << "Default (no flags): behaves as -a -c -p\n"
      << "If any flag is provided, defaults are disabled and only given flags are run.\n";
}

}  // namespace ggc
