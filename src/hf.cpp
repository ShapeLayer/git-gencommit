#include "hf.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace ggc {
namespace fs = std::filesystem;

namespace {

size_t write_file_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
  std::ofstream* ofs = static_cast<std::ofstream*>(userdata);
  const size_t n = size * nmemb;
  ofs->write(static_cast<const char*>(ptr), static_cast<std::streamsize>(n));
  return n;
}

size_t write_string_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
  std::string* out = static_cast<std::string*>(userdata);
  const size_t n = size * nmemb;
  out->append(static_cast<const char*>(ptr), n);
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

bool starts_with(const std::string& s, const std::string& p) {
  return s.rfind(p, 0) == 0;
}

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

bool ends_with_ci(const std::string& s, const std::string& suffix) {
  if (s.size() < suffix.size()) {
    return false;
  }
  const std::string tail = s.substr(s.size() - suffix.size());
  return to_lower(tail) == to_lower(suffix);
}

std::string model_cache_filename(const std::string& key) {
  const std::string safe = sanitize_name(key);
  if (ends_with_ci(safe, ".gguf")) {
    return safe;
  }
  return safe + ".gguf";
}

std::string url_encode(const std::string& s) {
  std::ostringstream oss;
  oss.fill('0');
  oss << std::hex;
  for (const unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      oss << static_cast<char>(c);
    } else {
      oss << '%' << std::uppercase;
      oss.width(2);
      oss << static_cast<int>(c);
      oss << std::nouppercase;
    }
  }
  return oss.str();
}

std::string url_encode_path(const std::string& s) {
  std::ostringstream oss;
  oss.fill('0');
  oss << std::hex;
  for (const unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
      oss << static_cast<char>(c);
    } else {
      oss << '%' << std::uppercase;
      oss.width(2);
      oss << static_cast<int>(c);
      oss << std::nouppercase;
    }
  }
  return oss.str();
}

std::vector<std::string> json_extract_array_string_field(const std::string& json,
                                                         const std::string& field,
                                                         int limit) {
  std::vector<std::string> out;
  if (limit <= 0) {
    return out;
  }

  auto parse_json_string = [&](size_t begin, std::string* value) -> size_t {
    if (begin >= json.size() || json[begin] != '"') {
      return std::string::npos;
    }
    size_t i = begin + 1;
    value->clear();
    while (i < json.size()) {
      char c = json[i++];
      if (c == '"') {
        return i;
      }
      if (c == '\\' && i < json.size()) {
        const char e = json[i++];
        if (e == 'n') {
          value->push_back('\n');
        } else if (e == 'r') {
          value->push_back('\r');
        } else if (e == 't') {
          value->push_back('\t');
        } else {
          value->push_back(e);
        }
        continue;
      }
      value->push_back(c);
    }
    return std::string::npos;
  };

  const std::string needle = "\"" + field + "\"";
  size_t pos = 0;
  while (pos < json.size() && static_cast<int>(out.size()) < limit) {
    size_t k = json.find(needle, pos);
    if (k == std::string::npos) {
      break;
    }
    size_t i = k + needle.size();
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i])) != 0) {
      ++i;
    }
    if (i >= json.size() || json[i] != ':') {
      pos = k + 1;
      continue;
    }
    ++i;
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i])) != 0) {
      ++i;
    }
    if (i >= json.size() || json[i] != '"') {
      pos = k + 1;
      continue;
    }

    std::string value;
    const size_t next = parse_json_string(i, &value);
    if (next == std::string::npos) {
      break;
    }
    if (!value.empty()) {
      out.push_back(value);
    }
    pos = next;
  }

  return out;
}

std::string http_get(const std::string& url, long* status) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    throw std::runtime_error("curl_easy_init failed");
  }

  constexpr int kMaxRetries = 2;
  CURLcode last_rc = CURLE_OK;
  long last_code = 0;
  std::string body;

  for (int attempt = 0; attempt <= kMaxRetries; ++attempt) {
    body.clear();
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "git-gencommit/0.1");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    last_rc = curl_easy_perform(curl);
    last_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &last_code);

    const bool retriable = (last_rc != CURLE_OK) || last_code == 429 || last_code >= 500;
    if (!retriable || attempt == kMaxRetries) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250 * (1 << attempt)));
  }

  curl_easy_cleanup(curl);

  if (status != nullptr) {
    *status = last_code;
  }
  if (last_rc != CURLE_OK) {
    throw std::runtime_error("HTTP request failed");
  }
  return body;
}

std::string pick_gguf_filename_from_model_meta(const std::string& model_repo) {
  long status = 0;
  const std::string body =
      http_get("https://huggingface.co/api/models/" + url_encode_path(model_repo), &status);
  if (status < 200 || status >= 300) {
    throw std::runtime_error("failed to inspect model repository on Hugging Face (HTTP " +
                             std::to_string(status) + ")");
  }

  const std::vector<std::string> files = json_extract_array_string_field(body, "rfilename", 512);
  std::vector<std::string> ggufs;
  ggufs.reserve(files.size());
  for (const std::string& f : files) {
    if (ends_with_ci(f, ".gguf")) {
      ggufs.push_back(f);
    }
  }
  if (ggufs.empty()) {
    throw std::runtime_error("no GGUF file found in repository: " + model_repo +
                             " (select a GGUF repo/file from search results)");
  }

  auto score = [](const std::string& f) -> int {
    const std::string lf = to_lower(f);
    if (lf == "model.gguf") {
      return 0;
    }
    if (lf.find("q4_k_m") != std::string::npos) {
      return 1;
    }
    if (lf.find("q4") != std::string::npos) {
      return 2;
    }
    return 10;
  };

  std::sort(ggufs.begin(), ggufs.end(), [&](const std::string& a, const std::string& b) {
    const int sa = score(a);
    const int sb = score(b);
    if (sa != sb) {
      return sa < sb;
    }
    return a < b;
  });
  return ggufs.front();
}

