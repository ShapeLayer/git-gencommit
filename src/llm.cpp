#include "llm.hpp"

#include "git.hpp"
#include "hf.hpp"
#include "json.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <vector>

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

std::string json_escape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 32);
  for (unsigned char c : in) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(static_cast<char>(c));
        break;
    }
  }
  return out;
}

std::string first_non_empty_line(const std::string& s) {
  size_t start = 0;
  while (start < s.size()) {
    size_t end = s.find('\n', start);
    if (end == std::string::npos) {
      end = s.size();
    }
    std::string line = trim(s.substr(start, end - start));
    if (!line.empty()) {
      return line;
    }
    start = end + 1;
  }
  return "";
}

std::string sanitize_commit_title(std::string title) {
  title = trim(title);
  if (title.empty()) {
    return "chore: update files";
  }
  const size_t nl = title.find('\n');
  if (nl != std::string::npos) {
    title = trim(title.substr(0, nl));
  }
  if (title.size() > 72) {
    title = trim(title.substr(0, 72));
  }
  return title;
}

bool contains_ci(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) {
    return true;
  }
  auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                        [](char a, char b) {
                          return std::tolower(static_cast<unsigned char>(a)) ==
                                 std::tolower(static_cast<unsigned char>(b));
                        });
  return it != haystack.end();
}

std::string lowercase(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

std::string basename_no_ext(const std::string& path) {
  std::string name = path;
  const size_t slash = name.find_last_of('/');
  if (slash != std::string::npos) {
    name = name.substr(slash + 1);
  }
  const size_t dot = name.find_last_of('.');
  if (dot != std::string::npos) {
    name = name.substr(0, dot);
  }
  if (name.empty()) {
    return "files";
  }
  return name;
}

bool is_command_available(const std::string& command) {
  if (command.empty()) {
    return false;
  }

  if (command.find('/') != std::string::npos) {
    CommandResult rc = run_command_capture("test -x '" + shell_escape_single(command) + "'");
    return rc.exit_code == 0;
  }

  CommandResult rc =
      run_command_capture("command -v '" + shell_escape_single(command) + "' >/dev/null 2>&1");
  return rc.exit_code == 0;
}

std::vector<std::string> changed_files_from_diff(const std::string& staged_diff) {
  std::vector<std::string> out;
  std::unordered_set<std::string> seen;
  std::istringstream iss(staged_diff);
  std::string line;
  while (std::getline(iss, line)) {
    if (line.rfind("+++ b/", 0) == 0) {
      const std::string path = line.substr(6);
      if (!path.empty() && path != "/dev/null" && seen.insert(path).second) {
        out.push_back(path);
      }
    }
  }
  return out;
}

std::string builtin_commit_title(const std::string& staged_diff) {
  const std::vector<std::string> files = changed_files_from_diff(staged_diff);
  const std::string lowered = lowercase(staged_diff);

  bool docs_only = !files.empty();
  bool has_test = false;
  bool has_ci = false;
  for (const std::string& f : files) {
    const std::string lf = lowercase(f);
    if (!(contains_ci(lf, "readme") || contains_ci(lf, "docs/") || contains_ci(lf, ".md"))) {
      docs_only = false;
    }
    if (contains_ci(lf, "test") || contains_ci(lf, "spec")) {
      has_test = true;
    }
    if (contains_ci(lf, ".github/") || contains_ci(lf, "gitlab-ci") || contains_ci(lf, "ci/")) {
      has_ci = true;
    }
  }

  std::string type = "chore";
  if (docs_only) {
    type = "docs";
  } else if (has_test) {
    type = "test";
  } else if (has_ci) {
    type = "ci";
  } else if (contains_ci(lowered, "fix") || contains_ci(lowered, "bug") ||
             contains_ci(lowered, "error") || contains_ci(lowered, "fail")) {
    type = "fix";
  } else if (contains_ci(staged_diff, "new file mode")) {
    type = "feat";
  } else {
    type = "refactor";
  }

  std::string summary;
  if (files.empty()) {
    summary = "update files";
  } else if (files.size() == 1) {
    summary = "update " + basename_no_ext(files.front());
  } else {
    summary = "update " + std::to_string(files.size()) + " files";
  }
  return sanitize_commit_title(type + ": " + summary);
}

size_t curl_write_to_string(void* contents, size_t size, size_t nmemb,
                            void* userp) {
  const size_t total = size * nmemb;
  std::string* out = static_cast<std::string*>(userp);
  out->append(static_cast<const char*>(contents), total);
  return total;
}

}  // namespace

LlmEngine::LlmEngine(Config cfg, ProviderRegistry providers, AppPaths paths)
    : config_(std::move(cfg)), providers_(std::move(providers)), paths_(std::move(paths)) {}

void LlmEngine::load_if_needed() {
  if (!config_.run_on_startup) {
    return;
  }
}

