/*
 * conversation.c - Conversation history and persistence.
 *
 * Messages are stored in a doubling dynamic array. All content strings are
 * copied on insertion; callers do not need to manage string lifetimes.
 *
 * Supports tool_calls on assistant messages and ROLE_TOOL result messages
 * as required by the OpenAI chat completions API. Serialises to/from JSON
 * for both API requests and session persistence.
 */

#define _POSIX_C_SOURCE 200809L

#include "conversation.h"
#include "boris_errors.h"
#include "logger.h"
#include "json.h"
#include "fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

/* Start with room for 8 messages. We'll grow as needed. */
#define INITIAL_CAPACITY 8

/*
 * Create a new, empty conversation.
 */
struct conversation_history *conversation_create(void)
{
	struct conversation_history *conv;

	conv = calloc(1, sizeof(struct conversation_history));
	if (!conv)
		return NULL;

	conv->messages = calloc(INITIAL_CAPACITY,
				sizeof(struct conversation_message));
	if (!conv->messages) {
		free(conv);
		return NULL;
	}

	conv->capacity = INITIAL_CAPACITY;
	conv->count = 0;
	conv->total_characters = 0;

	return conv;
}

/*
 * Free a single message's content and tool_calls.
 */
static void message_free_content(struct conversation_message *msg)
{
	free(msg->content);
	free(msg->tool_call_id);
	free(msg->tool_name);

	if (msg->tool_calls) {
		for (size_t i = 0; i < msg->tool_call_count; i++)
			free(msg->tool_calls[i].argument_json);
		free(msg->tool_calls);
	}

	msg->content = NULL;
	msg->tool_call_id = NULL;
	msg->tool_name = NULL;
	msg->tool_calls = NULL;
	msg->tool_call_count = 0;
}

static size_t message_character_count(const struct conversation_message *msg)
{
	size_t n = 0;
	if (msg->content)
		n += strlen(msg->content);
	if (msg->tool_call_id)
		n += strlen(msg->tool_call_id);
	if (msg->tool_name)
		n += strlen(msg->tool_name);
	for (size_t i = 0; i < msg->tool_call_count; i++)
		if (msg->tool_calls[i].argument_json)
			n += strlen(msg->tool_calls[i].argument_json);
	return n;
}

/*
 * Fill msg with copies of the given fields (any may be NULL). On
 * allocation failure everything already copied is freed.
 */
static enum boris_error message_init(struct conversation_message *msg,
				     enum message_role role,
				     const char *content,
				     const char *tool_call_id,
				     const char *tool_name,
				     const struct tool_call *tool_calls,
				     size_t tool_call_count)
{
	memset(msg, 0, sizeof(*msg));
	msg->role = role;

	if ((content && !(msg->content = strdup(content))) ||
	    (tool_call_id && !(msg->tool_call_id = strdup(tool_call_id))) ||
	    (tool_name && !(msg->tool_name = strdup(tool_name))))
		goto oom;

	if (tool_calls && tool_call_count > 0) {
		msg->tool_calls = calloc(tool_call_count, sizeof(struct tool_call));
		if (!msg->tool_calls)
			goto oom;
		for (size_t i = 0; i < tool_call_count; i++) {
			struct tool_call *dst = &msg->tool_calls[i];
			const struct tool_call *src = &tool_calls[i];
			snprintf(dst->id, sizeof(dst->id), "%s", src->id);
			snprintf(dst->name, sizeof(dst->name), "%s", src->name);
			msg->tool_call_count++;
			if (src->argument_json &&
			    !(dst->argument_json = strdup(src->argument_json)))
				goto oom;
		}
	}
	return BORIS_OK;

oom:
	message_free_content(msg);
	return BORIS_ERROR_OUT_OF_MEMORY;
}

static enum boris_error message_copy(struct conversation_message *dst,
				     const struct conversation_message *src)
{
	return message_init(dst, src->role, src->content, src->tool_call_id,
			    src->tool_name, src->tool_calls, src->tool_call_count);
}


