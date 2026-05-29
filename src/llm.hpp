#pragma once

#include "config.hpp"

#include <string>

namespace ggc {

class LlmEngine {
 public:
  LlmEngine(Config cfg, ProviderRegistry providers, AppPaths paths);

  void load_if_needed();
  void unload_if_needed();

  std::string generate_commit_message(const std::string& staged_diff) const;

 private:
  std::string build_prompt(const std::string& staged_diff) const;
  std::string generate_with_external(const ExternalProvider& provider,
                                     const std::string& prompt) const;
  std::string generate_with_local(const LocalProvider& provider,
                                  const std::string& prompt) const;

  Config config_;
  ProviderRegistry providers_;
  AppPaths paths_;
};

}  // namespace ggc
