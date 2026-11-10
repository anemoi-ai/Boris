# Boris

**A working AI agent, built in C from scratch. No frameworks. No SDK. Every line yours.**

[![Build](https://github.com/anemoi-ai/Boris/actions/workflows/ci.yml/badge.svg)](https://github.com/anemoi-ai/Boris/actions)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

![demo](./assets/demo.gif)

---

## What This Is

Boris is a complete AI agent in ~13,000 lines of C across 31 source files. It compiles clean under `-Wall -Wextra -Werror`, passes 800+ tests, and runs leak-free under Valgrind. It connects to any OpenAI-compatible API — OpenAI, Ollama, LM Studio, vLLM, llama.cpp — maintains conversation history, calls tools, manages its own context window, and persists sessions to disk.

It is built around two ideas:

1. **Every AI agent framework runs the same loop underneath.** You don't need a framework to write one. You need to understand what the loop actually does.
2. **C makes that loop legible.** When every allocation and every error case is explicit, the architecture stops being magic.

Boris is meant to be read. Every design decision is deliberate. Every abstraction is the minimum necessary. The dependency list is `libcurl` and a vendored cJSON. There is no Python, no package manager, no hidden runtime.

---

## The Loop

This is the entire core of every agent framework — LangChain, CrewAI, AutoGen, the OpenAI Assistants API — with the abstractions removed:

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

That's the shape. The rest of the codebase is what happens when you take it seriously: retry with exponential backoff, filesystem sandboxing via `realpath()`, atomic writes, per-request arena allocation, structured logging, a text-based tool fallback for models without native tool calls, and a hardened ReAct loop that handles every `finish_reason` case.

---

## In-Loop Self-Correction

The loop is instrumented. Boris watches the model's behaviour turn by turn and injects `<system-reminder>` blocks when something is going wrong:

| Situation | Reminder injected |
|---|---|
| 3 consecutive tool errors | Stop and rethink — re-read errors, check assumptions |
| Repeated identical tool calls | The previous result already answered your question, or the call is wrong |
| 3 tool turns in a row with no progress (only repeats or failures) | Check earlier results and take a different approach, or finish |
| 2 iterations left before `max_iterations` | Finish the current step and summarise what is done and left |
| Response cut off by `max_tokens` | Continue from where you left off (up to 3 continuations) |
| Context full, history truncated | Earlier results no longer visible — re-fetch via tools if needed |
| Model uses text tool calls instead of native | Prefer the native channel, it's more reliable |

The system prompt is extended automatically with an explanation of `<system-reminder>` blocks so the model treats them as system-level instructions, not user speech. This is the part framework documentation never covers.

---

## What You Learn by Reading This

**Memory management without a GC.** The arena allocator (`src/arena.c`, ~200 lines) hands out bump-allocated memory for a single request and resets in one call. Long-lived conversation messages are individually `malloc`'d and `free`'d. The ownership model is visible end-to-end.

**Real HTTP in C.** `src/http_client.c` is libcurl with SSL verification, a response size cap, and cancellation: Ctrl+C aborts an in-flight request through a progress callback. Retry with exponential backoff lives one layer up in `src/agent.c`, driven by a `recoverable` flag that `src/llm.c` sets per error (timeouts, 429 and 5xx retry; 4xx and bad configuration do not). `tests/mock_http.c` substitutes the same interface at link time, so the production binary carries no test infrastructure but the full agent loop is testable without an API key.

**JSON without magic.** cJSON is 1,400 lines. The conversation serialiser in `src/conversation.c` builds and parses the OpenAI wire format manually — you can read the protocol, line by line, in the code.

**Sandboxing as a real problem.** `src/sandbox.c` shows why naive prefix-checking fails (symlinks, `../`, absolute paths in arguments) and why `realpath()` before every tool call is the right answer, including the subtle case of `..` after a directory that does not exist yet. About 350 lines of careful code.

**Error propagation without exceptions.** Every function that can fail returns `enum boris_error`. Every caller checks it. There are no unchecked paths. This is what makes the agent actually reliable.

**Agentic self-correction.** `src/agent.c` shows how to monitor a ReAct loop in real time — tracking consecutive errors, repeated calls, turns that make no progress — and recover the model without terminating the session.

---

## Quick Start

### With Ollama (no API key)

```bash
# Install Ollama: https://ollama.com
ollama pull llama3.1:8b

git clone https://github.com/anemoi-ai/Boris
cd Boris
make

./boris init      # walks you through configuration
./boris chat
```

### With OpenAI

```bash
git clone https://github.com/anemoi-ai/Boris
cd Boris
make

BORIS_API_KEY=sk-... \
BORIS_MODEL_ENDPOINT=https://api.openai.com/v1 \
BORIS_MODEL_NAME=gpt-5.4-mini \
    ./boris chat
```

### Dependencies

```bash
# Ubuntu / Debian
sudo apt install libcurl4-openssl-dev

# macOS
brew install curl
```

That's the entire dependency list.

### On Windows

Boris builds natively with Visual Studio 2022 (MSVC or clang-cl) and CMake; no
WSL, MSYS or Cygwin. Windows 10 version 1903 or later.

```powershell
vcpkg install curl:x64-windows

git clone https://github.com/anemoi-ai/Boris
cd Boris
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure

build\Release\boris.exe init
build\Release\boris.exe chat
```

Everything works as on Linux, with the differences you would expect: paths may
use either slash, `~` is `%USERPROFILE%`, and the `run` tool also runs `.ps1`,
`.bat` and `.cmd` scripts. See [docs/windows.md](docs/windows.md) for how the
port is put together.

---

## Architecture

```
src/
├── main.c            Entry, CLI dispatch
├── exec.c            Non-interactive mode: one task, answer or JSON report
├── agent.c           The ReAct loop, self-correction, retry
├── llm.c             HTTP to the LLM API, response parsing, streaming
├── conversation.c    Message history, serialisation, truncation
├── http_client.c     libcurl wrapper: POST, GET (fetch), cancellation, size cap
├── trace.c           JSONL record of every model request/response (--trace)
├── tools.c           Registry, dispatch, confirmation, result cap
├── sandbox.c         Path validation via realpath()
├── fs.c              mkdir -p, atomic file replacement, ~/.boris paths
├── platform_win32.c  Windows only: the POSIX calls Boris uses, over Win32
├── platform_regex.c  Windows only: fnmatch and extended regex for search
├── configuration.c   INI + env + flags, layered overrides
├── arena.c           Per-request bump allocator
├── repl.c            Interactive loop, slash commands, session restore
├── editor.c          Line editor with history and completion
├── init.c            Setup wizard
├── logger.c          Structured logging
├── markdown.c        Terminal renderer (ANSI or plain)
├── metrics.c         Token counting, timing
├── terminal.c        Colour, TTY detection
├── json.c            JSON builder utilities
└── tools/            One file per tool: function, schema, definition
    ├── read.c        Paged file read
    ├── write.c       Atomic write (temp + rename)
    ├── edit.c        Exact-string replacement, atomic
    ├── list_dir.c    Directory listing
    ├── search.c      Recursive regex / name search
    ├── move.c        Rename or move within the sandbox
    ├── delete.c      Delete files and trees, never through symlinks
    ├── memory.c      Key-value store with file locking
    ├── run.c         Script execution via fork/exec with timeout
    ├── run_win32.c   The same on Windows: CreateProcess in a job object
    ├── plan.c        The model's own checklist
    └── fetch.c       HTTP(S) GET, HTML to text (off by default)
```

Dependency direction is strict: `main` → `repl/init` → `agent` → `llm/tools` → `http_client/conversation/sandbox` → `arena/logger`. No cycles.

---

## Built-in Tools

| Tool | What it does | Key detail |
|---|---|---|
| `read` | Read files within the sandbox | Supports `offset` + `length` for paging |
| `write` | Write files safely | Atomic: temp + `rename()`; overwrite / append / create_new |
| `edit` | Replace exact text in a file | `old_string` must be unique (or `replace_all`); atomic; keeps the file mode |
| `list_dir` | List directory contents | Names, types, sizes; hidden files excluded by default |
| `search` | Find text or files recursively | POSIX regex or literal, `glob` filter; skips hidden dirs, binaries, symlinks |
| `move` | Rename or move | Refuses to overwrite unless asked; moves a symlink, not its target |
| `delete` | Delete files or trees | Directories need `recursive`; never follows symlinks; never the sandbox root |
| `memory` | Persistent key-value store | JSON on disk, file-locked; get / set / delete / list |
| `run` | Execute a script in the sandbox | Shebang or extension; fork/exec, never `system()`; SIGKILL timeout; allowlisted environment |
| `plan` | The model's checklist | Kept outside the history, so it survives truncation |
| `fetch` | Fetch a web page as text | **Off by default**; asks first; http(s) only; private and loopback addresses refused |

`tools.enabled = default` turns on everything except `fetch`; `all` includes it. Every result is capped at `tools.max_result_bytes` (132 KB by default) with a note saying how much was cut.

Scripts started by `run` see only an allowlisted environment (`PATH`, `HOME`, locale and a few others), never `BORIS_API_KEY`. They still run with your permissions and can read files such as `~/.boris/config.ini`, so Boris also redacts its own API key from every tool result before the model sees it. `behaviour.confirm_run = true` is the safeguard if you need more.

---

## Tests

```bash
make test       # all 9 suites, no API key required
make memcheck   # all tests under Valgrind
```

```
--- test_arena ---         allocation, reset, OOM
--- test_conversation ---  history, serialisation, token estimation
--- test_sandbox ---       path traversal, symlink attacks
--- test_tools ---         every tool, sandbox escapes, result cap, registry
--- test_configuration --- INI, env vars, validation, defaults
--- test_logger ---        levels, file output, rotation
--- test_instructions ---  BORIS.md / AGENTS.md loading, symlink refusal, limits
--- test_markdown ---      headings, lists, inline stripping, fences
--- test_e2e ---           full agent loop, streaming, exec, tracing (mock HTTP)

Results: 9 passed, 0 failed
```

The end-to-end tests run without a real LLM. `tests/mock_http.c` provides a link-level substitute for `src/http_client.c` that returns pre-queued responses, so the complete agent loop — tool calls, self-correction nudges, retry, context overflow — can be exercised without an API key.

### Evals

`make test` uses a mock model; `evals/` measures Boris against a real one. Every trial runs in a throwaway `HOME` with a secret canary, is graded on what actually happened (files, hidden tests, tools called, the request trace), and fails if anything outside the sandbox changed or the canary reached the model. 60 cases cover tools, real project tasks, sessions and limits, long contexts, injected network faults and attacks; results come with confidence intervals, efficiency metrics and run-to-run comparison.

```bash
python3 evals/run_evals.py --endpoint http://localhost:11434 --model qwen3:8b --repeat 3
python3 evals/run_evals.py selftest     # checks the harness itself, no model needed
```

See [docs/evals.md](docs/evals.md).

---

## Compatibility

Boris speaks standard OpenAI chat completions. Any provider with the same wire format works unchanged:

- **Ollama** — local models, no API key
- **LM Studio** — local models with GUI
- **vLLM** — high-throughput serving
- **llama.cpp** — local GGUF models
- **OpenAI, Anthropic-compatible proxies, OpenRouter, Groq, Together, Fireworks** — and anything else that ships the standard format

Change `model_endpoint` in `~/.boris/config.ini` or set `BORIS_MODEL_ENDPOINT`. That's it.

**Text tool fallback.** Models that don't support native `tool_calls` often emit tool invocations as JSON inside markdown code fences or `<tool_call>` XML tags. Boris detects both and handles them identically to native tool calls (`agent.text_tool_fallback = true`, on by default). It also reminds the model that the native channel is preferred; set `agent.native_tool_reminder = false` for models that can only emit text tool calls.

---

## Configuration

Config layers, in priority order: compiled defaults → `~/.boris/config.ini` → environment variables → CLI flags.

Common knobs:

```bash
./boris chat --tools read,write,edit       # comma list, or 'default' / 'all' / 'none'
./boris chat --sandbox ./project           # restrict filesystem access
./boris chat --max-iterations 16           # ReAct loop budget
./boris chat --stream                      # show replies as they arrive
./boris chat --show-metrics                # token + timing summary on exit
./boris chat --set model.temperature=0.3   # override any config key
./boris chat --trace requests.jsonl        # record every model request/response
```

### Project instructions

Put standing instructions for a project in `BORIS.md` (or `AGENTS.md`, the name several agents share) at the root of the sandbox: coding conventions, how to run the tests, files to leave alone. Boris reads it at startup, appends it to the system prompt for every turn, and says so (`Using project instructions from BORIS.md`). `prompt.instructions` chooses the file: `auto` (the default), `none`, or a path anywhere. In `auto` mode the file must really be inside the sandbox; a symlink pointing elsewhere is ignored, so a link planted by a script cannot pull a private file into the prompt. Files over 32 KB are cut. `boris status` shows which file is in use.

The model can edit files in the sandbox, including `BORIS.md`, and a change takes effect in the next session; `behaviour.confirm_writes = true` makes every write ask first.

### Streaming

With `model.stream = true` (or `--stream`), `boris chat` shows replies as they arrive, including what the model says before it calls tools; reasoning in `<think>` blocks is hidden. Streamed text is shown plain; with streaming off, each reply is rendered as Markdown once it is complete. `boris exec` prints only the final answer either way.

### Non-interactive use

`boris exec` runs one task and exits, without the REPL or the saved session:

```bash
./boris exec "Summarise notes.md in three bullet points"
echo "Fix the failing test in calc.py" | ./boris exec --json
```

The answer goes to stdout and problems to stderr. With `--json` stdout is a single report:

```json
{"response":"...","finish_reason":"natural","iterations":3,
 "usage":{"prompt_tokens":5120,"completion_tokens":310},
 "tool_calls":[{"name":"edit","summary":"calc.py (12 -> 14 bytes)","is_error":false}],
 "error":null}
```

Exit status: 0 finished, 1 error, 2 stopped at `max_iterations`, 130 interrupted. `--session FILE` continues the conversation saved in `FILE` (if it exists) and saves it back, so a sequence of `exec` calls can hold one conversation. Tools that need confirmation are refused in `exec` (there is nobody to ask), with a message on stderr.

### Request traces

`--trace FILE` (or `logging.trace_file`, or `BORIS_TRACE_FILE`) appends each model request and response to `FILE` as JSON lines. Headers are never recorded, so the API key never reaches the file; the file is created with mode 0600 because it holds the whole conversation. `python3 evals/trace_prefix.py FILE` reports whether each request extends the previous one, which is what prompt caching depends on.

`./boris help` lists every flag and env var. Boris stores config in `~/.boris/config.ini`, sessions in `~/.boris/sessions/`, memory in `~/.boris/memory/memory.json`, and input history in `~/.boris/history.txt`.

REPL has `/help`, `/status`, `/tokens`, `/tools`, `/save`, `/load`, `/clear`, `/reset`, `/quit`. Type `\` at end of line to continue. Ctrl+C interrupts an agent run; Ctrl+C at the prompt saves and exits.

---

## License

MIT. Read it, learn from it, build on it.