/*
 * Free a conversation and all its messages.
 */
void conversation_destroy(struct conversation_history *conv)
{
	size_t i;

	if (!conv)
		return;

	for (i = 0; i < conv->count; i++)
		message_free_content(&conv->messages[i]);

	free(conv->messages);
	free(conv);
}

/* Double the message array capacity. Returns BORIS_OK or BORIS_ERROR_OUT_OF_MEMORY. */
static enum boris_error conversation_grow(struct conversation_history *conv)
{
	size_t new_capacity;
	struct conversation_message *new_messages;

	new_capacity = conv->capacity * 2;
	new_messages = realloc(conv->messages,
			       new_capacity * sizeof(struct conversation_message));
	if (!new_messages)
		return BORIS_ERROR_OUT_OF_MEMORY;

	/* Zero out the new slots */
	memset(new_messages + conv->capacity, 0,
	       (new_capacity - conv->capacity) *
		       sizeof(struct conversation_message));

	conv->messages = new_messages;
	conv->capacity = new_capacity;

	return BORIS_OK;
}


/*
 * Append msg, taking ownership of its heap fields. On failure they are
 * freed, so callers never need to clean up after this.
 */
static enum boris_error append_message(struct conversation_history *conv,
				       struct conversation_message *msg)
{
	if (conv->count >= conv->capacity && conversation_grow(conv) != BORIS_OK) {
		message_free_content(msg);
		return BORIS_ERROR_OUT_OF_MEMORY;
	}
	conv->messages[conv->count++] = *msg;
	conv->total_characters += message_character_count(msg);
	return BORIS_OK;
}

/* Build a message from the given fields and append it. */
static enum boris_error add_message(struct conversation_history *conv,
				    enum message_role role,
				    const char *content,
				    const char *tool_call_id,
				    const char *tool_name,
				    const struct tool_call *tool_calls,
				    size_t tool_call_count)
{
	struct conversation_message msg;
	enum boris_error err = message_init(&msg, role, content, tool_call_id,
					    tool_name, tool_calls, tool_call_count);
	return err != BORIS_OK ? err : append_message(conv, &msg);
}

/*
 * Add a message to the conversation.
 */
enum boris_error conversation_add(struct conversation_history *conv,
				  enum message_role role,
				  const char *content)
{
	if (!conv || !content)
		return BORIS_ERROR_BAD_ARGUMENT;
	return add_message(conv, role, content, NULL, NULL, NULL, 0);
}

/*
 * Convenience wrappers.
 */
enum boris_error conversation_add_system(struct conversation_history *conv,
					 const char *content)
{
	return conversation_add(conv, MESSAGE_ROLE_SYSTEM, content);
}

enum boris_error conversation_add_user(struct conversation_history *conv,
				       const char *content)
{
	return conversation_add(conv, MESSAGE_ROLE_USER, content);
}

enum boris_error conversation_add_assistant(struct conversation_history *conv,
					    const char *content)
{
	return conversation_add(conv, MESSAGE_ROLE_ASSISTANT, content);
}

/*
 * Append an assistant message that may include tool_calls.
 */
enum boris_error conversation_add_assistant_raw(
	struct conversation_history *conv,
	const char *content,
	const struct tool_call *tool_calls,
	size_t tool_call_count)
{
	if (!conv)
		return BORIS_ERROR_BAD_ARGUMENT;
	return add_message(conv, MESSAGE_ROLE_ASSISTANT, content, NULL, NULL,
			   tool_calls, tool_call_count);
}

/*
 * Append an assistant message from an llm_response.
 */
enum boris_error conversation_add_assistant_response(
	struct conversation_history *conv,
	const struct llm_response *resp)
{
	if (!conv || !resp)
		return BORIS_ERROR_BAD_ARGUMENT;

	return conversation_add_assistant_raw(conv, resp->content,
					      resp->tool_calls.calls,
					      resp->tool_calls.count);
}

/*
 * Add a tool result message.
 */
