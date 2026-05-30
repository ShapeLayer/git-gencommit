#include "cli.hpp"
#include "config.hpp"
#include "config_wizard.hpp"
#include "git.hpp"
#include "hf.hpp"
#include "llm.hpp"

#include <exception>
#include <iostream>
#include <memory>
#include <string>

int main(int argc, char** argv) {
  try {
    const ggc::Options opt = ggc::parse_cli(argc, argv);
    if (opt.help) {
      ggc::print_help(argv[0]);
      return 0;
    }

    const ggc::AppPaths paths = ggc::resolve_app_paths();
    const bool has_config_files = ggc::app_config_files_exist(paths);

    if (opt.configure) {
      ggc::run_config_wizard(paths);
      return 0;
    }
    if (!opt.download_model.empty()) {
      ggc::ensure_app_layout(paths);
      const std::string out = ggc::hf_download_model(opt.download_model, paths);
      std::cout << "[git-gencommit] model downloaded: " << out << "\n";
      return 0;
    }
    if (!opt.remove_model.empty()) {
      ggc::ensure_app_layout(paths);
      const std::string out = ggc::hf_remove_model(opt.remove_model, paths);
      std::cout << "[git-gencommit] model removed: " << out << "\n";
      return 0;
    }
    if (!has_config_files) {
      ggc::run_config_wizard(paths);
    }

    std::unique_ptr<ggc::LlmEngine> llm;

    // Execution order is fixed regardless of input option order: -a, -c, -p.
    if (opt.auto_add) {
      if (opt.dry_run) {
        std::cout << "[git-gencommit] dry-run: git add .\n";
      } else {
        std::cout << "[git-gencommit] running: git add .\n";
        ggc::git_add_all();
      }
    }

    if (opt.commit) {
      ggc::ensure_app_layout(paths);
      const ggc::Config cfg = ggc::load_config(paths);
      const ggc::ProviderRegistry providers = ggc::load_providers(paths);
      llm = std::make_unique<ggc::LlmEngine>(cfg, providers, paths);
      llm->load_if_needed();

      std::cout << "[git-gencommit] generating commit message with LLM\n";
      ggc::StagedChangeContext context;
      if (opt.dry_run && opt.auto_add) {
        context = ggc::git_virtual_staged_context_all();
      } else {
        context = ggc::git_staged_context();
      }
      const std::string message = llm->generate_commit_message(context);
      if (opt.print_message || opt.dry_run) {
        std::cout << "[git-gencommit] generated title: " << message << "\n";
      }
      if (opt.dry_run) {
        std::cout << "[git-gencommit] dry-run: git commit -m \"<generated>\"\n";
      } else {
        std::cout << "[git-gencommit] running: git commit -m \"<generated>\"\n";
        ggc::git_commit(message);
      }
    }

    if (opt.push) {
      if (opt.dry_run) {
        std::cout << "[git-gencommit] dry-run: git push\n";
      } else {
        std::cout << "[git-gencommit] running: git push\n";
        ggc::git_push();
      }
    }

    if (llm) {
      llm->unload_if_needed();
    }
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "[git-gencommit] error: " << ex.what() << "\n";
    return 1;
  }
}
