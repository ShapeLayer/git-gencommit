#pragma once

#include <string>

namespace ggc {

struct Options {
  bool has_any_option = false;
  bool auto_add = false;
  bool commit = false;
  bool push = false;
  bool dry_run = false;
  bool print_message = false;
  bool help = false;
};

Options parse_cli(int argc, char** argv);
void print_help(const std::string& bin_name);

}  // namespace ggc
