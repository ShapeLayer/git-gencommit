#include "config_wizard.hpp"

#include "hf.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

namespace ggc {
namespace {

enum class KeyType {
  Up,
  Down,
  Left,
  Right,
  Enter,
  Backspace,
  Character,
  CtrlC,
  Unknown,
};

struct KeyEvent {
  KeyType type = KeyType::Unknown;
  char ch = '\0';
};

class ScopedRawMode {
 public:
  ScopedRawMode() {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
      throw std::runtime_error("configuration wizard requires an interactive TTY");
    }
    if (tcgetattr(STDIN_FILENO, &original_) != 0) {
      throw std::runtime_error("failed to read terminal attributes");
    }
    termios raw = original_;
    raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | ISIG));
    raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
      throw std::runtime_error("failed to enable raw terminal mode");
    }
    enabled_ = true;
  }

  ~ScopedRawMode() {
    if (enabled_) {
      tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
    }
  }

  ScopedRawMode(const ScopedRawMode&) = delete;
  ScopedRawMode& operator=(const ScopedRawMode&) = delete;

 private:
  termios original_{};
  bool enabled_ = false;
};

void clear_screen() {
  std::cout << "\033[2J\033[H";
}

KeyEvent read_key() {
  char c = '\0';
  const ssize_t n = ::read(STDIN_FILENO, &c, 1);
  if (n <= 0) {
    return {KeyType::Unknown, '\0'};
  }

  if (c == '\003') {
    return {KeyType::CtrlC, '\0'};
  }
  if (c == '\r' || c == '\n') {
    return {KeyType::Enter, '\0'};
  }
  if (c == 127 || c == '\b') {
    return {KeyType::Backspace, '\0'};
  }
  if (c == '\033') {
    char seq[2] = {'\0', '\0'};
    if (::read(STDIN_FILENO, &seq[0], 1) <= 0) {
      return {KeyType::Unknown, '\0'};
    }
    if (::read(STDIN_FILENO, &seq[1], 1) <= 0) {
      return {KeyType::Unknown, '\0'};
    }
    if (seq[0] == '[') {
      if (seq[1] == 'A') {
        return {KeyType::Up, '\0'};
      }
      if (seq[1] == 'B') {
        return {KeyType::Down, '\0'};
      }
      if (seq[1] == 'C') {
        return {KeyType::Right, '\0'};
      }
      if (seq[1] == 'D') {
        return {KeyType::Left, '\0'};
      }
    }
    return {KeyType::Unknown, '\0'};
  }

  if (std::isprint(static_cast<unsigned char>(c)) != 0) {
    return {KeyType::Character, c};
  }
  return {KeyType::Unknown, '\0'};
}

std::string on_off(bool value) {
  return value ? "true" : "false";
}

std::string invert_video(const std::string& text) {
  return "\033[7m" + text + "\033[0m";
}

size_t terminal_width() {
  winsize ws{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
    return static_cast<size_t>(ws.ws_col);
  }
  return 120;
}

size_t visible_width_ansi(const std::string& text) {
  size_t width = 0;
  size_t i = 0;
  while (i < text.size()) {
    const unsigned char ch = static_cast<unsigned char>(text[i]);
    if (ch == 0x1B && i + 1 < text.size() && text[i + 1] == '[') {
      i += 2;
      while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c >= 0x40 && c <= 0x7E) {
          ++i;
          break;
        }
        ++i;
      }
      continue;
    }

    if ((ch & 0xC0) != 0x80) {
      ++width;
    }
    ++i;
  }
  return width;
}

std::string focus_row_full_width(const std::string& row, bool focused) {
  if (!focused) {
    return row;
  }

  const size_t cols = terminal_width();
  const size_t used = visible_width_ansi(row);
  std::string padded = row;
  if (used < cols) {
    padded.append(cols - used, ' ');
  }
  return invert_video(padded);
}

std::string selector_value(const std::string& text, bool focused) {
  if (focused) {
    return "\033[27;39;49m " + text + " \033[7m";
  }
  return invert_video(" " + text + " ");
}

std::string left_right_selector(const std::string& text, bool focused) {
  const std::string left = focused ? "\u25C0" : " ";
  const std::string right = focused ? "\u25B6" : " ";
  return left + " " + selector_value(text, focused) + " " + right;
}

