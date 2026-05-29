#include "toml_lite.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace ggc {
namespace {

std::string trim(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) {
    ++b;
  }
  size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
    --e;
  }
  return s.substr(b, e - b);
}

std::string strip_quotes(const std::string& s) {
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\''))) {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

}  // namespace

TomlDoc parse_toml_lite(const std::string& text) {
  TomlDoc doc;
  std::string current = "";
  std::istringstream iss(text);
  std::string line;
  while (std::getline(iss, line)) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '#') {
      continue;
    }
    if (t.front() == '[' && t.back() == ']') {
      current = trim(t.substr(1, t.size() - 2));
      doc.tables.try_emplace(current, TomlTable{});
      continue;
    }
    const auto pos = t.find('=');
    if (pos == std::string::npos) {
      continue;
    }
    std::string key = trim(t.substr(0, pos));
    std::string value = trim(t.substr(pos + 1));
    const auto hash_pos = value.find('#');
    if (hash_pos != std::string::npos) {
      value = trim(value.substr(0, hash_pos));
    }
    value = strip_quotes(value);
    doc.tables[current].values[key] = value;
  }
  return doc;
}

std::string toml_get(const TomlDoc& doc, const std::string& table,
                     const std::string& key,
                     const std::string& default_value) {
  const auto t = doc.tables.find(table);
  if (t == doc.tables.end()) {
    return default_value;
  }
  const auto v = t->second.values.find(key);
  if (v == t->second.values.end()) {
    return default_value;
  }
  return v->second;
}

std::vector<std::string> toml_tables_with_prefix(const TomlDoc& doc,
                                                 const std::string& prefix) {
  std::vector<std::string> out;
  for (const auto& kv : doc.tables) {
    if (kv.first.rfind(prefix, 0) == 0) {
      out.push_back(kv.first);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace ggc