enum boris_error conversation_add_tool_result(
	struct conversation_history *conv,
	const char *tool_call_id,
	const char *tool_name,
	const char *content)
{
	if (!conv || !content)
		return BORIS_ERROR_BAD_ARGUMENT;
	return add_message(conv, MESSAGE_ROLE_TOOL, content, tool_call_id,
			   tool_name, NULL, 0);
}


/* Rough token estimate: chars/4 + 4 per message (API role/separator overhead). */
size_t conversation_estimate_tokens_raw(const struct conversation_history *conv)
{
	if (!conv)
		return 0;

	return (conv->total_characters / 4) + (conv->count * 4);
}

/*
 * Calibration. The rough estimate is wrong in two ways the server can
 * correct: by a constant (the tool schemas and chat template are part of
 * every prompt but not of the history) and by a ratio (code and non-English
 * text run well under four characters per token). Each response reports the
 * true prompt size, so:
 *
 *     tokens ~= raw * g_scale + g_offset
 *
 * The first report fixes the offset. After that, how much the true count
 * grew between two requests, against how much the raw estimate grew, gives
 * the ratio. Until a server reports usage, scale 1 and offset 0 leave the
 * rough estimate as it is.
 */
#define CALIBRATE_MIN_GROWTH 200 /* raw tokens between samples worth a ratio */

static double g_scale = 1.0;
static double g_offset = 0.0;
static size_t g_last_raw = 0;
static int g_last_actual = 0;

void conversation_calibrate(size_t raw_estimate, int prompt_tokens)
{
	if (prompt_tokens <= 0 || raw_estimate == 0)
		return;

	if (g_last_actual > 0 && raw_estimate >= g_last_raw + CALIBRATE_MIN_GROWTH &&
	    prompt_tokens > g_last_actual) {
		double ratio = (double)(prompt_tokens - g_last_actual) /
			       (double)(raw_estimate - g_last_raw);
		ratio = ratio < 0.6 ? 0.6 : ratio > 2.5 ? 2.5 : ratio;
		g_scale = 0.5 * g_scale + 0.5 * ratio; /* smooth out one odd turn */
	}
	if (g_last_actual == 0 || raw_estimate >= g_last_raw + CALIBRATE_MIN_GROWTH ||
	    raw_estimate < g_last_raw) {
		g_last_raw = raw_estimate;
		g_last_actual = prompt_tokens;
	}

	g_offset = (double)prompt_tokens - (double)raw_estimate * g_scale;
	if (g_offset < 0)
		g_offset = 0;
}

void conversation_calibration_reset(void)
{
	g_scale = 1.0;
	g_offset = 0.0;
	g_last_raw = 0;
	g_last_actual = 0;
}

size_t conversation_estimate_tokens(const struct conversation_history *conv)
{
	if (!conv)
		return 0;
	return (size_t)((double)conversation_estimate_tokens_raw(conv) * g_scale + g_offset);
}

/*
 * Convert a role enum to the string the OpenAI API expects.
 */
static const char *role_to_string(enum message_role role)
{
	switch (role) {
	case MESSAGE_ROLE_SYSTEM:
		return "system";
	case MESSAGE_ROLE_USER:
		return "user";
	case MESSAGE_ROLE_ASSISTANT:
		return "assistant";
	case MESSAGE_ROLE_TOOL:
		return "tool";
	default:
		return "user";
	}
}

/*
 * Serialise the conversation to a cJSON array.
 *
 * The output format matches the OpenAI chat completions API.
 */
struct cJSON *conversation_to_json(const struct conversation_history *conv)
{
	struct cJSON *arr;
	size_t i;

	if (!conv)
		return NULL;

	arr = cJSON_CreateArray();
	if (!arr)
		return NULL;

