# git-gencommit

When a Git repository is used not as a source-code version control tool but as a personal synchronization tool—more like a network drive manager—commits often become nothing more than synchronization timestamps or a means to push, without any real meaning.

In such cases, changes that should originally be split into multiple commits get bundled into a single commit, or commit messages fail to describe the changes and are filled with meaningless strings like "update", "sync", even "asdf" or "_". These kinds of commit messages usually arise because writing commit messages is a hassle and interrupts the flow of work: it forces the author to switch context in their head to summarize the changes.

This Git extension automates staging files, creating a commit, and pushing to the remote repository with a single command, reducing the burden of writing commit messages and helping commit messages focus more on describing the changes. It uses a large language model (LLM) to automatically generate commit messages so they accurately reflect the changes. It also lets you define templates to keep commit message formatting consistent.

## Getting Started

Requirements:

- Git
- CMake 3.16+
- C++17 compatible compiler

```bash
bash scripts/install-git-gencommit.sh
```

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\Install-Git-Gencommit.ps1
```

## Configuration

```bash
git gencommit config
# or
git gencommit --configure
```

### `~/.gitgencommit/config.toml`

```toml
[app]
run_on_startup = false
unload_after_commit = true

[commit]
template = "{type}: {summary}"
default_provider = "openai_main"
auto_download_models = true
```

### `~/.gitgencommit/providers.toml`

OpenAI Compatible API:

```toml
[external.openai_main]
base_url = "https://api.openai.com/v1"
model = "gpt-4o-mini"
api_key = "sk-..."
timeout_sec = 30
max_retries = 2
```

Local:

```toml
[local.local_qwen]
model_name = "Qwen/Qwen2.5-Coder-7B-Instruct"
# or
# model_path = "/Users/<name>/.gitgencommit/models/qwen2.5-coder-7b.gguf"
llama_cli_path = "/absolute/path/to/llama-cli"
```

## Usage

```bash
git gencommit
```

or

```bash
# add
git gencommit -a

# commit
git gencommit -c --print-message

# push
git gencommit -p
```

### Usecase

```bash
# simulation only
git gencommit -a -c -p --dry-run
```
