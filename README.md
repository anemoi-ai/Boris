# Boris

**A working AI agent, built in C from scratch. No frameworks. No SDK. Every line yours.**

[![Build](https://github.com/anemoi-ai/Boris/actions/workflows/ci.yml/badge.svg)](https://github.com/anemoi-ai/Boris/actions)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

![demo](./assets/demo.gif)

---

## What This Is

Boris is a complete AI agent in ~10,000 lines of C across 21 source files. It compiles clean under `-Wall -Wextra -Werror`, passes 400+ tests, and runs leak-free under Valgrind. It connects to any OpenAI-compatible API — OpenAI, Ollama, LM Studio, vLLM, llama.cpp — maintains conversation history, calls tools, manages its own context window, and persists sessions to disk.

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
| 6+ tool turns with no summary | Pause and state what you have learned before continuing |
| Response cut off by `max_tokens` | Continue from where you left off (up to 3 continuations) |
| Context full, history truncated | Earlier results no longer visible — re-fetch via tools if needed |
| Model uses text tool calls instead of native | Prefer the native channel, it's more reliable |

The system prompt is extended automatically with an explanation of `<system-reminder>` blocks so the model treats them as system-level instructions, not user speech. This is the part framework documentation never covers.

---

## What You Learn by Reading This

**Memory management without a GC.** The arena allocator (`src/arena.c`, ~200 lines) hands out bump-allocated memory for a single request and resets in one call. Long-lived conversation messages are individually `malloc`'d and `free`'d. The ownership model is visible end-to-end.

**Real HTTP in C.** `src/http_client.c` is libcurl with retry, exponential backoff, streaming, SSL verification, and connection reuse. `tests/mock_http.c` substitutes the same interface at link time, so the production binary carries no test infrastructure but the full agent loop is testable without an API key.

**JSON without magic.** cJSON is 1,400 lines. The conversation serialiser in `src/conversation.c` builds and parses the OpenAI wire format manually — you can read the protocol, line by line, in the code.

**Sandboxing as a real problem.** `src/sandbox.c` shows why naive prefix-checking fails (symlinks, `../`, absolute paths in arguments) and why `realpath()` before every tool call is the right answer. 380 lines of careful code.

**Error propagation without exceptions.** Every function that can fail returns `enum boris_error`. Every caller checks it. There are no unchecked paths. This is what makes the agent actually reliable.

**Agentic self-correction.** `src/agent.c` shows how to monitor a ReAct loop in real time — tracking consecutive errors, repeated calls, context drift — and recover the model without terminating the session.

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

---

## Architecture

```
src/
├── main.c            Entry, CLI dispatch
├── agent.c           The ReAct loop, self-correction, retry
├── llm.c             HTTP to the LLM API, response parsing, streaming
├── conversation.c    Message history, serialisation, truncation
├── http_client.c     libcurl wrapper with retry/backoff
├── tools.c           Registry, dispatch, JSON schemas
├── sandbox.c         Path validation via realpath()
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
└── tools/
    ├── read.c        Paged file read
    ├── write.c       Atomic write (temp + rename)
    ├── list_dir.c    Directory listing
    ├── memory.c      Key-value store with file locking
    └── run.c         Script execution via fork/exec with timeout
```

Dependency direction is strict: `main` → `repl/init` → `agent` → `llm/tools` → `http_client/conversation/sandbox` → `arena/logger`. No cycles.

---

## Built-in Tools

| Tool | What it does | Key detail |
|---|---|---|
| `read` | Read files within the sandbox | Supports `offset` + `length` for paging |
| `write` | Write files safely | Atomic: temp + `rename()`; overwrite / append / create_new |
| `list_dir` | List directory contents | Names, types, sizes; hidden files excluded by default |
| `memory` | Persistent key-value store | JSON on disk, file-locked; get / set / delete / list |
| `run` | Execute a script in the sandbox | Detects interpreter from shebang or extension; fork/exec, never `system()`; SIGKILL timeout |

---

## Tests

```bash
make test       # all 8 suites, no API key required
make memcheck   # all tests under Valgrind
```

```
--- test_arena ---         allocation, reset, OOM
--- test_conversation ---  history, serialisation, token estimation
--- test_sandbox ---       path traversal, symlink attacks
--- test_tools ---         read, write, list_dir, memory, run, error paths
--- test_configuration --- INI, env vars, validation, defaults
--- test_logger ---        levels, file output, rotation
--- test_markdown ---      headings, lists, inline stripping, fences
--- test_e2e ---           full agent loop with mock HTTP

Results: 8 passed, 0 failed
```

The end-to-end tests run without a real LLM. `tests/mock_http.c` provides a link-level substitute for `src/http_client.c` that returns pre-queued responses, so the complete agent loop — tool calls, self-correction nudges, retry, context overflow — can be exercised without an API key.

---

## Compatibility

Boris speaks standard OpenAI chat completions. Any provider with the same wire format works unchanged:

- **Ollama** — local models, no API key
- **LM Studio** — local models with GUI
- **vLLM** — high-throughput serving
- **llama.cpp** — local GGUF models
- **OpenAI, Anthropic-compatible proxies, OpenRouter, Groq, Together, Fireworks** — and anything else that ships the standard format

Change `model_endpoint` in `~/.boris/config.ini` or set `BORIS_MODEL_ENDPOINT`. That's it.

**Text tool fallback.** Models that don't support native `tool_calls` often emit tool invocations as JSON inside markdown code fences or `<tool_call>` XML tags. Boris detects both and handles them identically to native tool calls (`agent.text_tool_fallback = true`, on by default).

---

## Configuration

Config layers, in priority order: compiled defaults → `~/.boris/config.ini` → environment variables → CLI flags.

Common knobs:

```bash
./boris chat --tools read,write,list_dir   # comma list, or 'all' / 'none'
./boris chat --sandbox ./project           # restrict filesystem access
./boris chat --max-iterations 16           # ReAct loop budget
./boris chat --stream                      # streaming responses
./boris chat --show-metrics                # token + timing summary on exit
./boris chat --set model.temperature=0.3   # override any config key
```

`./boris help` lists every flag and env var. Boris stores config in `~/.boris/config.ini`, sessions in `~/.boris/sessions/`, memory in `~/.boris/memory/memory.json`, and input history in `~/.boris/history.txt`.

REPL has `/help`, `/status`, `/tokens`, `/tools`, `/save`, `/load`, `/clear`, `/reset`, `/quit`. Type `\` at end of line to continue. Ctrl+C interrupts an agent run; Ctrl+C at the prompt saves and exits.

---

## License

MIT. Read it, learn from it, build on it.
