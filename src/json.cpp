#include "json.hpp"

#include <cctype>

namespace ggc {
namespace {

size_t skip_ws(const std::string& s, size_t i) {
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
    ++i;
  }
  return i;
}

bool parse_string(const std::string& s, size_t& i, std::string& out) {
  if (i >= s.size() || s[i] != '"') {
    return false;
  }
  ++i;
  out.clear();
  while (i < s.size()) {
    char c = s[i++];
    if (c == '"') {
      return true;
    }
    if (c == '\\' && i < s.size()) {
      char e = s[i++];
      switch (e) {
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case '"':
          out.push_back('"');
          break;
        case '\\':
          out.push_back('\\');
          break;
        default:
          out.push_back(e);
          break;
      }
      continue;
    }
    out.push_back(c);
  }
  return false;
}

size_t skip_value(const std::string& s, size_t i) {
  i = skip_ws(s, i);
  if (i >= s.size()) {
    return i;
  }
  if (s[i] == '"') {
    std::string tmp;
    if (!parse_string(s, i, tmp)) {
      return s.size();
    }
    return i;
  }
  if (s[i] == '{' || s[i] == '[') {
    const char open = s[i];
    const char close = (open == '{') ? '}' : ']';
    int depth = 0;
    while (i < s.size()) {
      if (s[i] == '"') {
        std::string tmp;
        if (!parse_string(s, i, tmp)) {
          return s.size();
        }
        continue;
      }
      if (s[i] == open) {
        ++depth;
      } else if (s[i] == close) {
        --depth;
        if (depth == 0) {
          return i + 1;
        }
      }
      ++i;
    }
    return s.size();
  }
  while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') {
    ++i;
  }
  return i;
}

}  // namespace

std::string json_extract_first_content_from_chat_completions(const std::string& json) {
  size_t i = 0;
  while (i < json.size()) {
    i = skip_ws(json, i);
    if (i >= json.size()) {
      break;
    }
    if (json[i] == '"') {
      std::string key;
      if (!parse_string(json, i, key)) {
        break;
      }
      i = skip_ws(json, i);
      if (i >= json.size() || json[i] != ':') {
        continue;
      }
      ++i;
      i = skip_ws(json, i);
      if (key == "content" && i < json.size() && json[i] == '"') {
        std::string value;
        if (parse_string(json, i, value)) {
          return value;
        }
        break;
      }
      i = skip_value(json, i);
      continue;
    }
    ++i;
  }
  return "";
}

}  // namespace ggc