	for (i = 0; i < conv->count; i++) {
		const struct conversation_message *msg = &conv->messages[i];
		struct cJSON *msg_obj = cJSON_CreateObject();
		if (!msg_obj) {
			cJSON_Delete(arr);
			return NULL;
		}

		cJSON_AddStringToObject(msg_obj, "role",
					role_to_string(msg->role));

		/* content - may be NULL for tool-call-only assistant msgs */
		if (msg->content) {
			cJSON_AddStringToObject(msg_obj, "content",
						msg->content);
		} else {
			cJSON_AddNullToObject(msg_obj, "content");
		}

		/* tool_calls (assistant messages only) */
		if (msg->role == MESSAGE_ROLE_ASSISTANT &&
		    msg->tool_calls && msg->tool_call_count > 0) {
			struct cJSON *tc_arr = cJSON_CreateArray();
			if (!tc_arr) {
				cJSON_Delete(msg_obj);
				cJSON_Delete(arr);
				return NULL;
			}

			for (size_t ti = 0; ti < msg->tool_call_count; ti++) {
				const struct tool_call *tc =
					&msg->tool_calls[ti];
				struct cJSON *tc_obj = cJSON_CreateObject();
				if (!tc_obj) {
					cJSON_Delete(tc_arr);
					cJSON_Delete(msg_obj);
					cJSON_Delete(arr);
					return NULL;
				}

				cJSON_AddStringToObject(tc_obj, "id",
							tc->id[0] ? tc->id : "");
				cJSON_AddStringToObject(tc_obj, "type",
							"function");

				struct cJSON *fn = cJSON_CreateObject();
				if (!fn) {
					cJSON_Delete(tc_obj);
					cJSON_Delete(tc_arr);
					cJSON_Delete(msg_obj);
					cJSON_Delete(arr);
					return NULL;
				}
				cJSON_AddStringToObject(fn, "name",
							tc->name[0] ? tc->name : "");
				const char *args_val =
					tc->argument_json ? tc->argument_json : "{}";
				cJSON_AddStringToObject(fn, "arguments", args_val);
				cJSON_AddItemToObject(tc_obj, "function", fn);
				cJSON_AddItemToArray(tc_arr, tc_obj);
			}

			cJSON_AddItemToObject(msg_obj, "tool_calls", tc_arr);
		}

		/* tool_call_id and name (ROLE_TOOL only) */
		if (msg->role == MESSAGE_ROLE_TOOL) {
			if (msg->tool_call_id)
				cJSON_AddStringToObject(msg_obj,
							"tool_call_id", msg->tool_call_id);
			if (msg->tool_name)
				cJSON_AddStringToObject(msg_obj,
							"name", msg->tool_name);
		}

		cJSON_AddItemToArray(arr, msg_obj);
	}

	return arr;
}

/*
 * Remove the oldest exchanges until the estimated token count falls below
 * max_tokens. Removes in structural units (one user turn + all following
 * assistant/tool messages) to keep the history API-valid. The system message
 * at index 0 and the most recent user turn are never removed.
 *
 * Returns the number of messages removed.
 */
int conversation_truncate(struct conversation_history *conv, size_t max_tokens)
{
	size_t start_index;
	int removed = 0;

	if (!conv || conv->count == 0)
		return 0;

	/* Always keep the system message (index 0) if it exists */
	start_index = (conv->count > 0 &&
		       conv->messages[0].role == MESSAGE_ROLE_SYSTEM)
			      ? 1
			      : 0;

	/* Remove units from the start until we're under the limit */
	while (conversation_estimate_tokens(conv) > max_tokens &&
	       conv->count > start_index + 1) {
		/*
		 * Find the end of the deletion unit starting at start_index.
		 * A unit = one message + all immediately following assistant/
		 * tool messages until the next user message or end of array.
		 */
		size_t unit_end = start_index + 1;
		while (unit_end < conv->count &&
		       conv->messages[unit_end].role != MESSAGE_ROLE_USER) {
			unit_end++;
		}

		/* Don't remove the last user turn */
		int remaining_user = 0;
		for (size_t i = unit_end; i < conv->count; i++) {
			if (conv->messages[i].role == MESSAGE_ROLE_USER)
				remaining_user++;
		}
		if (remaining_user == 0)
			break;

		/* Free messages in the unit */
		size_t unit_size = unit_end - start_index;
		size_t chars_removed = 0;

		for (size_t i = start_index; i < unit_end; i++) {
			chars_removed += message_character_count(&conv->messages[i]);
			message_free_content(&conv->messages[i]);
		}

		/* Shift remaining messages down */
		memmove(&conv->messages[start_index],
			&conv->messages[unit_end],
			(conv->count - unit_end) *
				sizeof(struct conversation_message));

		conv->count -= unit_size;
		conv->total_characters -= chars_removed;
		removed += (int)unit_size;
	}

	if (removed > 0)
		log_info("Truncated conversation: removed %d messages", removed);

	return removed;
}

