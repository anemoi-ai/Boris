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

#ifdef __cplusplus
extern "C" {
#endif

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
 * llm_response_free(). Returns NULL only on catastrophic OOM — all other
 * errors are reported via llm_response.error = true.
 */
struct llm_response *llm_complete(const struct conversation_history *conv,
				  const struct tool_definition *tools,
				  size_t num_tools,
				  const struct agent_configuration *config);

/*
 * Free an llm_response and all its heap members.
 * Frees the struct itself. Safe to call with NULL.
 */
void llm_response_free(struct llm_response *reply);

#ifdef __cplusplus
}
#endif

#endif /* LLM_H */