std::string template_with_cursor(const std::string& value, size_t cursor, bool focused) {
  if (!focused) {
    return value;
  }
  if (value.empty()) {
    return "";
  }

  const size_t pos = std::min(cursor, value.size() - 1);
  std::string out;
  if (pos > 0) {
    out += value.substr(0, pos);
  }
  out += "\033[27;39;49m";
  out.push_back(value[pos]);
  out += "\033[7m";
  if (pos + 1 < value.size()) {
    out += value.substr(pos + 1);
  }
  return out;
}

void print_common_guide() {
  std::cout << "\n";
  std::cout << "Guide: \u2191\u2193 move  Enter next\n";
}

void check_ctrl_c(const KeyEvent& ev) {
  if (ev.type == KeyType::CtrlC) {
    throw std::runtime_error("configuration wizard cancelled");
  }
}

void adjust_focus(int* focus, int count, KeyType key) {
  if (count <= 0) {
    *focus = 0;
    return;
  }
  if (key == KeyType::Up) {
    *focus = (*focus - 1 + count) % count;
  }
  if (key == KeyType::Down) {
    *focus = (*focus + 1) % count;
  }
}

void render_global_screen(const Config& cfg, int focus, size_t template_cursor) {
  clear_screen();
  std::cout << "git-gencommit configuration wizard\n";
  std::cout << "Step 1/4: app and commit settings\n\n";

  const std::string row0 = "  load llm on start system = " +
                           left_right_selector(on_off(cfg.run_on_startup), focus == 0);
  const std::string row1 = "  unload after commit = " +
                           left_right_selector(on_off(cfg.unload_after_commit), focus == 1);
  const std::string row2 = "  template = \"" +
                           template_with_cursor(cfg.commit_template, template_cursor, focus == 2) +
                           "\"";
  const std::string row3 = "  auto download models = " +
                           left_right_selector(on_off(cfg.auto_download_models), focus == 3);

  std::cout << focus_row_full_width(row0, focus == 0) << "\n";
  std::cout << focus_row_full_width(row1, focus == 1) << "\n";
  std::cout << focus_row_full_width(row2, focus == 2) << "\n";
  std::cout << focus_row_full_width(row3, focus == 3) << "\n";

  if (focus == 2) {
    std::cout << "\nTemplate edit: \u2190\u2192 move cursor, type/backspace edit\n";
  }

  print_common_guide();
}

void run_global_form(Config* cfg) {
  int focus = 0;
  size_t template_cursor = cfg->commit_template.size();
  constexpr int kFieldCount = 4;

  while (true) {
    if (template_cursor > cfg->commit_template.size()) {
      template_cursor = cfg->commit_template.size();
    }

    render_global_screen(*cfg, focus, template_cursor);
    const KeyEvent ev = read_key();
    check_ctrl_c(ev);

    if (ev.type == KeyType::Up || ev.type == KeyType::Down) {
      adjust_focus(&focus, kFieldCount, ev.type);
      continue;
    }

    if ((ev.type == KeyType::Left || ev.type == KeyType::Right) && focus == 2) {
      if (ev.type == KeyType::Left && template_cursor > 0) {
        --template_cursor;
      }
      if (ev.type == KeyType::Right && template_cursor < cfg->commit_template.size()) {
        ++template_cursor;
      }
      continue;
    }

    if ((ev.type == KeyType::Left || ev.type == KeyType::Right) &&
        (focus == 0 || focus == 1 || focus == 3)) {
      if (focus == 0) {
        cfg->run_on_startup = !cfg->run_on_startup;
      }
      if (focus == 1) {
        cfg->unload_after_commit = !cfg->unload_after_commit;
      }
      if (focus == 3) {
        cfg->auto_download_models = !cfg->auto_download_models;
      }
      continue;
    }

    if (focus == 2 && ev.type == KeyType::Character) {
      cfg->commit_template.insert(cfg->commit_template.begin() + static_cast<long>(template_cursor),
                                  ev.ch);
      ++template_cursor;
      continue;
    }

    if (focus == 2 && ev.type == KeyType::Backspace) {
      if (template_cursor > 0 && !cfg->commit_template.empty()) {
        cfg->commit_template.erase(cfg->commit_template.begin() + static_cast<long>(template_cursor - 1));
        --template_cursor;
      }
      continue;
    }

    if (ev.type == KeyType::Enter) {
      if (focus == kFieldCount - 1) {
        break;
      }
      ++focus;
      continue;
    }
  }
}

