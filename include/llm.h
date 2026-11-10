/*
 * llm.h - OpenAI-compatible chat completions client.
 *
 * Builds a JSON request from the conversation history and tool schemas,
 * POSTs it to the configured endpoint, and parses the response into an
 * llm_response. Supports both non-streaming JSON and SSE streaming.
 */

#ifndef LLM_H
#define LLM_H

#include "boris_types.h"
#include "boris_errors.h"
#include "conversation.h"
#include "tools.h"

/* Finish reason string constants shared between llm.c and agent.c. */
#define LLM_FINISH_STOP   "stop"
#define LLM_FINISH_TOOL   "tool_calls"
#define LLM_FINISH_LENGTH "length"
#define LLM_FINISH_FILTER "content_filter"

/*
 * Send the conversation to the LLM and return its response.
 *
 * Only tools enabled in cfg->tools_enabled are included in the request.
 * Returns a heap-allocated llm_response; the caller must call
 * llm_response_free(). Returns NULL only on catastrophic OOM - all other
 * errors are reported via llm_response.error = true.
 */
/*
 * Receives reply text as it streams in, <think> blocks removed. Text from
 * every model call is passed on, including the remarks a model makes
 * before calling tools. Only used when streaming is on (model.stream).
 */
typedef void (*llm_text_fn)(const char *text, size_t len, void *ctx);

/* Install (or clear, with NULL) the streaming text observer. */
void llm_set_text_observer(llm_text_fn fn, void *ctx);

struct llm_response *llm_complete(const struct conversation_history *conv,
				  const struct tool_definition *tools,
				  size_t num_tools,
				  const struct agent_configuration *config);

/*
 * What an Ollama server says about the configured model's context window.
 * Every field is 0 when unknown.
 */
struct llm_server_context {
	int loaded;    /* context of the model as currently loaded */
	int num_ctx;   /* num_ctx set in the model's own parameters */
	int model_max; /* the most the model supports */
};

/*
 * Ask the server behind config->model_endpoint. Returns false if it is not
 * Ollama (or does not answer); other servers have no such question to ask.
 * Two short local requests; nothing is sent to the model.
 */
bool llm_query_server_context(const struct agent_configuration *config,
			      struct llm_server_context *out);

/*
 * Compare model.context_window with the server's and log a warning if the
 * server's is smaller: Ollama silently drops the start of a prompt that
 * does not fit, which looks like the model forgetting its instructions.
 * Returns true once the server's effective context is known. Call it again
 * after the first reply if it returned false: only a loaded model reports
 * the context it was really given.
 */
bool llm_check_server_context(const struct agent_configuration *config);

/*
 * Free an llm_response and all its heap members.
 * Frees the struct itself. Safe to call with NULL.
 */
void llm_response_free(struct llm_response *reply);


#endif /* LLM_H */
