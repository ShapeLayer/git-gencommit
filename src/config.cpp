#include "config.hpp"

#include "toml_lite.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ggc {
namespace fs = std::filesystem;

namespace {

std::string read_file(const std::string& path) {
  std::ifstream ifs(path);
  if (!ifs.is_open()) {
    return "";
  }
  return std::string((std::istreambuf_iterator<char>(ifs)),
                     std::istreambuf_iterator<char>());
}

void write_if_missing(const std::string& path, const std::string& content) {
  if (fs::exists(path)) {
    return;
  }
  std::ofstream ofs(path, std::ios::out | std::ios::trunc);
  ofs << content;
}

bool to_bool(const std::string& s, bool fallback) {
  if (s == "true" || s == "1") {
    return true;
  }
  if (s == "false" || s == "0") {
    return false;
  }
  return fallback;
}

int to_int(const std::string& s, int fallback) {
  if (s.empty()) {
    return fallback;
  }
  std::istringstream iss(s);
  int v = fallback;
  iss >> v;
  if (iss.fail()) {
    return fallback;
  }
  return v;
}

}  // namespace

AppPaths resolve_app_paths() {
  const char* home_c = std::getenv("HOME");
  if (!home_c || std::string(home_c).empty()) {
    throw std::runtime_error("HOME is not set");
  }
  AppPaths paths;
  paths.home = home_c;
  paths.root = paths.home + "/.gitgencommit";
  paths.models_dir = paths.root + "/models";
  paths.config_toml = paths.root + "/config.toml";
  paths.providers_toml = paths.root + "/providers.toml";
  paths.cache_dir = paths.root + "/cache";
  return paths;
}

void ensure_app_layout(const AppPaths& paths) {
  fs::create_directories(paths.models_dir);
  fs::create_directories(paths.cache_dir);

  const std::string default_config =
      "[app]\n"
      "run_on_startup = false\n"
      "unload_after_commit = true\n"
      "\n"
      "[commit]\n"
      "template = \"{type}: {summary}\"\n"
      "default_provider = \"\"\n"
      "auto_download_models = true\n";

  const std::string default_providers =
      "# Local model registry\n"
      "# [local.<key>]\n"
      "# model_name = \"Qwen/Qwen2.5-Coder-7B-Instruct\"\n"
      "# model_path = \"/Users/<name>/.gitgencommit/models/qwen2.5-coder-7b\"\n"
      "# llama_cli_path = \"/absolute/path/to/llama-cli\"\n"
      "\n"
      "# External OpenAI-compatible provider\n"
      "# [external.<key>]\n"
      "# base_url = \"https://api.openai.com/v1\"\n"
      "# model = \"gpt-4o-mini\"\n"
      "# api_key = \"sk-...\"\n"
      "# timeout_sec = 30\n"
      "# max_retries = 2\n";

  write_if_missing(paths.config_toml, default_config);
  write_if_missing(paths.providers_toml, default_providers);
}

Config load_config(const AppPaths& paths) {
  Config cfg;
  const std::string raw = read_file(paths.config_toml);
  const TomlDoc doc = parse_toml_lite(raw);

  cfg.run_on_startup = to_bool(toml_get(doc, "app", "run_on_startup", "false"), false);
  cfg.unload_after_commit =
      to_bool(toml_get(doc, "app", "unload_after_commit", "true"), true);
  cfg.default_provider = toml_get(doc, "commit", "default_provider", "");
  cfg.commit_template = toml_get(doc, "commit", "template", "{type}: {summary}");
  cfg.auto_download_models =
      to_bool(toml_get(doc, "commit", "auto_download_models", "true"), true);
  return cfg;
}

ProviderRegistry load_providers(const AppPaths& paths) {
  ProviderRegistry pr;
  const std::string raw = read_file(paths.providers_toml);
  const TomlDoc doc = parse_toml_lite(raw);

  for (const auto& table : toml_tables_with_prefix(doc, "local.")) {
    const std::string key = table.substr(std::string("local.").size());
    LocalProvider lp;
    lp.model_name = toml_get(doc, table, "model_name", "");
    lp.model_path = toml_get(doc, table, "model_path", "");
    lp.llama_cli_path = toml_get(doc, table, "llama_cli_path", "llama-cli");
    if (!key.empty() && (!lp.model_path.empty() || !lp.model_name.empty())) {
      pr.local_models[key] = lp;
    }
  }

  for (const auto& table : toml_tables_with_prefix(doc, "external.")) {
    const std::string key = table.substr(std::string("external.").size());
    ExternalProvider ep;
    ep.base_url = toml_get(doc, table, "base_url", "");
    ep.model = toml_get(doc, table, "model", "");
    ep.api_key = toml_get(doc, table, "api_key", "");
    ep.timeout_sec = to_int(toml_get(doc, table, "timeout_sec", "30"), 30);
    ep.max_retries = to_int(toml_get(doc, table, "max_retries", "2"), 2);
    if (!key.empty()) {
      pr.external[key] = ep;
    }
  }

  return pr;
}

}  // namespace ggc
