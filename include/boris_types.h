/*
 * boris_types.h - Core type definitions shared across the codebase.
 *
 * Defines the structs and enums used by the agent loop, conversation
 * manager, tool registry, LLM client, and configuration system.
 * All string fields in heap-allocated structs are owned by that struct
 * and must be freed by the corresponding _destroy() or _free() function.
 */

#ifndef BORIS_TYPES_H
#define BORIS_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include "boris_errors.h"

/* Role of a message within a conversation history. */
enum message_role {
	MESSAGE_ROLE_SYSTEM,
	MESSAGE_ROLE_USER,
	MESSAGE_ROLE_ASSISTANT,
	MESSAGE_ROLE_TOOL,
};

/* Reason the agent loop terminated. */
enum finish_reason {
	FINISHED_NATURALLY,
	FINISHED_MAX_ITERATIONS,
	FINISHED_ERROR,
	FINISHED_INTERRUPTED,
};

/*
 * Runtime configuration for the agent.
 *
 * Populated by configuration_set_defaults(), configuration_load_from_file(),
 * and configuration_apply_environment(). All heap string fields are freed
 * by configuration_destroy().
 */
struct agent_configuration {
	char *model_endpoint;
	char *model_name;
	char *api_key;

	float temperature;
	int max_tokens;
	int context_window;
	int request_timeout_seconds;

	char *system_prompt;
	char *system_prompt_file;

	unsigned int tools_enabled; /* Bitmask of enabled tools (TOOL_* constants) */
	char *sandbox_root;
	int run_timeout_seconds;
	char *run_env; /* extra environment variable names passed to run scripts */
	int max_result_bytes; /* cap on any tool result; <= 0 means the default */

	bool verify_ssl;

	int max_iterations;
	bool text_tool_fallback; /* Parse XML tool calls embedded in plain-text responses */
	/* Remind the model to prefer native tool_calls when it emits them as text.
	 * Turn off for models that can only produce text tool calls (e.g.
	 * qwen2.5-coder): there the reminder is noise it cannot act on.
	 * Ported from upstream PR #4 by @ilmich. */
	bool native_tool_reminder;
	int max_retries;
	int retry_backoff_ms;

	bool stream_responses;
	bool confirm_writes;
	bool confirm_bash;
	bool confirm_fetch;	  /* ask before each fetch (default on) */
	bool fetch_allow_private; /* let fetch reach loopback/private addresses */
	bool memory_persist;

	int log_level;
	char *log_file;
	char *trace_file; /* JSONL record of every model request/response */

	bool show_metrics;
	bool show_tool_calls; /* print each tool call and a result preview in the REPL */
};

/*
 * A single message in the conversation history.
 *
 * For ROLE_ASSISTANT: tool_calls holds tool calls requested by the model.
 * For ROLE_TOOL: tool_call_id and tool_name link the result to its call.
 * content may be NULL for assistant messages that only contain tool calls.
 */
struct conversation_message {
	enum message_role role;
	char *content;
	char *tool_call_id;
	char *tool_name;
	struct tool_call *tool_calls;
	size_t tool_call_count;
};

/*
 * Dynamic array of conversation messages.
 *
 * total_characters is a running byte count used to estimate token usage
 * for context-window trimming.
 */
struct conversation_history {
	struct conversation_message *messages;
	size_t count;
	size_t capacity;
	size_t total_characters;
};

/* Tool enable/disable bitmask constants. */
#define TOOL_READ     (1u << 0)
#define TOOL_WRITE    (1u << 1)
#define TOOL_EDIT     (1u << 2)
#define TOOL_SEARCH   (1u << 3)
#define TOOL_LIST_DIR (1u << 4)
#define TOOL_MEMORY   (1u << 5)
#define TOOL_RUN      (1u << 6)
#define TOOL_MOVE     (1u << 7)
#define TOOL_DELETE   (1u << 8)
#define TOOL_PLAN     (1u << 9)
#define TOOL_FETCH    (1u << 10)
#define TOOL_ALL                                                                  \
	(TOOL_READ | TOOL_WRITE | TOOL_EDIT | TOOL_SEARCH | TOOL_LIST_DIR |       \
	 TOOL_MEMORY | TOOL_RUN | TOOL_MOVE | TOOL_DELETE | TOOL_PLAN | TOOL_FETCH)