bool run_yes_no_screen(const std::string& title, const std::string& question, bool default_yes) {
  bool choice = default_yes;
  while (true) {
    clear_screen();
    std::cout << "git-gencommit configuration wizard\n";
    std::cout << title << "\n\n";
    std::cout << question << "\n";
    const std::string row = "  " + left_right_selector(choice ? "yes" : "no", true);
    std::cout << focus_row_full_width(row, true) << "\n";
    std::cout << "\nGuide: Enter select\n";

    const KeyEvent ev = read_key();
    check_ctrl_c(ev);

    if (ev.type == KeyType::Left || ev.type == KeyType::Right) {
      choice = !choice;
      continue;
    }
    if (ev.type == KeyType::Enter) {
      return choice;
    }
  }
}

bool is_valid_provider_id(const std::string& id) {
  if (id.empty()) {
    return false;
  }
  for (char ch : id) {
    const bool ok = std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' || ch == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

bool is_non_negative_int(const std::string& s) {
  if (s.empty()) {
    return false;
  }
  for (char ch : s) {
    if (!std::isdigit(static_cast<unsigned char>(ch))) {
      return false;
    }
  }
  return true;
}

int parse_or_default(const std::string& s, int fallback) {
  if (!is_non_negative_int(s)) {
    return fallback;
  }
  try {
    return std::stoi(s);
  } catch (...) {
    return fallback;
  }
}

std::string sanitize_provider_id_token(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  bool last_underscore = false;
  for (char ch : value) {
    const unsigned char u = static_cast<unsigned char>(ch);
    if (std::isalnum(u) != 0) {
      out.push_back(static_cast<char>(std::tolower(u)));
      last_underscore = false;
      continue;
    }
    if (!last_underscore) {
      out.push_back('_');
      last_underscore = true;
    }
  }
  while (!out.empty() && out.front() == '_') {
    out.erase(out.begin());
  }
  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }
  if (out.empty()) {
    out = "model";
  }
  return out;
}

std::string dim_text(const std::string& text) {
  return "\033[2m" + text + "\033[22m";
}

enum class ProviderType {
  Local,
  External,
};

struct ProviderDraft {
  std::string provider_id;
  ProviderType type = ProviderType::Local;
  std::string model_name;
  std::string model_path;
  std::string llama_cli_path = "llama-cli";
  std::string base_url = "https://api.openai.com/v1";
  std::string model = "gpt-4o-mini";
  std::string api_key;
  std::string timeout_sec = "30";
  std::string max_retries = "2";
};

enum class ProviderField {
  ProviderId,
  Type,
  LocalDownload,
  ModelName,
  ModelPath,
  LlamaCliPath,
  BaseUrl,
  Model,
  ApiKey,
  Timeout,
  MaxRetries,
};

std::vector<ProviderField> active_provider_fields(const ProviderDraft& draft) {
  std::vector<ProviderField> out;
  out.push_back(ProviderField::ProviderId);
  out.push_back(ProviderField::Type);
  if (draft.type == ProviderType::Local) {
    out.push_back(ProviderField::LocalDownload);
    out.push_back(ProviderField::ModelName);
    out.push_back(ProviderField::ModelPath);
    out.push_back(ProviderField::LlamaCliPath);
  } else {
    out.push_back(ProviderField::BaseUrl);
    out.push_back(ProviderField::Model);
    out.push_back(ProviderField::ApiKey);
    out.push_back(ProviderField::Timeout);
    out.push_back(ProviderField::MaxRetries);
  }
  return out;
}

std::string provider_type_name(ProviderType t) {
  return t == ProviderType::Local ? "local" : "external";
}

std::string* mutable_provider_field(ProviderDraft* draft, ProviderField f) {
  if (f == ProviderField::ProviderId) {
    return &draft->provider_id;
  }
  if (f == ProviderField::ModelName) {
    return &draft->model_name;
  }
  if (f == ProviderField::ModelPath) {
    return &draft->model_path;
  }
  if (f == ProviderField::LlamaCliPath) {
    return &draft->llama_cli_path;
  }
  if (f == ProviderField::BaseUrl) {
    return &draft->base_url;
  }
  if (f == ProviderField::Model) {
    return &draft->model;
  }
  if (f == ProviderField::ApiKey) {
    return &draft->api_key;
  }
  if (f == ProviderField::Timeout) {
    return &draft->timeout_sec;
  }
  if (f == ProviderField::MaxRetries) {
    return &draft->max_retries;
  }
  return nullptr;
}

std::string provider_field_label(ProviderField f) {
  if (f == ProviderField::ProviderId) {
    return "provider id";
  }
  if (f == ProviderField::Type) {
    return "provider type";
  }
  if (f == ProviderField::LocalDownload) {
    return "download local model";
  }
  if (f == ProviderField::ModelName) {
    return "model_name";
  }
  if (f == ProviderField::ModelPath) {
    return "model_path";
  }
  if (f == ProviderField::LlamaCliPath) {
    return "llama_cli_path";
  }
  if (f == ProviderField::BaseUrl) {
    return "base_url";
  }
  if (f == ProviderField::Model) {
    return "model";
  }
  if (f == ProviderField::ApiKey) {
    return "api_key";
  }
  if (f == ProviderField::Timeout) {
    return "timeout_sec";
  }
  return "max_retries";
}

std::string provider_field_value(const ProviderDraft& draft, ProviderField f, bool focused) {
  if (f == ProviderField::Type) {
    return left_right_selector(provider_type_name(draft.type), focused);
  }
  if (f == ProviderField::LocalDownload) {
    return "press Enter";
  }
  ProviderDraft copy = draft;
  std::string* value = mutable_provider_field(&copy, f);
  if (!value) {
    return "";
  }
  return *value;
}

void render_local_download_screen(const std::string& query,
                                  const std::vector<std::string>& results,
                                  int focus,
                                  const std::string& status,
                                  const std::string& error_message) {
  clear_screen();
  std::cout << "git-gencommit configuration wizard\n";
  std::cout << "Step 2/4: local model search/download\n\n";

  std::vector<std::string> rows;
  const std::string query_view =
      query.empty() ? dim_text("Qwen/Qwen3.5-0.8B") : query;
  rows.push_back("  search query = " + query_view);
  rows.push_back("  search models");
  rows.push_back("  download query as model id/url");
  for (const std::string& r : results) {
    rows.push_back("  result: " + r);
  }
  rows.push_back("  back to provider form");

  for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
    std::cout << focus_row_full_width(rows[i], i == focus) << "\n";
  }

  if (!status.empty()) {
    std::cout << "\nstatus: " << status << "\n";
  }
  if (!error_message.empty()) {
    std::cout << "error: " << error_message << "\n";
  }

  if (focus == 0) {
    std::cout << "\nQuery edit: type/backspace, Enter on 'search models'\n";
  }
  std::cout << "Guide: \u2191\u2193 move  Enter select\n";
}