void LlmEngine::unload_if_needed() {
  if (!config_.unload_after_commit) {
    return;
  }
}

std::string LlmEngine::build_prompt(const std::string& staged_diff) const {
  const std::string head =
      "Generate exactly one Conventional Commit title in English. "
      "Return one line only, no code block, no body, max 72 chars. "
      "Allowed types: feat, fix, chore, docs, refactor, test, perf, ci, build, style, revert.\n"
      "Infer the best type and concise summary from diff.\n\n"
      "DIFF:\n";
  return head + staged_diff;
}

std::string LlmEngine::generate_with_external(const ExternalProvider& provider,
                                              const std::string& prompt) const {
  if (provider.base_url.empty() || provider.model.empty() || provider.api_key.empty()) {
    throw std::runtime_error("external provider missing base_url/model/api_key");
  }

  const std::string json =
      "{\"model\":\"" + json_escape(provider.model) +
      "\",\"messages\":[{\"role\":\"user\",\"content\":\"" +
      json_escape(prompt) + "\"}],\"temperature\":0.2}";

  const std::string endpoint = provider.base_url + "/chat/completions";
  const std::string auth = "Authorization: Bearer " + provider.api_key;
  CURL* curl = curl_easy_init();
  if (!curl) {
    throw std::runtime_error("curl_easy_init failed");
  }

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, auth.c_str());

  for (int attempt = 0; attempt <= provider.max_retries; ++attempt) {
    std::string body;
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(json.size()));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(provider.timeout_sec));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

    const CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    if (rc != CURLE_OK) {
      if (attempt == provider.max_retries) {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        throw std::runtime_error("external request failed");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(250 * (1 << attempt)));
      continue;
    }

    if ((code == 429 || code >= 500) && attempt < provider.max_retries) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250 * (1 << attempt)));
      continue;
    }
    if (code < 200 || code >= 300) {
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      throw std::runtime_error("external provider HTTP error: " + std::to_string(code));
    }

    std::string content = json_extract_first_content_from_chat_completions(body);
    if (content.empty()) {
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      throw std::runtime_error("failed to parse content from provider response");
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return sanitize_commit_title(first_non_empty_line(content));
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  throw std::runtime_error("external generation failed after retries");
}

std::string LlmEngine::generate_with_local(const LocalProvider& provider,
                                           const std::string& prompt) const {
  const std::string model_path =
      hf_download_if_needed(provider, paths_, config_.auto_download_models);
  if (model_path.empty()) {
    throw std::runtime_error("local provider missing usable model_path");
  }

  std::string cli = provider.llama_cli_path.empty() ? "llama-cli" : provider.llama_cli_path;
  if (!is_command_available(cli)) {
    throw std::runtime_error("local runtime not available: llama-cli command not found");
  }
  std::string cmd =
      "'" + shell_escape_single(cli) + "'"
      " -m '" + shell_escape_single(model_path) + "'"
      " -n 96 --temp 0.2 --top-p 0.9"
      " -p '" + shell_escape_single(prompt) + "'";

  const CommandResult rc = run_command_capture(cmd);
  if (rc.exit_code != 0) {
    throw std::runtime_error("local llama.cpp inference failed");
  }

  const std::string line = first_non_empty_line(rc.output);
  return sanitize_commit_title(line);
}

std::string LlmEngine::generate_commit_message(const std::string& staged_diff) const {
  if (staged_diff.empty()) {
    throw std::runtime_error("no staged diff to generate commit message from");
  }

  const std::string prompt = build_prompt(staged_diff);

  auto use_external = [&](const std::string& key) -> std::string {
    const auto it = providers_.external.find(key);
    if (it == providers_.external.end()) {
      throw std::runtime_error("external provider not found: " + key);
    }
    return generate_with_external(it->second, prompt);
  };

  auto use_local = [&](const std::string& key) -> std::string {
    const auto it = providers_.local_models.find(key);
    if (it == providers_.local_models.end()) {
      throw std::runtime_error("local provider not found: " + key);
    }
    return generate_with_local(it->second, prompt);
  };

  try {
    if (!config_.default_provider.empty()) {
      if (providers_.external.count(config_.default_provider) != 0) {
        return use_external(config_.default_provider);
      }
      if (providers_.local_models.count(config_.default_provider) != 0) {
        return use_local(config_.default_provider);
      }
      throw std::runtime_error("default_provider not found in providers.toml");
    }

    if (!providers_.external.empty()) {
      return generate_with_external(providers_.external.begin()->second, prompt);
    }
    if (!providers_.local_models.empty()) {
      return generate_with_local(providers_.local_models.begin()->second, prompt);
    }

    throw std::runtime_error("no provider configured in providers.toml");
  } catch (...) {
    return builtin_commit_title(staged_diff);
  }
}

}  // namespace ggc
