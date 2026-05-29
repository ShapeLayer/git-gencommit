#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace ggc {

struct TomlTable {
  std::unordered_map<std::string, std::string> values;
};

struct TomlDoc {
  std::unordered_map<std::string, TomlTable> tables;
};

TomlDoc parse_toml_lite(const std::string& text);
std::string toml_get(const TomlDoc& doc, const std::string& table,
                     const std::string& key,
                     const std::string& default_value = "");
std::vector<std::string> toml_tables_with_prefix(const TomlDoc& doc,
                                                 const std::string& prefix);

}  // namespace ggc