/* What "default" enables in tools.enabled: everything except fetch. */
#define TOOL_DEFAULT  (TOOL_ALL & ~TOOL_FETCH)
#define TOOL_NONE     (0u)

/* Maximum tool calls the model may request in one turn. */
#define TOOL_CALL_LIST_MAX 32

/* Maximum number of tools that can be registered. */
#define TOOLS_REGISTRY_MAX 64

/* Default cap on a tool result: read's 128 KB maximum plus room for its
 * own paging note, so read is never cut by the generic cap. */
#define TOOL_RESULT_MAX_DEFAULT (131072 + 4096)

/*
 * Result returned by agent_run() / agent_run_conv().
 * Caller must call agent_result_free() when done.
 */
struct agent_result {
	enum finish_reason finish_reason;
	char *final_response;
	int turns_used;
	int total_prompt_tokens;
	int total_completion_tokens;
	enum boris_error error_code;
};

/*
 * Structured error with a human-readable message and recoverability flag.
 */
struct boris_error_info {
	enum boris_error code;
	const char *msg;
	bool recoverable;
	char msg_storage[512];
};

/*
 * A single tool call requested by the LLM.
 *
 * id uniquely identifies the call so results can be matched back.
 * argument_json is a heap-allocated JSON object string.
 */
struct tool_call {
	char id[64];
	char name[128];
	char *argument_json;
};

/* Array of tool calls from a single LLM response. */
struct tool_call_list {
	struct tool_call *calls;
	size_t count;
};

/*
 * Response from the LLM backend.
 *
 * content is the text reply with thinking tags stripped (NULL when the
 * response consists only of tool_calls). thinking holds the extracted
 * chain-of-thought if present. Caller must call llm_response_free().
 */
struct llm_response {
	char *content;
	char *thinking;
	struct tool_call_list tool_calls;
	char finish_reason[32]; /* "stop", "tool_calls", "length", etc. */
	int prompt_tokens;
	int completion_tokens;
	bool error;
	struct boris_error_info err;
};

/*
 * Result of executing a tool.
 *
 * content is heap-allocated. is_error=true means content holds an error
 * message. Caller must call tool_result_free().
 */
struct tool_result {
	char tool_name[64];
	char tool_call_id[64];
	char *content;
	bool is_error;
};

/* Forward declaration — arena.h not included here to keep this header lean. */
struct memory_arena;

/*
 * Tool function signature.
 *
 * scratch is a per-dispatch arena for temporary allocations, destroyed
 * by tools_dispatch() after the tool returns. tool_result.content must
 * be heap-allocated (not arena memory).
 */
typedef struct tool_result (*tool_fn)(const char *argument_json,
				      const struct agent_configuration *cfg,
				      struct memory_arena *scratch);

/*
 * Tool call formatter.
 *
 * Writes a short, one-line summary of a call's arguments into buf (at most
 * size bytes, NUL-terminated), e.g. "notes.md (overwrite, 812 bytes)". The
 * tool name is shown separately, so do not repeat it. Read argument strings
 * in place; never copy large values (file content) just to describe them.
 * The registry sanitises the result for terminal display.
 */
typedef void (*tool_format_fn)(const char *argument_json, char *buf, size_t size);

/*
 * Which setting, if any, makes a tool ask the user before each call.
 * The REPL's confirm callback is consulted when the setting is on.
 */
enum tool_confirm {
	TOOL_CONFIRM_NEVER = 0, /* never asks */
	TOOL_CONFIRM_WRITES,	/* behaviour.confirm_writes */
	TOOL_CONFIRM_RUN,	/* behaviour.confirm_run */
	TOOL_CONFIRM_FETCH,	/* behaviour.confirm_fetch */
};

/*
 * Registered tool descriptor.
 *
 * Built-in tools define one of these as a const in their own source file
 * (see tool_builtins.h). parameters_schema is a JSON Schema object string;
 * the registry stores its own copy. fn is the implementation; flag is the
 * TOOL_* bit used for enable/disable gating. format_call is optional;
 * without it, displays fall back to the raw (truncated) argument JSON.
 */
struct tool_definition {
	char name[64];
	char description[1024];
	const char *parameters_schema;
	tool_fn fn;
	tool_format_fn format_call;
	unsigned int flag;
	enum tool_confirm confirm;
};

#endif /* BORIS_TYPES_H */
