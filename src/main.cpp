#include "cli.hpp"
#include "config.hpp"
#include "git.hpp"
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
      const ggc::AppPaths paths = ggc::resolve_app_paths();
      ggc::ensure_app_layout(paths);
      const ggc::Config cfg = ggc::load_config(paths);
      const ggc::ProviderRegistry providers = ggc::load_providers(paths);
      llm = std::make_unique<ggc::LlmEngine>(cfg, providers, paths);
      llm->load_if_needed();

      std::cout << "[git-gencommit] generating commit message with LLM\n";
      std::string diff;
      if (opt.dry_run && opt.auto_add) {
        diff = ggc::git_virtual_staged_diff_all();
      } else {
        diff = ggc::git_staged_diff();
      }
      const std::string message = llm->generate_commit_message(diff);
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
