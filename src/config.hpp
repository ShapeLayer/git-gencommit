#pragma once

#include <string>
#include <unordered_map>

namespace ggc {

struct Config {
  bool run_on_startup = false;
  bool unload_after_commit = true;
  std::string default_provider = "";
  std::string commit_template = "{type}: {summary}";
  bool auto_download_models = true;
};

struct ExternalProvider {
  std::string base_url;
  std::string model;
  std::string api_key;
  int timeout_sec = 30;
  int max_retries = 2;
};

struct LocalProvider {
  std::string model_name;
  std::string model_path;
  std::string llama_cli_path;
};

struct ProviderRegistry {
  std::unordered_map<std::string, LocalProvider> local_models;
  std::unordered_map<std::string, ExternalProvider> external;
};

struct AppPaths {
  std::string home;
  std::string root;
  std::string models_dir;
  std::string config_toml;
  std::string providers_toml;
  std::string cache_dir;
};

AppPaths resolve_app_paths();
void ensure_app_layout(const AppPaths& paths);
bool app_config_files_exist(const AppPaths& paths);
Config load_config(const AppPaths& paths);
ProviderRegistry load_providers(const AppPaths& paths);
void save_config(const AppPaths& paths, const Config& cfg);
void save_providers(const AppPaths& paths, const ProviderRegistry& providers);

}  // namespace ggc
