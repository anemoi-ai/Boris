/*
 * conversation.h - Conversation history management.
 *
 * Manages the message array sent to the LLM on each request. Handles
 * adding messages of all roles (system, user, assistant, tool), estimating
 * token usage, truncating to fit context windows, and serialising to the
 * cJSON format expected by the OpenAI chat completions API.
 */

#ifndef CONVERSATION_H
#define CONVERSATION_H

#include "boris_types.h"
#include <stddef.h>

struct cJSON;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Create a new, empty conversation.
 *
 * Returns NULL if memory allocation fails.
 */
struct conversation_history *conversation_create(void);

/*
 * Free a conversation and all its messages.
 */
void conversation_destroy(struct conversation_history *conv);

/*
 * Add a message to the conversation.
 *
 * The content is copied (strdup'd), so you can free your copy after
 * calling this. Returns an error code if allocation fails.
 */
enum boris_error conversation_add(struct conversation_history *conv,
				  enum message_role role,
				  const char *content);

/*
 * Add a system message (convenience wrapper).
 */
enum boris_error conversation_add_system(struct conversation_history *conv,
					 const char *content);

/*
 * Add a user message (convenience wrapper).
 */
enum boris_error conversation_add_user(struct conversation_history *conv,
				       const char *content);

/*
 * Add an assistant message (convenience wrapper).
 */
enum boris_error conversation_add_assistant(struct conversation_history *conv,
					    const char *content);

/*
 * Append an assistant message that may include tool_calls.
 *
 * Takes the relevant fields from an llm_response without depending on
 * the llm_response type (avoiding a circular dependency).
 * Pass tool_calls=NULL and tool_call_count=0 for plain text.
 *
 * Returns BORIS_OK on success, BORIS_ERROR_OUT_OF_MEMORY on failure.
 */
enum boris_error conversation_add_assistant_raw(struct conversation_history *conv,
						const char *content,
						const struct tool_call *tool_calls,
						size_t tool_call_count);

/*
 * Append an assistant message from an llm_response object.
 *
 * Convenience wrapper around conversation_add_assistant_raw() that
 * extracts the content and tool_calls from the llm_response.
 */
enum boris_error conversation_add_assistant_response(
	struct conversation_history *conv,
	const struct llm_response *resp);

/*
 * Add a ROLE_TOOL message linking a result to its originating tool call.
 */
enum boris_error conversation_add_tool_result(struct conversation_history *conv,
					      const char *tool_call_id,
					      const char *tool_name,
					      const char *content);

/*
 * Estimate token count (~4 characters per token).
 * Fast approximation used to decide when to truncate.
 */
size_t conversation_estimate_tokens(const struct conversation_history *conv);

/*
 * Remove oldest messages until the estimated token count is under max_tokens.
 * The system message at index 0 is always preserved.
 * Returns the number of messages removed.
 */
int conversation_truncate(struct conversation_history *conv, size_t max_tokens);

/*
 * Serialise the conversation to a cJSON array for the OpenAI API.
 *
 * The caller owns the returned cJSON object and must call cJSON_Delete()
 * when done. Returns NULL on OOM.
 *
 * Format produced:
 *   [
 *     {"role": "system",    "content": "..."},
 *     {"role": "user",      "content": "..."},
 *     {"role": "assistant", "content": null,
 *      "tool_calls": [{"id":"...","type":"function",
 *                      "function":{"name":"...","arguments":"..."}}]},
 *     {"role": "tool", "tool_call_id": "...", "name": "...", "content": "..."}
 *   ]
 */
struct cJSON *conversation_to_json(const struct conversation_history *conv);

/*
 * Return a read-only pointer to the last message, or NULL if empty.
 */
const struct conversation_message *conversation_last(
	const struct conversation_history *conv);

/*
 * Return the number of messages with the given role.
 */
int conversation_count_role(const struct conversation_history *conv,
			    enum message_role role);

/*
 * Clear all messages except the system prompt at index 0.
 *
 * Frees every non-system message and resets total_characters so it
 * reflects only the preserved system message. Safe to call on an
 * empty conversation.
 */
void conversation_clear(struct conversation_history *conv);

/*
 * Replace the system prompt (message at index 0) with new text.
 *
 * If no system message exists, one is prepended. total_characters is
 * updated accordingly. Returns BORIS_OK on success or
 * BORIS_ERROR_OUT_OF_MEMORY on allocation failure.
 */
enum boris_error conversation_replace_system_prompt(
	struct conversation_history *conv, const char *new_prompt);

/*
 * Deep-copy src into dst, replacing dst's entire contents.
 *
 * All existing messages in dst are freed first. Returns BORIS_OK on
 * success or BORIS_ERROR_OUT_OF_MEMORY if allocation fails partway
 * through (dst is left in a valid but shorter state).
 */
enum boris_error conversation_clone(struct conversation_history *dst,
				    const struct conversation_history *src);

/* -------------------------------------------------------------------------
 * Persistence — save and load conversations between sessions
 * ---------------------------------------------------------------------- */

/*
 * Save a conversation to a JSON file.
 *
 * The file format includes metadata (timestamp, message count) and
 * the full message array with tool calls preserved.
 *
 * Returns BORIS_OK on success, or an error code on failure.
 */
enum boris_error conversation_save(const struct conversation_history *conv,
				   const char *file_path);

/*
 * Load a conversation from a JSON file.
 *
 * Returns a newly allocated conversation_history, or NULL on failure.
 * The caller owns the result and must call conversation_destroy().
 */
struct conversation_history *conversation_load(const char *file_path);

/*
 * Return the default session file path (~/.boris/sessions/session.json).
 * Caller must free() the returned string. Returns NULL on failure.
 */
char *conversation_session_path(void);

/*
 * Read the "saved_at" timestamp from a session file without loading the
 * full conversation. Returns a heap-allocated ISO-8601 string, or NULL
 * if the file is missing or the field is absent. Caller must free().
 */
char *conversation_load_saved_at(const char *file_path);

#ifdef __cplusplus
}
#endif

#endif /* CONVERSATION_H */
