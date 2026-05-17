# Boris

![boris hero](./assets/boris-hero.svg)

**A working AI agent, built in C from scratch. No frameworks. No SDK. Every line yours.**

[![Build](https://github.com/anemoi-ai/Boris/actions/workflows/ci.yml/badge.svg)](https://github.com/anemoi-ai/Boris/actions)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

![demo](./assets/demo.gif)

---

## What This Is

Boris is a complete AI agent implementation in C. It connects to any OpenAI-compatible API (including local models via Ollama), maintains conversation history, calls tools, manages its own context window, and persists sessions to disk.

The codebase is ~10,000 lines across 21 source files. It compiles clean under `-Wall -Wextra -Werror`. It passes 400+ tests. Valgrind reports zero leaks.

**This is not a toy.** It implements everything a production agent needs: retry with exponential backoff, filesystem sandboxing, structured logging, atomic file writes, per-request arena allocation, a text-based tool fallback for models without native tool call support, and a hardened ReAct loop that handles all `finish_reason` cases including context overflow — plus in-loop self-correction nudges that steer the model when it gets stuck.

**This is also a teaching project.** Every design decision is deliberate. Every abstraction is the minimum necessary. The code is meant to be read.

---

## The Core Loop

![boris react loop](./assets/boris-react-loop.svg)

Every AI agent framework — LangChain, CrewAI, AutoGen, the OpenAI Assistants API — runs this loop underneath. Here it is without the abstraction:

```c
/* from src/agent.c */
while (iterations < cfg->max_iterations) {
    llm_complete_with_retry(conv, tools, num_tools, cfg);

    if (finish_reason == "stop")
        return FINISHED_NATURALLY;

    if (finish_reason == "tool_calls")
        dispatch_tool_calls();      /* append results, loop */

    if (finish_reason == "length")
        truncate_and_continue();    /* context overflow — real agents handle this */
}
return FINISHED_MAX_ITERATIONS;
```

Building this yourself — in a language that makes every allocation and every error case explicit — teaches you things about AI agent architecture that framework documentation never will.

### In-loop self-correction

The agent loop goes beyond the basic ReAct pattern. It monitors the model's behaviour turn by turn and injects `<system-reminder>` nudges when it detects problems:

| Situation | Reminder injected |
|-----------|-------------------|
| 3 consecutive tool errors | Stop and rethink — re-read errors, check assumptions |
| Repeated identical tool calls | The previous result already answered your question, or the call is wrong |
| 6+ tool turns with no summary | Pause and state what you have learned before continuing |
| Response cut off by `max_tokens` | Continue from where you left off (up to 3 continuations) |
| Context window full, history truncated | Earlier results no longer visible — re-fetch via tools if needed |
| Model uses text tool calls instead of native | Prefer the native channel — it is more reliable |

The system prompt is automatically extended with an explanation of `<system-reminder>` blocks so the model treats them as system-level instructions, not user speech.

---

## Quick Start

### With Ollama (no API key needed)

```bash
# Install Ollama: https://ollama.com
ollama pull llama3.1:8b

git clone https://github.com/anemoi-ai/Boris
cd boris
make

./boris init      # walks you through configuration
./boris chat
```

### With OpenAI

```bash
git clone https://github.com/anemoi-ai/Boris
cd boris
make

BORIS_API_KEY=sk-... BORIS_MODEL_ENDPOINT=https://api.openai.com/v1 \
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

That's the entire dependency list. libcurl and cJSON (vendored). No Python. No package manager.

---

## What Boris Can Do

### REPL commands

```
/help                  list all commands
/status                model, endpoint, message count, token usage, context %
/tokens                estimate current token usage
/tools                 list registered tools with enabled/disabled status
/save [path]           save conversation to disk
/load [path]           load a previous conversation
/clear                 clear history, keep system prompt (asks for confirmation)
/reset                 clear history, reload system prompt from config
/quit  (/exit)         auto-save and exit
```

Type `\` at the end of a line to continue your message on the next line. Tab-completes slash commands; typos get "did you mean?" suggestions.

Ctrl+C during an agent run interrupts it and returns to the prompt. Ctrl+C at the prompt saves the session and exits.

### CLI flags

```bash
boris chat --config FILE         load config from a specific file
boris chat --tools LIST          comma-separated tools (read,write,list_dir,memory,run,all,none)
boris chat --sandbox DIR         set sandbox root
boris chat --max-iterations N    ReAct loop iteration budget (default: 16)
boris chat --max-retries N       LLM retry count on transient errors (default: 2)
boris chat --stream              enable streaming responses
boris chat --log-level LEVEL     debug / info / warn / error
boris chat --log-file FILE       write logs to file
boris chat --show-metrics        print token and timing summary on exit
boris chat --insecure            skip SSL certificate verification
boris chat --set KEY=VALUE       override any config key (e.g. model.temperature=0.3)
```

### Environment variables

```bash
BORIS_API_KEY              API key
BORIS_MODEL_ENDPOINT       model endpoint URL
BORIS_MODEL_NAME           model name
BORIS_SANDBOX_ROOT         sandbox root directory
BORIS_TOOLS                enabled tools (comma-separated)
BORIS_MAX_ITERATIONS       ReAct loop budget
BORIS_MAX_RETRIES          retry count
BORIS_LOG_LEVEL            log level
BORIS_LOG_FILE             log file path
BORIS_MEMORY_PERSIST       persist memory store (true/false)
BORIS_SHOW_METRICS         print metrics at exit
BORIS_MODEL_VERIFY_SSL     SSL verification (true/false)
```

Config is loaded in priority order: compiled defaults → `~/.boris/config.ini` → environment variables → CLI flags.

### Built-in tools

| Tool | What it does | Key implementation detail |
|------|-------------|--------------------------|
| `read` | Read files within the sandbox | Supports `offset` + `length` for paging large files |
| `write` | Write files safely | Atomic: write to temp, then `rename()`; supports overwrite / append / create_new modes |
| `list_dir` | List directory contents | Shows names, types, sizes; hidden files excluded by default |
| `memory` | Persistent key-value store | JSON on disk, file-locked; actions: get / set / delete / list |
| `run` | Execute a script within the sandbox | Detects interpreter from shebang or extension (.py .js .sh .rb .lua .pl .php); fork/exec, never `system()`; hard SIGKILL timeout |

---

## Architecture

```
src/
├── main.c            Entry point, CLI dispatch (chat / init / status / help)
├── agent.c           The ReAct loop — tool dispatch, self-correction nudges, retry
├── llm.c             HTTP to the LLM API, response parsing, streaming
├── conversation.c    Message history: dynamic array, JSON serialisation, truncation
├── http_client.c     libcurl wrapper with retry/backoff, SSL, connection reuse
├── tools.c           Tool registry, bitmask dispatch, JSON schema definitions
├── sandbox.c         Path validation — realpath() before every tool call
├── configuration.c   INI file parsing, env vars, layered override system
├── arena.c           Per-request bump allocator, ~200 lines
├── repl.c            Interactive loop, slash commands, spinner, session restore
├── init.c            Setup wizard (boris init)
├── editor.c          Line editor: cursor movement, history, tab completion
├── logger.c          Structured logging to file or stderr
├── markdown.c        Terminal markdown renderer — ANSI formatting or plain text
├── metrics.c         Token counting, timing, retry tracking
├── terminal.c        Colour output, TTY detection
├── json.c            JSON builder utilities
└── tools/
    ├── read.c        File read with offset/length paging
    ├── write.c       Atomic file write, append, create_new
    ├── list_dir.c    Directory listing with type/size
    ├── memory.c      Key-value persistence with file locking
    └── run.c         Script execution via fork/exec with timeout
```

The dependency direction is strict: `main` → `repl/init` → `agent` → `llm/tools` → `http_client/conversation/sandbox` → `arena/logger`. No cycles.

---

## Tests

```bash
make test
```

```
--- test_arena ---         arena allocation, reset, OOM handling
--- test_conversation ---  message history, serialisation, token estimation
--- test_sandbox ---       path traversal attempts, symlink attacks
--- test_tools ---         read, write, list_dir, memory, run — all error paths
--- test_configuration --- INI parsing, env vars, validation, defaults
--- test_logger ---        log levels, file output, rotation
--- test_markdown ---      heading detection, list markers, inline stripping, fences
--- test_e2e ---           full agent loop with mock HTTP — no real model needed

Results: 8 passed, 0 failed
```

The end-to-end tests run without a real LLM. `tests/mock_http.c` provides a complete replacement for `src/http_client.c` — it implements the same `http_post()` interface but returns pre-queued responses instead of making real requests. The test binary links `mock_http.c` in place of `http_client.o`, so the production binary contains no test infrastructure at all. You can test the complete agent loop — including tool call sequences and self-correction nudge injection — without an API key.

```bash
make memcheck    # run all tests under Valgrind
```

---

## What You Learn by Reading This

**Memory management without a GC.** The arena allocator (`src/arena.c`) allocates from a fixed region and resets in one call. Every message in the conversation history is individually `malloc`'d and `free`'d. Reading the ownership model teaches you more about memory than any tutorial.

**Real HTTP in C.** `src/http_client.c` implements retry with exponential backoff, streaming support, SSL verification, and connection reuse. Not a wrapper — the actual libcurl options, with comments explaining each one. `tests/mock_http.c` shows how to test it: a clean linker-level substitution that implements the same interface with pre-queued responses, so the production binary carries no test code.

**JSON without magic.** cJSON is 1,400 lines of C. The conversation serialiser in `src/conversation.c` builds and parses JSON manually. You'll see exactly how the OpenAI wire format works.

**Sandboxing as a real problem.** `src/sandbox.c` shows why naive prefix-checking doesn't work (symlinks, `../` sequences, absolute paths in arguments). `realpath()` resolves before checking. It's 380 lines of careful code.

**Error propagation without exceptions.** Every function that can fail returns `enum boris_error`. Every caller checks it. There are no unchecked error paths. This discipline is what makes the code actually reliable.

**Agentic self-correction.** `src/agent.c` shows how to instrument a ReAct loop — tracking consecutive errors, repeated calls, and context drift — and inject nudges that recover the model without terminating the session. This is the part framework documentation never covers.

---

## Compatible With Any OpenAI-Format API

Boris sends standard OpenAI chat completions requests. Any provider that speaks the same wire format works unchanged:

- **Ollama** — local models, no API key
- **LM Studio** — local models with GUI
- **vLLM** — high-throughput local serving
- **Anthropic** — via the OpenAI compatibility endpoint
- **Mistral, Together AI, Groq** — any hosted provider

Change `model_endpoint` in `~/.boris/config.ini` or set `BORIS_MODEL_ENDPOINT`. That's it.

### Text tool fallback

Models that don't support native `tool_calls` (many local models) often emit tool invocations as JSON inside markdown code fences or `<tool_call>` XML tags. Boris detects both formats and handles them identically to native tool calls. Enable with `agent.text_tool_fallback = true` in your config (on by default).

---

## Building and Running

```bash
make              # builds ./boris
make test         # runs all 8 test suites
make memcheck     # runs tests under Valgrind
make clean        # removes build/

./boris help      # see all subcommands
./boris init      # first-time setup wizard
./boris chat      # start chatting
./boris status    # show active configuration
```

Boris stores config in `~/.boris/config.ini`, sessions in `~/.boris/sessions/`, memory in `~/.boris/memory.json`, and input history in `~/.boris/history.txt`.

---

## License

MIT. Read it, learn from it, build on it.

---

*The ReAct loop is not a framework. It's a while loop and a function pointer table. Build it yourself and you'll never be confused about what your agent is doing again.*
