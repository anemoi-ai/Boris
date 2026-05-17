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
	int max_conversation_turns;
	int request_timeout_seconds;

	char *system_prompt;
	char *system_prompt_file;

	unsigned int tools_enabled; /* Bitmask of enabled tools (TOOL_* constants) */
	char *sandbox_root;
	int run_timeout_seconds;

	bool verify_ssl;

	int max_iterations;
	bool text_tool_fallback; /* Parse XML tool calls embedded in plain-text responses */
	int max_retries;
	int retry_backoff_ms;

	bool stream_responses;
	bool confirm_writes;
	bool confirm_bash;
	bool memory_persist;

	int log_level;
	char *log_file;
	char *log_format; /* "text" or "json" */

	bool json_output;
	bool show_metrics;
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
#define TOOL_READ     (1 << 0)
#define TOOL_WRITE    (1 << 1)
#define TOOL_LIST_DIR (1 << 4)
#define TOOL_MEMORY   (1 << 5)
#define TOOL_RUN      (1 << 6)
#define TOOL_ALL      (TOOL_READ | TOOL_WRITE | TOOL_LIST_DIR | TOOL_MEMORY | TOOL_RUN)
#define TOOL_NONE     (0)

/* Maximum tool calls the model may request in one turn. */
#define TOOL_CALL_LIST_MAX 32

/* Maximum number of tools that can be registered. */
#define TOOLS_REGISTRY_MAX 64

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

/* Growable array of tool calls from a single LLM response. */
struct tool_call_list {
	struct tool_call *calls;
	size_t count;
	size_t cap;
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
 * Registered tool descriptor.
 *
 * parameters_schema is heap-allocated by the registry (strdup'd at
 * registration time). fn is the implementation; flag is the TOOL_*
 * bitmask used for enable/disable gating.
 */
struct tool_definition {
	char name[64];
	char description[512];
	char *parameters_schema;
	tool_fn fn;
	int flag;
};

#endif /* BORIS_TYPES_H */
