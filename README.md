# git-gencommit

LLM을 사용해 Git 커밋 메시지를 자동 생성하고, 옵션에 따라 `add -> commit -> push`를 순차 실행하는 Git 확장 명령입니다.

- 실행 명령: `git gencommit`
- 실제 바이너리: `git-gencommit` (Git subcommand 규칙으로 자동 연결)
- 설정 위치: `~/.gitgencommit/`

## 핵심 동작

`git gencommit`은 옵션 순서와 무관하게 항상 아래 순서로 실행됩니다.

1. `-a` / `--auto-add`  -> `git add .`
2. `-c` / `--commit`    -> LLM으로 제목 생성 후 `git commit -m "..."`
3. `-p` / `--push`      -> `git push`

기본 규칙:
- **옵션이 하나도 없으면** `-a -c -p`를 자동 적용합니다.
- **옵션이 하나라도 있으면** 기본 자동 적용을 끄고, 지정한 옵션만 실행합니다.

예시:
- `git gencommit` -> `-a -c -p`
- `git gencommit -c` -> 커밋만 수행(스테이징된 변경 기준)
- `git gencommit -p -a` -> 입력 순서와 무관하게 내부 실행은 `-a` 후 `-p`

## 추가 옵션

- `--dry-run`: 실제 `git add/commit/push`를 실행하지 않고 계획된 작업만 출력
- `--print-message`: `--commit` 사용 시 생성된 커밋 제목 출력
- `-h`, `--help`: 도움말

## Getting Started

### 1) 빌드

요구사항:
- Git
- CMake 3.16+
- C++17 컴파일러
- libcurl 개발 라이브러리(시스템 기본 또는 패키지 설치)

```bash
cmake -S . -B build
cmake --build build -j
```

### 2) 설치 (Git 서브커맨드 등록)

#### macOS / Linux (Shell)

```bash
bash scripts/install-git-gencommit.sh
```

스크립트는 `~/bin/git-gencommit`으로 설치합니다.
`~/bin`이 PATH에 있어야 `git gencommit`으로 실행됩니다.

#### Windows (PowerShell)

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install-git-gencommit.ps1
```

스크립트는 `$HOME\bin\git-gencommit.exe`로 설치합니다.
`$HOME\bin`이 PATH에 있어야 `git gencommit`이 동작합니다.

### 3) 설정 마법사 실행

설치 스크립트는 가능한 경우 설치 직후 대화형 설정 마법사를 자동 실행합니다.

언제든 아래 명령으로 설정 마법사를 다시 실행할 수 있습니다.

```bash
git gencommit config
```

또는:

```bash
git gencommit --configure
```

처음 실행 시 `~/.gitgencommit/config.toml` 또는 `~/.gitgencommit/providers.toml`가 없으면,
일반 실행(`git gencommit`, `git gencommit -c` 등) 중에도 마법사가 자동으로 시작됩니다.

`git gencommit --help`는 Git의 help 라우팅으로 처리되어 로컬 man page를 찾으려다가
`No manual entry for git-gencommit`가 출력될 수 있습니다. CLI 도움말 확인은 `git gencommit -h`를 사용하세요.

생성 구조:

```text
~/.gitgencommit/
  config.toml
  providers.toml
  models/
  cache/
```

### 4) Provider 설정 파일

`~/.gitgencommit/config.toml`에서 기본 provider key를 지정합니다.

```toml
[app]
run_on_startup = false
unload_after_commit = true

[commit]
template = "{type}: {summary}"
default_provider = "openai_main"
auto_download_models = true
```

`~/.gitgencommit/providers.toml`에서 provider를 등록합니다.

#### A. 외부 LLM (OpenAI Compatible API)

```toml
[external.openai_main]
base_url = "https://api.openai.com/v1"
model = "gpt-4o-mini"
api_key = "sk-..."
timeout_sec = 30
max_retries = 2
```

#### B. 로컬 LLM (llama.cpp + GGUF)

```toml
[local.local_qwen]
model_name = "Qwen/Qwen2.5-Coder-7B-Instruct"
# model_path를 지정하면 해당 파일 우선 사용
# model_path = "/Users/<name>/.gitgencommit/models/qwen2.5-coder-7b.gguf"
llama_cli_path = "/absolute/path/to/llama-cli"
```

로컬 provider 동작:
- `model_path`가 있으면 해당 경로를 사용
- 없고 `model_name`이 있으면 `auto_download_models=true`일 때 Hugging Face에서 다운로드 시도
- 다운로드 URL은 HTTPS만 허용

## 사용법

### 기본 자동화

```bash
git gencommit
```

동작:
1. `git add .`
2. LLM으로 커밋 제목 생성 (영문 Conventional Commit 1줄, 최대 72자)
3. `git commit -m "<generated>"`
4. `git push`

### 단계별 실행

```bash
# add만
git gencommit -a

# commit만 (기존 staged 변경 기준)
git gencommit -c --print-message

# push만
git gencommit -p
```

### 안전 점검

```bash
# 실제 반영 없이 시뮬레이션
git gencommit -a -c -p --dry-run
```

## 커밋 메시지 생성 규칙

현재 구현은 LLM에 아래 정책을 전달합니다.
- English Conventional Commit title
- 단일 라인
- 본문 없음
- 최대 72자
- 허용 타입: `feat, fix, chore, docs, refactor, test, perf, ci, build, style, revert`

## 문제 해결

- `git: 'gencommit' is not a git command`
  - `git-gencommit` 바이너리가 PATH에 있는지 확인
  - `~/bin` (또는 `$HOME\bin`) PATH 등록 확인

- `no provider configured in providers.toml`
  - `providers.toml`에 `[external.<key>]` 또는 `[local.<key>]` 추가
  - `config.toml`의 `default_provider`가 실제 key와 일치하는지 확인

- `local llama.cpp inference failed`
  - `llama_cli_path`가 정확한 실행 파일인지 확인
  - 모델 파일 경로 및 권한 확인

- `llama-cli: command not found`
  - `llama_cli_path`를 올바른 경로로 설정하면 로컬 추론을 사용할 수 있습니다.
  - 경로가 없거나 실행 불가하면 내장 규칙 기반 제목 생성으로 자동 폴백됩니다.

- 외부 API 호출 실패
  - `base_url`, `api_key`, `model` 값 확인
  - 네트워크/방화벽 및 timeout 설정 점검

## 현재 구현 메모

- `config.toml`의 `template` 필드는 로드되지만, 현재 메시지 생성 로직에 직접 반영되지는 않습니다.
- `run_on_startup`, `unload_after_commit`은 수명주기 훅으로 연결되어 있으며 확장 가능한 구조입니다.
