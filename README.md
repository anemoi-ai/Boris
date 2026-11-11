# Boris

**A working AI agent, built in C from scratch. No frameworks. No SDK. Every line yours.**

[![Build](https://github.com/anemoi-ai/Boris/actions/workflows/ci.yml/badge.svg)](https://github.com/anemoi-ai/Boris/actions)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![Platforms](https://img.shields.io/badge/platforms-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey)

![demo](./assets/demo.gif)

Boris is a complete AI agent in about 16,500 lines of C. It talks to any OpenAI-compatible model server (Ollama, llama.cpp, LM Studio, vLLM, OpenAI), keeps a conversation, calls tools, manages its own context window and saves sessions to disk. It runs natively on Linux, macOS and Windows.

It is built around two ideas:

1. **Every AI agent framework runs the same loop underneath.** You don't need a framework to write one. You need to understand what the loop actually does.
2. **C makes that loop legible.** When every allocation and every error case is explicit, the architecture stops being magic.

Boris is meant to be read. The dependency list is `libcurl` and a vendored cJSON. There is no Python in the agent, no package manager, no hidden runtime. It compiles clean under `-Wall -Wextra -Werror` (and `/W4 /WX` on Windows) and is covered by ten test suites with over 1,000 assertions.

## Contents

- [Quick start](#quick-start)
- [Building](#building)
  - [Linux](#linux)
  - [macOS](#macos)
  - [Windows](#windows)
- [Using Boris](#using-boris)
- [Configuration](#configuration)
- [Built-in tools](#built-in-tools)
- [Safety](#safety)
- [Model compatibility](#model-compatibility)
- [How it works](#how-it-works)
- [Tests and evals](#tests-and-evals)
- [Further reading](#further-reading)

---

## Quick start

You need a model server. The quickest is [Ollama](https://ollama.com), which needs no API key:

```bash
ollama pull llama3.1:8b
```

Then build Boris and run the setup wizard.

**Linux / macOS**

```bash
git clone https://github.com/anemoi-ai/Boris
cd Boris
make

./boris init      # asks a few questions, writes ~/.boris/config.ini
./boris chat
```

**Windows** (PowerShell; see [Windows](#windows) for the prerequisites)

```powershell
git clone https://github.com/anemoi-ai/Boris
cd Boris
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release

build\Release\boris.exe init
build\Release\boris.exe chat
```

At the wizard's endpoint question, give `http://localhost:11434/v1/chat/completions` for Ollama. It then lists the models the server has and lets you pick one.

To use a hosted model instead, skip the wizard and set three variables:

```bash
BORIS_API_KEY=sk-... \
BORIS_MODEL_ENDPOINT=https://api.openai.com/v1 \
BORIS_MODEL_NAME=gpt-5.4-mini \
    ./boris chat
```

---

## Building

The only dependency on every platform is libcurl.

### Linux

```bash
sudo apt install build-essential libcurl4-openssl-dev pkg-config   # Debian / Ubuntu
sudo dnf install gcc make libcurl-devel pkgconf-pkg-config         # Fedora

make            # builds ./boris
make test       # runs every test suite; no model or API key needed
```

`make RELEASE=1` builds with optimisation. `make memcheck` runs the tests under Valgrind. `make clean` starts over.

### macOS

```bash
xcode-select --install
brew install curl pkg-config

make
make test
```

macOS has no equivalent of the script confinement used on Linux and Windows; see [Safety](#safety).

### Windows

Boris builds natively with Visual Studio and CMake. There is no WSL, MSYS or Cygwin involved, and the result is an ordinary `boris.exe`.

**What you need**

| | |
|---|---|
| Windows | 10 version 1903 or later, or 11; 64-bit |
| Compiler | Visual Studio 2022 or newer (Community is fine), or the Build Tools, with the **Desktop development with C++** workload |
| CMake | 3.20 or later. The one Visual Studio installs is enough if you use *Developer PowerShell for VS* |
| Git | [git-scm.com](https://git-scm.com/download/win) |
| vcpkg | to get libcurl; set up in step 1 |

**1. Install vcpkg and libcurl** (once)

```powershell
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg install curl:x64-windows
```

If you keep vcpkg somewhere else, use that path in the next step.

**2. Build**

```powershell
git clone https://github.com/anemoi-ai/Boris
cd Boris
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

This produces `build\Release\boris.exe`, with `libcurl.dll` and the zlib DLL it needs copied next to it. Keep them together.

**3. Test** (optional; no model or API key needed)

```powershell
ctest --test-dir build -C Release --output-on-failure
```

**4. Run**

```powershell
build\Release\boris.exe init
build\Release\boris.exe chat
```

To run it as plain `boris` from anywhere, add `build\Release` to your `PATH`, or copy the folder's contents somewhere that already is.

**A single file with no DLLs**

Use vcpkg's static triplet. The result is one self-contained `boris.exe` of about 1 MB that you can copy to another machine:

```powershell
C:\vcpkg\vcpkg install curl:x64-windows-static
cmake -B build-static -DVCPKG_TARGET_TRIPLET=x64-windows-static `
      -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build-static --config Release
```

CI builds this on every run and keeps it as the `boris-windows-x64` artifact.

**If something goes wrong**

| Symptom | Cause and fix |
|---|---|
| `cmake` is not recognised | Open *Developer PowerShell for VS* from the Start menu, or install CMake from [cmake.org](https://cmake.org/download/) |
| `Could NOT find CURL` | The `-DCMAKE_TOOLCHAIN_FILE` path is wrong, or `vcpkg install curl:x64-windows` has not been run |
| `libcurl.dll was not found` when starting | `boris.exe` was moved without its DLLs. Move them together, or build the single-file version |
| No colours or line editing in Git Bash | Git Bash's terminal is not a Windows console. Use Windows Terminal or PowerShell, or run `winpty boris chat` |
| Spinner shows `\|/-\` instead of dots | The classic console's fonts have no braille characters. Cosmetic; Windows Terminal shows the dots |
| Some tests say "skipped: cannot create symbolic links" | Creating symlinks needs Developer Mode (Settings → System → For developers). The tests that need only directory links still run |
| Accented characters in file names fail | Windows older than 10 version 1903 ignores the UTF-8 setting Boris relies on |

**What is different on Windows**

- Paths may use either slash. `~` and `~/.boris` are under `%USERPROFILE%`.
- The `run` tool also runs `.ps1`, `.bat` and `.cmd` scripts. `.sh` scripts work if Git for Windows is installed.
- `boris init` stores your API key encrypted for your Windows account rather than as plain text.
- MinGW is not supported; use MSVC or clang-cl.

[docs/windows.md](docs/windows.md) explains how the port is put together.

---

## Using Boris

| Command | What it does |
|---|---|
| `boris init` | Setup wizard: your name, the model endpoint, API key, model, personality |
| `boris chat` | Interactive conversation with line editing, history and tab completion |
| `boris exec "task"` | Run one task and print the answer; for scripts and pipelines |
| `boris status` | Show the active configuration, including what the model server reports |
| `boris help` | Every flag |

### chat

```bash
boris chat                                   # uses ~/.boris/config.ini
boris chat --sandbox ./project               # let it work in this directory
boris chat --tools read,search,list_dir      # read-only: no writing, no scripts
```

Replies appear as they are written. The last session is restored on start and saved on exit.

Inside the conversation:

| | |
|---|---|
| `/help` | List commands |
| `/status`, `/tokens` | Conversation size and context use |
| `/tools` | Which tools are enabled |
| `/save [file]`, `/load [file]` | Save or load a conversation |
| `/clear`, `/reset` | Start over (`/reset` also reloads the system prompt) |
| `/quit` or Ctrl+D | Save and exit |
| `\` at the end of a line | Continue on the next line |
| Tab | Complete a `/command` |
| Ctrl+C | While Boris is working: stop it. At the prompt: discard the line |

### exec

`boris exec` runs one task and exits, without the REPL or the saved session:

```bash
boris exec "Summarise notes.md in three bullet points"
echo "Fix the failing test in calc.py" | boris exec --json
```

The answer goes to stdout and problems to stderr. With `--json`, stdout is a single report:

```json
{"response":"...","finish_reason":"natural","iterations":3,
 "usage":{"prompt_tokens":5120,"completion_tokens":310},
 "tool_calls":[{"name":"edit","summary":"calc.py (12 -> 14 bytes)","is_error":false}],
 "error":null}
```

Exit status: 0 finished, 1 error, 2 stopped at `max_iterations`, 130 interrupted. `--session FILE` continues the conversation saved in `FILE` and saves it back, so a sequence of `exec` calls can hold one conversation.

Nobody is there to answer a confirmation prompt, so tools that need one are refused, with a message on stderr. To let `exec` run scripts, pass `--set behaviour.confirm_run=false`.

---

## Configuration

Settings are layered, each overriding the one before:

**compiled defaults → `~/.boris/config.ini` → `--config FILE` → `BORIS_*` environment variables → command-line flags**

[config/boris.ini.example](config/boris.ini.example) documents every key. The ones you are most likely to change:

| Key | Flag or variable | Default | |
|---|---|---|---|
| `model.endpoint` | `BORIS_MODEL_ENDPOINT` | none | The server's chat completions URL |
| `model.name` | `BORIS_MODEL_NAME` | none | |
| `model.api_key` | `BORIS_API_KEY` | none | Prefer the variable to the file |
| `model.context_window` | | 8192 | Must match what the server gives the model |
| `model.max_tokens` | | 2048 | Longest reply; keep well below the window |
| `model.stream` | `--stream` | on in `chat` | |
| `tools.enabled` | `--tools`, `BORIS_TOOLS` | none | Comma list, or `default`, `all`, `none` |
| `tools.sandbox_root` | `--sandbox`, `BORIS_SANDBOX_ROOT` | none | The directory tools may touch |
| `tools.run_confine` | | true | OS-level confinement of scripts; see [Safety](#safety) |
| `behaviour.confirm_run` | | true | Ask before running a script |
| `behaviour.confirm_writes` | | false | Ask before every write |
| `agent.max_iterations` | `--max-iterations` | 16 | Model calls per task |
| `logging.trace_file` | `--trace`, `BORIS_TRACE_FILE` | none | Record every request and response |

Any key can be set for one run with `--set KEY=VALUE`, for example `--set model.temperature=0.3`.

**Getting the context window right.** If `context_window` is larger than what the server really gives the model, the server silently drops the start of long prompts, which looks like the model forgetting its instructions. Boris warns when `max_tokens` is not well below `context_window`, and with Ollama it asks the server and warns when the two disagree. `boris status` shows what the server reports.

**Where things are kept**

| | |
|---|---|
| `~/.boris/config.ini` | Configuration |
| `~/.boris/system_prompt.txt` | The personality you gave it in `boris init` |
| `~/.boris/sandbox/` | The default sandbox |
| `~/.boris/sessions/` | Saved conversations |
| `~/.boris/memory/memory.json` | The `memory` tool's store |
| `~/.boris/history.txt` | Input history |

### Project instructions

Put standing instructions for a project in `BORIS.md` (or `AGENTS.md`, the name several agents share) at the root of the sandbox: coding conventions, how to run the tests, files to leave alone. Boris reads it at startup, appends it to the system prompt for every turn, and says so. `prompt.instructions` chooses the file: `auto` (the default), `none`, or a path. In `auto` mode the file must really be inside the sandbox; a link pointing elsewhere is ignored, so a planted link cannot pull a private file into the prompt. Files over 32 KB are cut.

### Streaming

`boris chat` shows replies as they arrive, including what the model says before it calls tools; reasoning in `<think>` blocks is hidden. Streamed text is shown plain. With `model.stream = false` each reply is rendered as Markdown once it is complete. `boris exec` prints only the final answer either way.

### Request traces

`--trace FILE` appends each model request and response to `FILE` as JSON lines. Headers are never recorded, so the API key never reaches the file. `python3 evals/trace_prefix.py FILE` reports whether each request extends the previous one, which is what prompt caching depends on.

---

## Built-in tools

| Tool | What it does | Key detail |
|---|---|---|
| `read` | Read files within the sandbox | `offset` + `length` for paging |
| `write` | Write files | Atomic: temp file + rename; overwrite / append / create_new |
| `edit` | Replace exact text in a file | `old_string` must be unique (or `replace_all`); atomic |
| `list_dir` | List a directory | Names, types, sizes; hidden files excluded by default |
| `search` | Find text or files recursively | Extended regex or literal, `glob` filter; skips hidden directories, binaries and links |
| `move` | Rename or move | Refuses to overwrite unless asked; moves a link, not its target |
| `delete` | Delete files or trees | Directories need `recursive`; never follows links; never the sandbox root |
| `memory` | Persistent key-value store | JSON on disk, file-locked |
| `run` | Execute a script in the sandbox | Asks first; confined by the OS; hard timeout; allowlisted environment |
| `plan` | The model's own checklist | Kept outside the history, so it survives truncation |
| `fetch` | Fetch a web page as text | **Off by default**; asks first; private and loopback addresses refused |

`default` enables everything except `fetch`; `all` includes it. Every result is capped at `tools.max_result_bytes` (132 KB by default) with a note saying how much was cut.

`run` picks the interpreter from the script's `#!` line or its extension: `.py .js .sh .rb .lua .pl .php`, and on Windows also `.ps1 .bat .cmd`. It never passes anything to a shell.

Adding a tool is one file and three lines elsewhere: see [docs/extending.md](docs/extending.md).

---

## Safety

A model that can write files and run code needs limits that do not depend on the model behaving. Boris has four.

**1. The path sandbox.** Every file tool resolves its path with `realpath()` and checks the result is inside `tools.sandbox_root` before touching anything, so `../`, absolute paths and links out of the sandbox are refused. Files that are opened are checked again by where they really are, not only by name. On Windows the check also refuses device names (`NUL`, `CON`), alternate data streams and the other spellings Win32 rewrites.

**2. Confirmation.** `behaviour.confirm_run` is on by default: `boris chat` shows you each script and asks before running it. `confirm_writes` and `confirm_fetch` do the same for writes and web requests.

**3. Confinement of scripts.** The sandbox decides which script may start, not what it then does: a script is ordinary code running as you. So `tools.run_confine`, on by default, hands the script to the operating system's own sandbox:

| | A script can read | A script can write |
|---|---|---|
| **Linux** (Landlock, kernel 5.13+) | The system, its interpreter, the sandbox | The sandbox and a private temp directory |
| **Windows** (low integrity) | Anything you can | The sandbox and a private temp directory |
| **macOS**, older Linux kernels | Anything you can | Anything you can |

On Linux the rest of your home directory, `~/.ssh` and `~/.boris/config.ini` included, is unreachable; `tools.run_read_paths` adds directories a script may read. Where there is no confinement Boris logs a warning, and `boris status` always shows which row applies. Network access is not restricted on any platform.

**4. Secrets.** Scripts get an allowlisted environment (`PATH`, `HOME`, locale and a few others), never `BORIS_API_KEY` or any other `BORIS_*` variable. Boris redacts its own API key from every tool result before the model sees it, never writes it to a trace, and on Windows stores it encrypted for your account.

What this does not cover: the confinement limits files, not the network, and on Windows and macOS a script can still read your files. If you point Boris at a directory you care about, keep `confirm_run` on.

---

## Model compatibility

Boris speaks standard OpenAI chat completions. Any server with the same wire format works unchanged:

- **Ollama**, **llama.cpp**, **LM Studio**, **vLLM** for local models
- **OpenAI**, **OpenRouter**, **Groq**, **Together**, **Fireworks**, and Anthropic-compatible proxies

Set `model.endpoint` and `model.name`, and an API key if the server wants one.

**Models without native tool calls.** Some models emit tool calls as JSON inside code fences or `<tool_call>` tags instead of using the API's `tool_calls` field. Boris detects both and handles them like native calls (`agent.text_tool_fallback`, on by default). It also reminds the model that the native channel is preferred; set `agent.native_tool_reminder = false` for models that can only emit text.

---

## How it works

### The loop

This is the core of every agent framework, with the abstractions removed:

```c
/* from src/agent.c */
while (iterations < cfg->max_iterations) {
    llm_complete_with_retry(conv, tools, num_tools, cfg);

    if (finish_reason == "stop")
        return FINISHED_NATURALLY;

    if (finish_reason == "tool_calls")
        dispatch_tool_calls();      /* append results, loop */

    if (finish_reason == "length")
        truncate_and_continue();    /* context overflow */
}
return FINISHED_MAX_ITERATIONS;
```

That's the shape. The rest of the codebase is what happens when you take it seriously: retry with exponential backoff, path sandboxing, atomic writes, per-request arena allocation, structured logging, a fallback for models without native tool calls, and a loop that handles every `finish_reason`.

### In-loop self-correction

The loop is instrumented. Boris watches the model's behaviour turn by turn and injects `<system-reminder>` blocks when something is going wrong:

| Situation | Reminder injected |
|---|---|
| 3 consecutive tool errors | Stop and rethink: re-read the errors, check assumptions |
| Repeated identical tool calls | The previous result already answered your question, or the call is wrong |
| 3 tool turns in a row with no progress | Check earlier results and take a different approach, or finish |
| 2 iterations left before `max_iterations` | Finish the current step and summarise what is done and left |
| Response cut off by `max_tokens` | Continue from where you left off (up to 3 continuations) |
| Context full, history truncated | Earlier results are no longer visible; re-fetch if needed |
| Model uses text tool calls instead of native | Prefer the native channel, it is more reliable |

The system prompt explains `<system-reminder>` blocks so the model treats them as system-level instructions, not user speech. This is the part framework documentation never covers.

### Source layout

```
src/
├── main.c            Entry, CLI dispatch
├── exec.c            Non-interactive mode: one task, answer or JSON report
├── agent.c           The ReAct loop, self-correction, retry
├── llm.c             Requests to the model, response parsing, streaming
├── conversation.c    Message history, serialisation, truncation
├── http_client.c     libcurl wrapper: POST, GET, cancellation, size cap
├── trace.c           JSONL record of every model request/response
├── tools.c           Registry, dispatch, confirmation, result cap
├── sandbox.c         Path validation via realpath()
├── fs.c              mkdir -p, atomic file replacement, ~/.boris paths
├── configuration.c   INI + env + flags, layered overrides
├── arena.c           Per-request bump allocator
├── repl.c            Interactive loop, slash commands, session restore
├── editor.c          Line editor with history and completion
├── init.c            Setup wizard
├── logger.c          Structured logging
├── markdown.c        Terminal renderer (ANSI or plain)
├── metrics.c         Token counting, timing
├── terminal.c        Colour, TTY detection
├── json.c            JSON helpers
├── platform_win32.c  Windows only: the POSIX calls Boris uses, over Win32
├── platform_regex.c  Windows only: fnmatch and extended regex for search
└── tools/            One file per tool: function, schema, definition
    ├── read.c  write.c  edit.c  list_dir.c  search.c  move.c  delete.c
    ├── memory.c  plan.c  fetch.c
    ├── run.c         Script execution via fork/exec with timeout
    ├── run_win32.c   The same on Windows: CreateProcess in a job object
    └── run_confine.c Landlock rules that confine a script to the sandbox
```

Dependency direction is strict: `main` → `repl/init` → `agent` → `llm/tools` → `http_client/conversation/sandbox` → `arena/logger`. No cycles. The code is written against POSIX; `include/platform.h` is the single seam where Windows differs.

### What you learn by reading it

**Memory management without a GC.** The arena allocator (`src/arena.c`, about 200 lines) hands out bump-allocated memory for a single request and resets in one call. Long-lived conversation messages are individually allocated and freed. The ownership model is visible end to end.

**Real HTTP in C.** `src/http_client.c` is libcurl with SSL verification, a response size cap and cancellation: Ctrl+C aborts an in-flight request through a progress callback. Retry with backoff lives one layer up in `src/agent.c`, driven by a `recoverable` flag that `src/llm.c` sets per error.

**JSON without magic.** The conversation serialiser in `src/conversation.c` builds and parses the OpenAI wire format by hand. You can read the protocol, line by line, in the code.

**Sandboxing as a real problem.** `src/sandbox.c` shows why naive prefix checking fails (links, `../`, absolute paths) and why resolving first is the right answer, including the subtle case of `..` after a directory that does not exist yet.

**Error propagation without exceptions.** Every function that can fail returns `enum boris_error`, and every caller checks it.

**Porting without `#ifdef` soup.** `src/platform_win32.c` implements the few dozen POSIX calls Boris uses, with the same names and contracts, so the rest of the code does not know which system it is on.

---

## Tests and evals

```bash
make test                                              # Linux, macOS
ctest --test-dir build -C Release --output-on-failure  # Windows
```

Ten suites, none of which needs a model or an API key:

| Suite | Covers |
|---|---|
| `test_arena` | Allocation, reset, out-of-memory |
| `test_conversation` | History, serialisation, token estimation |
| `test_sandbox` | Traversal, link attacks, Windows path tricks |
| `test_tools` | Every tool, sandbox escapes, script confinement, result cap |
| `test_configuration` | INI, environment, defaults, API key storage |
| `test_logger` | Levels, file output |
| `test_instructions` | `BORIS.md` / `AGENTS.md` loading, link refusal, limits |
| `test_markdown` | Headings, lists, inline styles, fences |
| `test_platform` | Regex, `fnmatch` and option parsing give the same answers on every system |
| `test_e2e` | The full agent loop, streaming, exec, tracing |

The end-to-end tests run the real agent loop against `tests/mock_http.c`, a link-time substitute for `src/http_client.c` that returns queued responses. Tool calls, self-correction, retry and context overflow are all exercised without a model, and the production binary carries no test code.

CI builds and tests on Linux and Windows on every push.

### Evals

The tests use a mock model; `evals/` measures Boris against a real one. Each of the 60 cases runs in a throwaway home directory with a secret canary, is graded on what actually happened (files, hidden tests, tools called, the request trace), and fails if anything outside the sandbox changed or the canary reached the model.

```bash
python3 evals/run_evals.py --endpoint http://localhost:11434 --model qwen3:8b --repeat 3
python3 evals/run_evals.py selftest     # checks the harness itself, no model needed
```

See [docs/evals.md](docs/evals.md).

---

## Further reading

| | |
|---|---|
| [docs/walkthrough.md](docs/walkthrough.md) | A long-form walk through the agent loop in `src/agent.c` |
| [docs/architecture.md](docs/architecture.md) | Data flow and what each component is responsible for |
| [docs/extending.md](docs/extending.md) | Adding a tool |
| [docs/evals.md](docs/evals.md) | The eval harness and its cases |
| [docs/windows.md](docs/windows.md) | How the Windows port works |
| [config/boris.ini.example](config/boris.ini.example) | Every configuration key |

## License

MIT. Read it, learn from it, build on it.
