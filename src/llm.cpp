#include "llm.hpp"

#include "git.hpp"
#include "hf.hpp"
#include "json.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <vector>

namespace ggc {
namespace {
namespace fs = std::filesystem;

constexpr const char* kBootstrapLlamaEnv = "GIT_GENCOMMIT_BOOTSTRAP_LLAMACPP";
constexpr const char* kPinnedLlamaCommit = "1738129bee5c81b06fa1850daf3f958813c76f5f";

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

void append_prompt_section(std::string& out, const std::string& title,
                           const std::string& body) {
  out += title;
  out += ":\n";
  if (body.empty()) {
    out += "(empty)\n\n";
    return;
  }
  out += body;
  if (body.back() != '\n') {
    out += '\n';
  }
  out += '\n';
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

std::string llama_cli_help_text(const std::string& cli) {
  const CommandResult rc = run_command_capture("'" + shell_escape_single(cli) + "' --help 2>&1");
  return rc.output;
}

bool help_supports_option(const std::string& help, const std::string& opt) {
  return !help.empty() && contains_ci(help, opt);
}

std::string getenv_or_empty(const char* key) {
  const char* value = std::getenv(key);
  if (!value) {
    return "";
  }
  return value;
}

bool is_non_negative_integer(const std::string& s) {
  if (s.empty()) {
    return false;
  }
  for (unsigned char c : s) {
    if (std::isdigit(c) == 0) {
      return false;
    }
  }
  return true;
}

bool env_truthy(const std::string& value) {
  const std::string v = lowercase(trim(value));
  return v == "1" || v == "true" || v == "yes" || v == "on";
}

bool env_falsy(const std::string& value) {
  const std::string v = lowercase(trim(value));
  return v == "0" || v == "false" || v == "no" || v == "off";
}

bool cli_supports_gpu_layers_flag(const std::string& help) {
  return help_supports_option(help, "-ngl") || help_supports_option(help, "--n-gpu-layers");
}

std::string resolve_non_interactive_args(const std::string& help) {
  std::string args;

  if (help_supports_option(help, "-st") || help_supports_option(help, "--single-turn")) {
    args += " -st";
  }
  if (help_supports_option(help, "-no-cnv")) {
    args += " -no-cnv";
  } else if (help_supports_option(help, "--no-conversation")) {
    args += " --no-conversation";
  }
  if (help_supports_option(help, "--no-display-prompt")) {
    args += " --no-display-prompt";
  }
  if (help_supports_option(help, "--no-warmup")) {
    args += " --no-warmup";
  }
  return args;
}

std::string extract_commit_title_from_local_output(const std::string& output,
                                                   const std::string& prompt) {
  const std::regex conventional_title(
      R"((feat|fix|chore|docs|refactor|test|perf|ci|build|style|revert)(\([^)]+\))?!?:\s+\S.*)",
      std::regex_constants::icase);

  std::unordered_set<std::string> prompt_lines;
  {
    std::istringstream pss(prompt);
    std::string pline;
    while (std::getline(pss, pline)) {
      pline = trim(pline);
      if (!pline.empty()) {
        prompt_lines.insert(pline);
      }
    }
  }

  auto cleanup = [](std::string line) {
    const std::vector<std::string> noise_tokens = {
        "[end of text]", "<|end_of_text|>", "</s>", "<s>", "<think>", "</think>"};
    for (const std::string& token : noise_tokens) {
      const size_t pos = line.find(token);
      if (pos != std::string::npos) {
        line = line.substr(0, pos);
      }
    }
    return trim(line);
  };

  std::istringstream iss(output);
  std::string line;
  std::string candidate;
  while (std::getline(iss, line)) {
    line = trim(line);
    if (line.empty()) {
      continue;
    }
    if (line.rfind(">", 0) == 0) {
      line = trim(line.substr(1));
    }
    if (line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0) {
      line = trim(line.substr(2));
    }
    if (line.size() >= 2 && line.front() == '`' && line.back() == '`') {
      line = trim(line.substr(1, line.size() - 2));
    }
    line = cleanup(line);
    if (line.empty()) {
      continue;
    }
    if (line == "`" || line == "'" || line == "\"" || line == "```") {
      continue;
    }
    if (line.size() <= 2 &&
        std::all_of(line.begin(), line.end(), [](unsigned char ch) {
          return !std::isalnum(ch);
        })) {
      continue;
    }
    if (prompt_lines.count(line) != 0) {
      continue;
    }

    std::smatch m;
    if (std::regex_search(line, m, conventional_title)) {
      candidate = trim(m.str(0));
    }
  }
  if (!candidate.empty()) {
    return candidate;
  }
  return "";
}

std::string detect_gpu_backend(const std::string& cli) {
  const CommandResult rc =
      run_command_capture("'" + shell_escape_single(cli) + "' --list-devices 2>&1");
  if (rc.exit_code != 0 || rc.output.empty()) {
    return "";
  }

  const std::string out = lowercase(rc.output);
  if (out.find("metal") != std::string::npos) {
    return "metal";
  }
  if (out.find("cuda") != std::string::npos) {
    return "cuda";
  }
  if (out.find("rocm") != std::string::npos || out.find("hip") != std::string::npos) {
    return "hip";
  }
  if (out.find("vulkan") != std::string::npos) {
    return "vulkan";
  }
  if (out.find("opencl") != std::string::npos) {
    return "opencl";
  }
  if (out.find("gpu") != std::string::npos) {
    return "gpu";
  }
  return "";
}

std::string resolve_gpu_offload_args(const std::string& cli, const std::string& help) {
  if (!cli_supports_gpu_layers_flag(help)) {
    return "";
  }

  const std::string gpu_env = getenv_or_empty("GIT_GENCOMMIT_GPU");
  if (env_falsy(gpu_env)) {
    return "";
  }

  const std::string backend = detect_gpu_backend(cli);
  const bool force_gpu = env_truthy(gpu_env);
  if (backend.empty() && !force_gpu) {
    return "";
  }

  std::string ngl = trim(getenv_or_empty("GIT_GENCOMMIT_N_GPU_LAYERS"));
  if (!is_non_negative_integer(ngl)) {
    ngl = "999";
  }

  if (!backend.empty()) {
    std::cout << "[git-gencommit] local runtime acceleration: " << backend << " (-ngl " << ngl
              << ")\n";
  } else {
    std::cout << "[git-gencommit] local runtime acceleration forced (-ngl " << ngl << ")\n";
  }
  return " -ngl " + ngl;
}

std::string run_checked_capture(const std::string& command,
                                const std::string& error_message) {
  const CommandResult rc = run_command_capture(command + " 2>&1");
  if (rc.exit_code != 0) {
    throw std::runtime_error(error_message + ": " + trim(rc.output));
  }
  return rc.output;
}

std::string verified_help_output(const std::string& cli) {
  if (!is_command_available(cli)) {
    throw std::runtime_error("local runtime executable not found: " + cli);
  }
  const CommandResult rc = run_command_capture("'" + shell_escape_single(cli) + "' --help 2>&1");
  if (rc.exit_code != 0 || trim(rc.output).empty()) {
    throw std::runtime_error("local runtime executable is not healthy: " + cli);
  }
  return rc.output;
}

std::string runtime_llama_cli_path(const AppPaths& paths) {
  return paths.runtime_dir + "/llama.cpp/build/bin/llama-cli";
}

std::string runtime_llama_completion_path(const AppPaths& paths) {
  return paths.runtime_dir + "/llama.cpp/build/bin/llama-completion";
}

std::string path_basename(const std::string& path) {
  const size_t pos = path.find_last_of('/');
  if (pos == std::string::npos) {
    return path;
  }
  return path.substr(pos + 1);
}

std::string path_dirname(const std::string& path) {
  const size_t pos = path.find_last_of('/');
  if (pos == std::string::npos) {
    return "";
  }
  return path.substr(0, pos);
}

std::string completion_command_for(const std::string& command) {
  const std::string base = path_basename(command);
  if (base != "llama-cli") {
    return "";
  }

  if (command.find('/') == std::string::npos) {
    if (is_command_available("llama-completion")) {
      return "llama-completion";
    }
    return "";
  }

  const std::string dir = path_dirname(command);
  if (dir.empty()) {
    return "";
  }
  const std::string sibling = dir + "/llama-completion";
  if (is_command_available(sibling)) {
    return sibling;
  }
  return "";
}

void bootstrap_llama_runtime_if_needed(const AppPaths& paths) {
  const std::string runtime_cli = runtime_llama_cli_path(paths);
  const std::string runtime_completion = runtime_llama_completion_path(paths);
  if (fs::exists(runtime_completion)) {
    verified_help_output(runtime_completion);
    return;
  }

  const std::string bootstrap_env = getenv_or_empty(kBootstrapLlamaEnv);
  if (!bootstrap_env.empty() && !env_truthy(bootstrap_env)) {
    throw std::runtime_error(std::string("local runtime not available and bootstrap disabled by ") +
                             kBootstrapLlamaEnv);
  }

  fs::create_directories(paths.runtime_dir);
  const std::string src_dir = paths.runtime_dir + "/llama.cpp/src";
  const std::string build_dir = paths.runtime_dir + "/llama.cpp/build";

  if (!fs::exists(src_dir + "/.git")) {
    std::cout << "[git-gencommit] local runtime missing, bootstrapping llama.cpp\n";
    run_checked_capture("mkdir -p '" + shell_escape_single(paths.runtime_dir + "/llama.cpp") +
                            "'",
                        "failed to prepare runtime directory");
    run_checked_capture("git clone --depth 1 https://github.com/ggml-org/llama.cpp.git '" +
                            shell_escape_single(src_dir) + "'",
                        "failed to clone llama.cpp runtime");
  }

  std::string current_commit = trim(run_checked_capture(
      "git -C '" + shell_escape_single(src_dir) + "' rev-parse HEAD",
      "failed to read llama.cpp runtime commit"));
  if (current_commit != kPinnedLlamaCommit) {
    run_checked_capture("git -C '" + shell_escape_single(src_dir) + "' fetch --depth 1 origin " +
                            kPinnedLlamaCommit,
                        "failed to fetch pinned llama.cpp commit");
    run_checked_capture("git -C '" + shell_escape_single(src_dir) + "' checkout --detach " +
                            kPinnedLlamaCommit,
                        "failed to checkout pinned llama.cpp commit");
    current_commit = trim(run_checked_capture(
        "git -C '" + shell_escape_single(src_dir) + "' rev-parse HEAD",
        "failed to verify pinned llama.cpp commit"));
  }
  if (current_commit != kPinnedLlamaCommit) {
    throw std::runtime_error("llama.cpp runtime commit is not pinned to trusted revision");
  }

  run_checked_capture("cmake -S '" + shell_escape_single(src_dir) + "' -B '" +
                          shell_escape_single(build_dir) +
                          "' -DCMAKE_BUILD_TYPE=Release",
                      "failed to configure llama.cpp runtime");
  std::string targets = "llama-completion";
  if (!fs::exists(runtime_cli)) {
    targets += " llama-cli";
  }
  run_checked_capture("cmake --build '" + shell_escape_single(build_dir) +
                          "' --config Release --target " + targets + " -j",
                      "failed to build llama.cpp runtime");

  if (!fs::exists(runtime_completion)) {
    throw std::runtime_error("llama.cpp runtime build succeeded but llama-completion was not found");
  }
  verified_help_output(runtime_completion);
}

std::string resolve_local_runtime_cli(const LocalProvider& provider,
                                      const AppPaths& paths) {
  const std::string configured_cli =
      provider.llama_cli_path.empty() ? "llama-cli" : provider.llama_cli_path;
  if (is_command_available(configured_cli)) {
    const std::string completion = completion_command_for(configured_cli);
    if (!completion.empty()) {
      return completion;
    }
    return configured_cli;
  }

  bootstrap_llama_runtime_if_needed(paths);
  const std::string runtime_completion = runtime_llama_completion_path(paths);
  if (is_command_available(runtime_completion)) {
    return runtime_completion;
  }
  const std::string runtime_cli = runtime_llama_cli_path(paths);
  if (is_command_available(runtime_cli)) {
    const std::string completion = completion_command_for(runtime_cli);
    if (!completion.empty()) {
      return completion;
    }
    return runtime_cli;
  }

  throw std::runtime_error(
      "local runtime not available: failed to provision llama-completion/llama-cli");
}

std::vector<std::string> changed_files_from_diff(const std::string& staged_diff) {
  auto extract_path_from_plus_line = [](const std::string& line) -> std::string {
    if (line.rfind("+++ ", 0) != 0) {
      return "";
    }
    std::string path = line.substr(4);
    if (path == "/dev/null") {
      return "";
    }
    if (path.rfind("b/", 0) == 0) {
      return path.substr(2);
    }
    if (path.size() >= 4 && path.front() == '"' && path.back() == '"') {
      path = path.substr(1, path.size() - 2);
      if (path.rfind("b/", 0) == 0) {
        return path.substr(2);
      }
    }
    return "";
  };

  std::vector<std::string> out;
  std::unordered_set<std::string> seen;
  std::istringstream iss(staged_diff);
  std::string line;
  while (std::getline(iss, line)) {
    const std::string path = extract_path_from_plus_line(line);
    if (!path.empty() && seen.insert(path).second) {
      out.push_back(path);
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

std::string LlmEngine::build_prompt(const StagedChangeContext& context) const {
  std::string prompt;
  prompt.reserve(context.patch.size() + context.name_status.size() + context.numstat.size() +
                 1024);

  prompt +=
      "Generate exactly one Conventional Commit title in English. Return one line only, "
      "no code block, no body, max 72 chars. Allowed types: feat, fix, chore, docs, "
      "refactor, test, perf, ci, build, style, revert.\n"
      "Choose the type and summary from concrete code edits in PATCH hunks. "
      "Do not rely on file names alone. Use NAME_STATUS and NUMSTAT only as support.\n"
      "If changes are mixed, pick the dominant intent.\n\n";

  append_prompt_section(prompt, "NAME_STATUS", context.name_status);
  append_prompt_section(prompt, "NUMSTAT", context.numstat);
  append_prompt_section(prompt, "PATCH", context.patch);
  return prompt;
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

  const std::string cli = resolve_local_runtime_cli(provider, paths_);
  const std::string help = llama_cli_help_text(cli);
  const std::string non_interactive_args = resolve_non_interactive_args(help);
  const std::string gpu_offload_args = resolve_gpu_offload_args(cli, help);
  std::string cmd =
      "'" + shell_escape_single(cli) + "'"
      " -m '" + shell_escape_single(model_path) + "'"
      " -n 96 --temp 0.2 --top-p 0.9" + gpu_offload_args + non_interactive_args +
      " -p '" + shell_escape_single(prompt) + "'"
      " </dev/null 2>&1";

  const CommandResult rc = run_command_capture(cmd);
  if (rc.exit_code != 0) {
    throw std::runtime_error("local llama.cpp inference failed");
  }

  const std::string title = extract_commit_title_from_local_output(rc.output, prompt);
  if (title.empty()) {
    throw std::runtime_error("local llama.cpp output did not include a valid commit title");
  }
  return sanitize_commit_title(title);
}

std::string LlmEngine::generate_commit_message(const StagedChangeContext& context) const {
  if (context.patch.empty()) {
    throw std::runtime_error("no staged diff to generate commit message from");
  }

  const std::string prompt = build_prompt(context);

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
  } catch (const std::exception& ex) {
    std::cerr << "[git-gencommit] warning: LLM generation failed (" << ex.what()
              << "), using builtin fallback\n";
    return builtin_commit_title(context.patch);
  } catch (...) {
    std::cerr << "[git-gencommit] warning: LLM generation failed (unknown error), using builtin fallback\n";
    return builtin_commit_title(context.patch);
  }
}

}  // namespace ggc