void fill_local_provider_from_download(ProviderDraft* draft,
                                       const std::string& model_name,
                                       const std::string& model_path) {
  draft->model_name = model_name;
  draft->model_path = model_path;
  if (draft->llama_cli_path.empty()) {
    draft->llama_cli_path = "llama-cli";
  }
  if (draft->provider_id.empty()) {
    draft->provider_id = "local_" + sanitize_provider_id_token(model_name);
  }
}

void run_local_model_download_page(const AppPaths& paths, ProviderDraft* draft) {
  std::string query = draft->model_name;
  std::vector<std::string> results;
  int focus = 0;
  std::string status;
  std::string error_message;

  while (true) {
    const int row_count = 4 + static_cast<int>(results.size());
    if (focus < 0) {
      focus = 0;
    }
    if (focus >= row_count) {
      focus = row_count - 1;
    }

    render_local_download_screen(query, results, focus, status, error_message);
    const KeyEvent ev = read_key();
    check_ctrl_c(ev);

    if (ev.type == KeyType::Up || ev.type == KeyType::Down) {
      adjust_focus(&focus, row_count, ev.type);
      continue;
    }

    if (focus == 0 && ev.type == KeyType::Character) {
      query.push_back(ev.ch);
      error_message.clear();
      continue;
    }

    if (focus == 0 && ev.type == KeyType::Backspace) {
      if (!query.empty()) {
        query.pop_back();
      }
      error_message.clear();
      continue;
    }

    if (ev.type != KeyType::Enter) {
      continue;
    }

    if (focus == 1) {
      if (query.empty()) {
        error_message = "search query is empty";
        continue;
      }
      try {
        status = "searching Hugging Face...";
        error_message.clear();
        render_local_download_screen(query, results, focus, status, error_message);
        results = hf_search_models(query, 8);
        status = "search completed: " + std::to_string(results.size()) + " result(s)";
      } catch (const std::exception& ex) {
        error_message = ex.what();
      }
      continue;
    }

    if (focus == 2) {
      if (query.empty()) {
        error_message = "query is empty";
        continue;
      }
      try {
        status = "downloading model...";
        error_message.clear();
        render_local_download_screen(query, results, focus, status, error_message);
        const std::string model_path = hf_download_model(query, paths);
        fill_local_provider_from_download(draft, query, model_path);
        return;
      } catch (const std::exception& ex) {
        error_message = ex.what();
      }
      continue;
    }

    const int result_start = 3;
    const int back_row = result_start + static_cast<int>(results.size());
    if (focus >= result_start && focus < back_row) {
      const std::string model_id = results[focus - result_start];
      try {
        status = "downloading model...";
        error_message.clear();
        render_local_download_screen(query, results, focus, status, error_message);
        const std::string model_path = hf_download_model(model_id, paths);
        fill_local_provider_from_download(draft, model_id, model_path);
        return;
      } catch (const std::exception& ex) {
        error_message = ex.what();
      }
      continue;
    }

    if (focus == back_row) {
      return;
    }
  }
}

