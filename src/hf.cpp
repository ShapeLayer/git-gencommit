#include "hf.hpp"

#include <curl/curl.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace ggc {
namespace fs = std::filesystem;

namespace {

size_t write_file_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
  std::ofstream* ofs = static_cast<std::ofstream*>(userdata);
  const size_t n = size * nmemb;
  ofs->write(static_cast<const char*>(ptr), static_cast<std::streamsize>(n));
  return n;
}

std::string sanitize_name(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
      out.push_back(c);
    } else {
      out.push_back('_');
    }
  }
  return out;
}

std::string infer_model_url(const std::string& model_name) {
  if (model_name.empty()) {
    return "";
  }
  if (model_name.rfind("http://", 0) == 0) {
    return "";
  }
  if (model_name.rfind("https://", 0) == 0) {
    return model_name;
  }
  if (model_name.find('/') == std::string::npos) {
    return "";
  }
  return "https://huggingface.co/" + model_name + "/resolve/main/model.gguf";
}

}  // namespace

std::string hf_download_if_needed(const LocalProvider& provider,
                                  const AppPaths& paths,
                                  bool allow_download) {
  if (!provider.model_path.empty() && fs::exists(provider.model_path)) {
    return provider.model_path;
  }
  if (!allow_download) {
    return provider.model_path;
  }

  const std::string url = infer_model_url(provider.model_name);
  if (url.empty()) {
    return provider.model_path;
  }

  fs::create_directories(paths.models_dir);
  const std::string file_name = sanitize_name(provider.model_name) + ".gguf";
  const fs::path target = fs::path(paths.models_dir) / file_name;
  if (fs::exists(target)) {
    return target.string();
  }

  CURL* curl = curl_easy_init();
  if (!curl) {
    throw std::runtime_error("curl_easy_init failed");
  }

  std::ofstream ofs(target, std::ios::binary | std::ios::out | std::ios::trunc);
  if (!ofs.is_open()) {
    curl_easy_cleanup(curl);
    throw std::runtime_error("failed to open model file for writing");
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ofs);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "git-gencommit/0.1");

  const CURLcode rc = curl_easy_perform(curl);
  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(curl);
  ofs.close();

  if (rc != CURLE_OK || code < 200 || code >= 300) {
    fs::remove(target);
    throw std::runtime_error("failed to download model from Hugging Face");
  }

  return target.string();
}

}  // namespace ggc
