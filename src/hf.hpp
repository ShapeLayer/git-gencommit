#pragma once

#include "config.hpp"

#include <string>
#include <vector>

namespace ggc {

std::string hf_download_if_needed(const LocalProvider& provider,
                                  const AppPaths& paths,
                                  bool allow_download);
std::string hf_download_model(const std::string& model_name_or_url,
                              const AppPaths& paths);
std::string hf_remove_model(const std::string& model_name_or_url_or_path,
                            const AppPaths& paths);
std::vector<std::string> hf_search_models(const std::string& query, int limit);

}  // namespace ggc