/*
 * Return the last message, or NULL if empty.
 */
const struct conversation_message *conversation_last(
	const struct conversation_history *conv)
{
	if (!conv || conv->count == 0)
		return NULL;
	return &conv->messages[conv->count - 1];
}

/*
 * Clear all messages except the system prompt at index 0.
 */
void conversation_clear(struct conversation_history *conv)
{
	if (!conv)
		return;

	size_t start = (conv->count > 0 &&
			conv->messages[0].role == MESSAGE_ROLE_SYSTEM)
			       ? 1
			       : 0;

	for (size_t i = start; i < conv->count; i++)
		message_free_content(&conv->messages[i]);

	conv->count = start;

	conv->total_characters = 0;
	for (size_t i = 0; i < conv->count; i++)
		conv->total_characters += message_character_count(&conv->messages[i]);
}


enum boris_error conversation_replace_system_prompt(
	struct conversation_history *conv, const char *new_prompt)
{
	if (!conv || !new_prompt)
		return BORIS_ERROR_BAD_ARGUMENT;

	char *copy = strdup(new_prompt);
	if (!copy)
		return BORIS_ERROR_OUT_OF_MEMORY;

	if (conv->count > 0 && conv->messages[0].role == MESSAGE_ROLE_SYSTEM) {
		conv->total_characters -= message_character_count(&conv->messages[0]);
		free(conv->messages[0].content);
		conv->messages[0].content = copy;
		conv->total_characters += message_character_count(&conv->messages[0]);
		return BORIS_OK;
	}

	/* No system message yet: insert one at index 0 */
	if (conv->count >= conv->capacity && conversation_grow(conv) != BORIS_OK) {
		free(copy);
		return BORIS_ERROR_OUT_OF_MEMORY;
	}
	memmove(&conv->messages[1], &conv->messages[0],
		conv->count * sizeof(struct conversation_message));
	memset(&conv->messages[0], 0, sizeof(struct conversation_message));
	conv->messages[0].role = MESSAGE_ROLE_SYSTEM;
	conv->messages[0].content = copy;
	conv->count++;
	conv->total_characters += message_character_count(&conv->messages[0]);
	return BORIS_OK;
}

/*
 * Deep-copy src into dst, replacing dst's entire contents.
 */
enum boris_error conversation_clone(struct conversation_history *dst,
				    const struct conversation_history *src)
{
	if (!dst || !src)
		return BORIS_ERROR_BAD_ARGUMENT;

	/* Free existing dst contents */
	for (size_t i = 0; i < dst->count; i++)
		message_free_content(&dst->messages[i]);
	dst->count = 0;
	dst->total_characters = 0;

	for (size_t i = 0; i < src->count; i++) {
		struct conversation_message msg;
		if (message_copy(&msg, &src->messages[i]) != BORIS_OK ||
		    append_message(dst, &msg) != BORIS_OK)
			return BORIS_ERROR_OUT_OF_MEMORY;
	}
	return BORIS_OK;
}


/*
 * Count messages with a given role.
 */
int conversation_count_role(const struct conversation_history *conv,
			    enum message_role role)
{
	if (!conv)
		return 0;
	int n = 0;
	for (size_t i = 0; i < conv->count; i++) {
		if (conv->messages[i].role == role)
			n++;
	}
	return n;
}

/* -------------------------------------------------------------------------
 * Persistence - save and load conversations between sessions
 * ---------------------------------------------------------------------- */

