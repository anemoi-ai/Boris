# Boris — Architecture

This document describes the data flow through Boris and the responsibilities of each component. For file locations and line counts, see the [README](../README.md).

---

## Data Flow

```
User input (stdin)
    │
    ▼
repl_run()                          repl.c
    │  reads a line via the line editor,
    │  handles /slash commands,
    │  calls conversation_add_user()
    │
    ▼
agent_run_conv()                    agent.c
    │  validates conversation state,
    │  calls run_loop()
    │
    ▼
┌─────────────────────────────────────────────────────┐
│  ReAct loop  (run_loop)           agent.c            │
│                                                      │
│      llm_complete_with_retry()                       │
│            │                                         │
│            ▼                                         │
│      llm_complete()               llm.c             │
│            │  serialises conversation_history        │
│            │  + tool schemas to JSON                 │
│            │                                         │
│            ▼                                         │
│      http_post()                  http_client.c     │
│            │                                         │
│     ───────┴──────────────────────────────           │
│     HTTP POST /v1/chat/completions                   │
│     ───────────────────────────────                  │
│            │                                         │
│            ▼                                         │
│      (response JSON)              llm.c             │
│            │  parsed into struct llm_response        │
│            │                                         │
│     finish_reason == "stop"?                         │
│            │  yes → return response to REPL          │
│            │                                         │
│     finish_reason == "tool_calls"?                   │
│            │                                         │
│            ▼                                         │
│      tools_dispatch()             tools.c           │
│            │  looks up tool by name,                 │
│            │  checks tools_enabled bitmask           │
│            │                                         │
│            ▼                                         │
│      sandbox_resolve()            sandbox.c         │
│            │  calls realpath() on the path arg,      │
│            │  verifies it is inside sandbox_root     │
│            │                                         │
│            ▼                                         │
│      tool_fn()          tools/read.c, write.c, ...  │
│            │  executes the tool,                     │
│            │  returns struct tool_result             │
│            │                                         │
│            ▼                                         │
│      conversation_add_tool_result()  conversation.c │
│            │  appends tool output to history         │
│            │                                         │
│            └──────────── next iteration ────────────▶│
│                                                      │
│     finish_reason == "length"?                       │
│            │  yes → conversation_truncate(),         │
│            │         inject reminder, loop           │
└─────────────────────────────────────────────────────┘
    │
    ▼
repl_run()                          repl.c
    │  prints Boris's response,
    │  saves history to disk,
    │  loops back for next user input
```

---

## Components

### `repl.c` — Interactive loop

Owns the terminal interaction. Reads input using the line editor (`editor.c`), dispatches `/help`, `/status`, `/save`, and other slash commands, and calls `agent_run_conv()` for normal messages. Shows the animated spinner during model calls. Saves and restores session state from `~/.boris/sessions/`.

### `agent.c` — ReAct loop

The core of the agent. Runs the Reason → Act → Observe loop, handling all `finish_reason` cases:

| `finish_reason` | Action |
|----------------|--------|
| `stop` | Return response to REPL |
| `tool_calls` | Dispatch tools, append results, loop |
| `length` | Truncate history or inject continuation prompt |
| other | Log warning, return if content present |

Also manages in-loop self-correction: injects `<system-reminder>` nudges when it detects consecutive tool errors, repeated identical calls, prolonged tool-only turns without a summary, or text-embedded tool calls from models without native tool support.

### `llm.c` — LLM API client

Serialises the full conversation history and tool schemas into the OpenAI chat completions JSON format. Calls `http_post()`. Parses the response into `struct llm_response`, handling both native `tool_calls` responses and plain text content. Extracts token usage from the response for metrics.

### `http_client.c` — HTTP transport

Thin libcurl wrapper. Sends a single POST request and returns the response body as a heap-allocated string. Handles SSL verification, connection reuse, and request timeout. Has no retry logic — retries are the caller's (`agent.c`) responsibility.

### `tools.c` — Tool registry

Maintains a static array of `tool_definition` structs registered at startup. `tools_dispatch()` looks up a tool by name, checks the `tools_enabled` bitmask, allocates a scratch arena, and calls the tool's function pointer. Returns a `tool_result` with an error message if the tool is unknown or disabled.

### `sandbox.c` — Path validation

Called by every tool that accesses the filesystem before any `open()`, `stat()`, or `exec`. Uses `realpath()` to resolve symlinks and `..` sequences, then checks that the resolved path has `sandbox_root` as a prefix. Naive string prefix checks are not used — `realpath()` is called first.

### `conversation.c` — Message history

A dynamic array of `conversation_message` structs. Serialises to and from JSON for session persistence. `conversation_truncate()` removes the oldest user/assistant pairs while always preserving the system prompt. `conversation_estimate_tokens()` uses a character-count heuristic (chars/4 + 4 per message).

### `configuration.c` — Layered config

Applies settings in priority order: compiled defaults → INI file → environment variables → CLI flags (applied by `main.c`). `configuration_set_key()` is the single dispatch point for all settings; the INI parser and the env-var reader both call it.

### `arena.c` — Per-request scratch allocator

A fixed-region bump allocator created at the start of each tool dispatch and destroyed when it returns. Lets tool functions allocate temporary strings without managing individual `free()` calls. Falls back to `malloc()` for allocations that exceed the arena block size.

### `logger.c` — Structured logging

Writes timestamped log lines to a file or stderr. Log level (debug / info / warn / error) is set per session. The agent loop logs each iteration, tool call, and retry at `debug` level; errors and warnings at higher levels.

---

## Key Data Structures

```c
/* The conversation passed to every agent call */
struct conversation_history {
    struct conversation_message *messages;
    size_t count;
    size_t capacity;
    size_t total_characters;
};

/* One message in the conversation */
struct conversation_message {
    enum message_role role;      /* system, user, assistant, tool */
    char *content;               /* heap-allocated */
    char *tool_name;             /* set for role=tool */
    char *tool_call_id;          /* set for role=tool */
    /* assistant messages with tool calls also store the call list */
};

/* A request to call a tool */
struct tool_call {
    char  id[64];
    char  name[64];
    char *argument_json;         /* heap-allocated JSON string */
};

/* What a tool returns */
struct tool_result {
    char *content;               /* heap-allocated, must be freed */
    bool  is_error;
    char  tool_name[64];
    char  tool_call_id[64];
};

/* What agent_run() returns */
struct agent_result {
    enum finish_reason finish_reason;
    char *final_response;        /* heap-allocated, must be freed */
    int   turns_used;
    int   total_prompt_tokens;
    int   total_completion_tokens;
    enum boris_error error_code;
};
```

---

## Memory Ownership

- `conversation_history` owns all its messages. Free with `conversation_destroy()`.
- `llm_response` is heap-allocated by `llm_complete()`. Free with `llm_response_free()`.
- `tool_result.content` is heap-allocated. Free with `tool_result_free()`.
- `agent_result.final_response` is heap-allocated. Free with `agent_result_free()`.
- `agent_configuration` owns several heap strings. Free with `configuration_destroy()`.
- `memory_arena` is created per-dispatch and destroyed in the same scope.

---

## Dependency Order

```
main
 └── repl, init
      └── agent
           ├── llm
           │    └── http_client
           ├── tools
           │    └── sandbox
           └── conversation
                └── arena, logger
```

No cycles. Higher layers never `#include` headers from lower layers going upward.
