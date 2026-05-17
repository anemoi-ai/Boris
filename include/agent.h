/*
 * agent.h - ReAct (Reason → Act → Observe) agent loop.
 *
 * Loop steps:
 *   1. Build an LLM request from the current conversation
 *   2. Call the model; parse its response
 *   3. finish_reason "stop"       → return the final text
 *   4. finish_reason "tool_calls" → dispatch tools, append results, goto 1
 *   5. finish_reason "length"     → truncate conversation and retry
 *   6. iteration count >= max_iterations → FINISH_MAX_ITER
 *   7. HTTP/parse error           → FINISH_ERROR
 *
 * When cfg->text_tool_fallback is set and finish_reason is "stop" but the
 * response contains XML tool-call markers, they are parsed as tool calls
 * and the loop continues. This supports models that do not produce native
 * OpenAI tool_calls JSON.
 */

#ifndef AGENT_H
#define AGENT_H

#include "boris_types.h"
#include "boris_errors.h"
#include "conversation.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Run the agent loop on a single user message.
 *
 * Creates a transient conversation, seeds it with the system prompt and
 * user_message, runs the loop, then destroys the conversation. The caller
 * must call agent_result_free() on the returned result.
 */
struct agent_result agent_run(const char *user_message,
			      const struct agent_configuration *config);

/*
 * Run the agent loop on an existing conversation.
 *
 * Does NOT append the user message — the caller must do that before calling.
 * conv must contain at least one ROLE_USER message. The caller manages the
 * conversation lifetime. Caller must call agent_result_free() on the result.
 */
struct agent_result agent_run_conv(struct conversation_history *conv,
				   const struct agent_configuration *config);

/*
 * Free heap memory owned by an agent_result.
 * Does not free the struct itself. Safe to call with NULL.
 */
void agent_result_free(struct agent_result *r);

#ifdef __cplusplus
}
#endif

#endif /* AGENT_H */