struct DownloadSpec {
  std::string url;
  std::string key_for_filename;
};

DownloadSpec resolve_download_spec(const std::string& model_name_or_url) {
  if (model_name_or_url.empty()) {
    throw std::runtime_error("invalid model identifier or URL");
  }

  if (starts_with(model_name_or_url, "http://")) {
    throw std::runtime_error("HTTP is not allowed for model download URL");
  }

  if (starts_with(model_name_or_url, "https://")) {
    return {model_name_or_url, model_name_or_url};
  }

  const size_t first = model_name_or_url.find('/');
  if (first == std::string::npos || first == 0 || first + 1 >= model_name_or_url.size()) {
    throw std::runtime_error("invalid model identifier or URL");
  }

  const size_t second = model_name_or_url.find('/', first + 1);
  if (second == std::string::npos) {
    const std::string gguf = pick_gguf_filename_from_model_meta(model_name_or_url);
    const std::string url = "https://huggingface.co/" + model_name_or_url +
                            "/resolve/main/" + url_encode_path(gguf);
    return {url, model_name_or_url + "/" + gguf};
  }

  const std::string repo = model_name_or_url.substr(0, second);
  const std::string file = model_name_or_url.substr(second + 1);
  if (file.empty()) {
    throw std::runtime_error("invalid model identifier or URL");
  }
  const std::string url =
      "https://huggingface.co/" + repo + "/resolve/main/" + url_encode_path(file);
  return {url, model_name_or_url};
}

std::string download_model_file(const std::string& url,
                                const std::string& target_path) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    throw std::runtime_error("curl_easy_init failed");
  }

  std::ofstream ofs(target_path, std::ios::binary | std::ios::out | std::ios::trunc);
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
    fs::remove(target_path);
    throw std::runtime_error("failed to download model from Hugging Face (HTTP " +
                             std::to_string(code) + ")");
  }
  return target_path;
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

  if (provider.model_name.empty()) {
    return provider.model_path;
  }
  return hf_download_model(provider.model_name, paths);
}

std::string hf_download_model(const std::string& model_name_or_url,
                              const AppPaths& paths) {
  const DownloadSpec spec = resolve_download_spec(model_name_or_url);

  fs::create_directories(paths.models_dir);
  const std::string file_name = model_cache_filename(spec.key_for_filename);
  const fs::path target = fs::path(paths.models_dir) / file_name;
  if (fs::exists(target)) {
    return target.string();
  }
  return download_model_file(spec.url, target.string());
}

std::string hf_remove_model(const std::string& model_name_or_url_or_path,
                            const AppPaths& paths) {
  if (model_name_or_url_or_path.empty()) {
    throw std::runtime_error("model identifier or path is empty");
  }

  fs::path target;
  const fs::path input_path(model_name_or_url_or_path);
  if (input_path.is_absolute() || fs::exists(input_path)) {
    target = input_path;
  } else {
    std::vector<fs::path> candidates;
    candidates.push_back(fs::path(paths.models_dir) /
                         model_cache_filename(model_name_or_url_or_path));
    candidates.push_back(fs::path(paths.models_dir) /
                         (model_cache_filename(model_name_or_url_or_path) + ".gguf"));

    try {
      const DownloadSpec spec = resolve_download_spec(model_name_or_url_or_path);
      candidates.push_back(fs::path(paths.models_dir) / model_cache_filename(spec.key_for_filename));
      candidates.push_back(fs::path(paths.models_dir) /
                           (model_cache_filename(spec.key_for_filename) + ".gguf"));
    } catch (...) {
    }

    for (const fs::path& p : candidates) {
      if (fs::exists(p)) {
        target = p;
        break;
      }
    }
    if (target.empty()) {
      target = candidates.front();
    }
  }

  if (!fs::exists(target)) {
    throw std::runtime_error("model file not found: " + target.string());
  }
  if (!fs::is_regular_file(target)) {
    throw std::runtime_error("model path is not a file: " + target.string());
  }
  fs::remove(target);
  return target.string();
}

std::vector<std::string> hf_search_models(const std::string& query, int limit) {
  if (query.empty()) {
    return {};
  }
  if (limit <= 0) {
    limit = 5;
  }
  if (limit > 30) {
    limit = 30;
  }

  long status = 0;
  const std::string url = "https://huggingface.co/api/models?search=" + url_encode(query) +
                          "&limit=" + std::to_string(limit) + "&full=false&filter=gguf";
  const std::string body = http_get(url, &status);
  if (status < 200 || status >= 300) {
    throw std::runtime_error("failed to search Hugging Face models");
  }

  std::vector<std::string> ids = json_extract_array_string_field(body, "id", limit);
  if (ids.empty()) {
    ids = json_extract_array_string_field(body, "modelId", limit);
  }
  return ids;
}

}  // namespace ggc