void render_provider_screen(const ProviderDraft& draft, int focus, const std::string& error_message) {
  clear_screen();
  std::cout << "git-gencommit configuration wizard\n";
  std::cout << "Step 2/4: add provider\n\n";

  const std::vector<ProviderField> fields = active_provider_fields(draft);
  for (size_t i = 0; i < fields.size(); ++i) {
    const ProviderField field = fields[i];
    const bool active = static_cast<int>(i) == focus;
    const std::string row = "  " + provider_field_label(field) + " = " +
                            provider_field_value(draft, field, active);
    std::cout << focus_row_full_width(row, active) << "\n";
  }

  if (!error_message.empty()) {
    std::cout << "\nerror: " << error_message << "\n";
  }

  print_common_guide();
}

void apply_provider_draft(const ProviderDraft& draft, ProviderRegistry* providers) {
  if (draft.type == ProviderType::Local) {
    LocalProvider lp;
    lp.model_name = draft.model_name;
    lp.model_path = draft.model_path;
    lp.llama_cli_path = draft.llama_cli_path.empty() ? "llama-cli" : draft.llama_cli_path;
    providers->local_models[draft.provider_id] = lp;
    providers->external.erase(draft.provider_id);
    return;
  }

  ExternalProvider ep;
  ep.base_url = draft.base_url;
  ep.model = draft.model;
  ep.api_key = draft.api_key;
  ep.timeout_sec = parse_or_default(draft.timeout_sec, 30);
  ep.max_retries = parse_or_default(draft.max_retries, 2);
  providers->external[draft.provider_id] = ep;
  providers->local_models.erase(draft.provider_id);
}

std::string validate_provider_draft(const ProviderDraft& draft) {
  if (!is_valid_provider_id(draft.provider_id)) {
    return "provider id uses [A-Za-z0-9_-] only";
  }

  if (draft.type == ProviderType::Local) {
    if (draft.model_name.empty() && draft.model_path.empty()) {
      return "local provider requires model_name or model_path";
    }
    return "";
  }

  if (draft.base_url.empty() || draft.model.empty() || draft.api_key.empty()) {
    return "external provider requires base_url, model, api_key";
  }
  if (!is_non_negative_int(draft.timeout_sec)) {
    return "timeout_sec must be a non-negative integer";
  }
  if (!is_non_negative_int(draft.max_retries)) {
    return "max_retries must be a non-negative integer";
  }
  return "";
}

