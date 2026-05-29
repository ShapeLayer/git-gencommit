#pragma once

#include "config.hpp"

#include <string>

namespace ggc {

std::string hf_download_if_needed(const LocalProvider& provider,
                                  const AppPaths& paths,
                                  bool allow_download);

}  // namespace ggc