/*
 * Build the full save document: metadata + messages array.
 */
static struct cJSON *build_save_doc(const struct conversation_history *conv)
{
	struct cJSON *doc = cJSON_CreateObject();
	if (!doc)
		return NULL;

	/* Metadata */
	cJSON_AddStringToObject(doc, "format", "boris_session_v1");
	cJSON_AddNumberToObject(doc, "message_count", (double)conv->count);

	/* Timestamp */
	time_t now = time(NULL);
	struct tm *tm = gmtime(&now);
	if (tm) {
		char ts[64];
		strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", tm);
		cJSON_AddStringToObject(doc, "saved_at", ts);
	}

	/* Messages array */
	struct cJSON *msgs = conversation_to_json(conv);
	if (!msgs) {
		cJSON_Delete(doc);
		return NULL;
	}
	cJSON_AddItemToObject(doc, "messages", msgs);

	return doc;
}

int tool_call_from_json(const struct cJSON *tc_obj, struct tool_call *tc)
{
	memset(tc, 0, sizeof(*tc));

	const char *id = json_get_str(tc_obj, "id");
	snprintf(tc->id, sizeof(tc->id), "%s", id ? id : "");

	const cJSON *fn = cJSON_GetObjectItemCaseSensitive(tc_obj, "function");
	const char *name = json_get_str(fn, "name");
	const char *args = json_get_str(fn, "arguments");
	snprintf(tc->name, sizeof(tc->name), "%s", name ? name : "");

	tc->argument_json = strdup(args ? args : "{}");
	return tc->argument_json ? 0 : -1;
}

/*
 * Parse a single message JSON object into a conversation_message.
 */
static int parse_message(const struct cJSON *msg_obj,
			 struct conversation_message *msg)
{
	memset(msg, 0, sizeof(*msg));

	const char *role = json_get_str(msg_obj, "role");
	if (!role)
		return -1;

	if (strcmp(role, "system") == 0)
		msg->role = MESSAGE_ROLE_SYSTEM;
	else if (strcmp(role, "user") == 0)
		msg->role = MESSAGE_ROLE_USER;
	else if (strcmp(role, "assistant") == 0)
		msg->role = MESSAGE_ROLE_ASSISTANT;
	else if (strcmp(role, "tool") == 0)
		msg->role = MESSAGE_ROLE_TOOL;
	else
		return -1;

	const cJSON *content = cJSON_GetObjectItemCaseSensitive(msg_obj, "content");
	if (content && !cJSON_IsNull(content) && cJSON_IsString(content))
		msg->content = strdup(content->valuestring);

	/* tool_calls (assistant) */
	if (msg->role == MESSAGE_ROLE_ASSISTANT) {
		const cJSON *tc_arr = cJSON_GetObjectItemCaseSensitive(
			msg_obj, "tool_calls");
		if (tc_arr && cJSON_IsArray(tc_arr)) {
			int n = cJSON_GetArraySize(tc_arr);
			if (n > 0) {
				msg->tool_calls = calloc((size_t)n,
							 sizeof(struct tool_call));
				if (!msg->tool_calls)
					return -1;
				msg->tool_call_count = 0;

				struct cJSON *tc_obj = NULL;
				cJSON_ArrayForEach(tc_obj, tc_arr)
				{
					if (tool_call_from_json(tc_obj,
								&msg->tool_calls[msg->tool_call_count]) == 0)
						msg->tool_call_count++;
				}
			}
		}
	}

	/* tool_call_id and name (tool results) */
	if (msg->role == MESSAGE_ROLE_TOOL) {
		const cJSON *tci = cJSON_GetObjectItemCaseSensitive(
			msg_obj, "tool_call_id");
		if (tci && cJSON_IsString(tci))
			msg->tool_call_id = strdup(tci->valuestring);

		const cJSON *tn = cJSON_GetObjectItemCaseSensitive(
			msg_obj, "name");
		if (tn && cJSON_IsString(tn))
			msg->tool_name = strdup(tn->valuestring);
	}