void run_provider_form(const AppPaths& paths, ProviderRegistry* providers) {
  ProviderDraft draft;
  int focus = 0;
  std::string error_message;

  while (true) {
    const std::vector<ProviderField> fields = active_provider_fields(draft);
    if (focus < 0) {
      focus = 0;
    }
    if (focus >= static_cast<int>(fields.size())) {
      focus = static_cast<int>(fields.size()) - 1;
    }

    render_provider_screen(draft, focus, error_message);
    const KeyEvent ev = read_key();
    check_ctrl_c(ev);

    if (ev.type == KeyType::Up || ev.type == KeyType::Down) {
      adjust_focus(&focus, static_cast<int>(fields.size()), ev.type);
      continue;
    }

    const ProviderField active = fields[focus];

    if ((ev.type == KeyType::Left || ev.type == KeyType::Right) && active == ProviderField::Type) {
      draft.type = (draft.type == ProviderType::Local) ? ProviderType::External : ProviderType::Local;
      focus = std::min(focus, static_cast<int>(active_provider_fields(draft).size()) - 1);
      error_message.clear();
      continue;
    }

    if (ev.type == KeyType::Enter && active == ProviderField::LocalDownload) {
      run_local_model_download_page(paths, &draft);
      error_message.clear();
      continue;
    }

    std::string* field_value = mutable_provider_field(&draft, active);
    if (field_value && ev.type == KeyType::Character) {
      if (active == ProviderField::Timeout || active == ProviderField::MaxRetries) {
        if (std::isdigit(static_cast<unsigned char>(ev.ch)) == 0) {
          continue;
        }
      }
      field_value->push_back(ev.ch);
      error_message.clear();
      continue;
    }

    if (field_value && ev.type == KeyType::Backspace) {
      if (!field_value->empty()) {
        field_value->pop_back();
      }
      error_message.clear();
      continue;
    }

    if (ev.type == KeyType::Enter) {
      if (focus == static_cast<int>(fields.size()) - 1) {
        error_message = validate_provider_draft(draft);
        if (error_message.empty()) {
          apply_provider_draft(draft, providers);
          return;
        }
      } else {
        ++focus;
      }
      continue;
    }
  }
}

struct DefaultChoice {
  std::string display;
  std::string key;
};

std::vector<DefaultChoice> build_default_choices(const ProviderRegistry& providers) {
  std::vector<DefaultChoice> out;
  out.push_back({"(empty)", ""});

  std::vector<std::string> local_keys;
  std::vector<std::string> external_keys;
  for (const auto& kv : providers.local_models) {
    local_keys.push_back(kv.first);
  }
  for (const auto& kv : providers.external) {
    external_keys.push_back(kv.first);
  }
  std::sort(local_keys.begin(), local_keys.end());
  std::sort(external_keys.begin(), external_keys.end());

  for (const std::string& key : local_keys) {
    out.push_back({"local." + key, key});
  }
  for (const std::string& key : external_keys) {
    out.push_back({"external." + key, key});
  }
  return out;
}

std::string run_default_provider_selector(const ProviderRegistry& providers, const std::string& current_key) {
  const std::vector<DefaultChoice> choices = build_default_choices(providers);
  int focus = 0;
  for (size_t i = 0; i < choices.size(); ++i) {
    if (choices[i].key == current_key) {
      focus = static_cast<int>(i);
      break;
    }
  }

  while (true) {
    clear_screen();
    std::cout << "git-gencommit configuration wizard\n";
    std::cout << "Step 4/4: default provider\n\n";
    std::cout << "default_provider (provider information must be added first)\n\n";

    for (size_t i = 0; i < choices.size(); ++i) {
      const bool active = focus == static_cast<int>(i);
      const std::string row = "  " + choices[i].display;
      std::cout << focus_row_full_width(row, active) << "\n";
    }

    std::cout << "\nGuide: \u2191\u2193 move  Enter select\n";
    const KeyEvent ev = read_key();
    check_ctrl_c(ev);

    if (ev.type == KeyType::Up || ev.type == KeyType::Down) {
      adjust_focus(&focus, static_cast<int>(choices.size()), ev.type);
      continue;
    }
    if (ev.type == KeyType::Enter) {
      return choices[focus].key;
    }
  }
}

}  // namespace

void run_config_wizard(const AppPaths& paths) {
  ensure_app_layout(paths);

  Config cfg;
  ProviderRegistry providers;
  if (app_config_files_exist(paths)) {
    cfg = load_config(paths);
    providers = load_providers(paths);
  }
  if (cfg.commit_template.empty()) {
    cfg.commit_template = "{type}: {summary}";
  }

  {
    ScopedRawMode raw_mode;
    run_global_form(&cfg);

    bool add_provider = run_yes_no_screen("Step 2/4: provider", "Add a provider now?", true);
    while (add_provider) {
      run_provider_form(paths, &providers);
      add_provider = run_yes_no_screen("Step 3/4: provider", "Add another provider?", false);
    }

    cfg.default_provider = run_default_provider_selector(providers, cfg.default_provider);
  }

  save_config(paths, cfg);
  save_providers(paths, providers);

  std::cout << "[git-gencommit] configuration saved\n";
  std::cout << "[git-gencommit] " << paths.config_toml << "\n";
  std::cout << "[git-gencommit] " << paths.providers_toml << "\n";
}

}  // namespace ggc