	return 0;
}


enum boris_error conversation_save(const struct conversation_history *conv,
				   const char *file_path)
{
	if (!conv || !file_path)
		return BORIS_ERROR_BAD_ARGUMENT;

	if (fs_mkdirp_parent(file_path, 0755) != 0)
		return BORIS_ERROR_IO;

	struct cJSON *doc = build_save_doc(conv);
	if (!doc)
		return BORIS_ERROR_OUT_OF_MEMORY;

	char *json = cJSON_PrintUnformatted(doc);
	cJSON_Delete(doc);
	if (!json)
		return BORIS_ERROR_OUT_OF_MEMORY;

	/* Sessions can contain file contents and tool output: owner-only. */
	int rc = fs_write_atomic(file_path, json, strlen(json), 0600, false);
	free(json);
	if (rc != 0) {
		log_warning("Cannot save conversation to %s: %s",
			    file_path, strerror(errno));
		return BORIS_ERROR_IO;
	}

	log_info("Conversation saved to %s (%zu messages)",
		 file_path, conv->count);
	return BORIS_OK;
}

struct conversation_history *conversation_load(const char *file_path)
{
	if (!file_path)
		return NULL;

	size_t file_len;
	char *buf = file_read_all(file_path, 10 * 1024 * 1024, &file_len);
	if (!buf) {
		/* No saved session yet is the normal first-run case */
		if (errno == ENOENT)
			log_info("No saved conversation at %s", file_path);
		else
			log_warning("Cannot load conversation from %s: %s",
				    file_path, strerror(errno));
		return NULL;
	}

	struct cJSON *doc = json_parse(buf);
	free(buf);
	if (!doc) {
		log_warning("Failed to parse session file");
		return NULL;
	}

	/* Validate format */
	const char *fmt = json_get_str(doc, "format");
	if (!fmt || strcmp(fmt, "boris_session_v1") != 0) {
		log_warning("Unknown session format: %s", fmt ? fmt : "(null)");
		cJSON_Delete(doc);
		return NULL;
	}

	struct cJSON *msgs = cJSON_GetObjectItemCaseSensitive(doc, "messages");
	if (!msgs || !cJSON_IsArray(msgs)) {
		log_warning("Session file missing messages array");
		cJSON_Delete(doc);
		return NULL;
	}

	struct conversation_history *conv = conversation_create();
	if (!conv) {
		cJSON_Delete(doc);
		return NULL;
	}

	struct cJSON *msg_obj = NULL;
	cJSON_ArrayForEach(msg_obj, msgs)
	{
		struct conversation_message msg;
		if (parse_message(msg_obj, &msg) != 0) {
			message_free_content(&msg);
			continue;
		}
		if (append_message(conv, &msg) != BORIS_OK) {
			conversation_destroy(conv);
			cJSON_Delete(doc);
			return NULL;
		}
	}

	cJSON_Delete(doc);

	log_info("Loaded conversation from %s (%zu messages)",
		 file_path, conv->count);
	return conv;
}


char *conversation_load_saved_at(const char *file_path)
{
	if (!file_path)
		return NULL;

	FILE *f = fopen(file_path, "r");
	if (!f)
		return NULL;

	/* saved_at is always within the first ~100 bytes; read a small peek */
	char peek[512];
	size_t n = fread(peek, 1, sizeof(peek) - 1, f);
	fclose(f);
	peek[n] = '\0';

	/* Extract value with a string search - avoids JSON parse errors from
	 * the truncated buffer being structurally invalid JSON */
	const char *key = "\"saved_at\":\"";
	char *start = strstr(peek, key);
	if (!start)
		return NULL;
	start += strlen(key);
	char *end = strchr(start, '"');
	if (!end)
		return NULL;

	size_t len = (size_t)(end - start);
	char *result = malloc(len + 1);
	if (!result)
		return NULL;
	memcpy(result, start, len);
	result[len] = '\0';
	return result;
}

char *conversation_session_path(void)
{
	return fs_boris_path("sessions/session.json");
}
