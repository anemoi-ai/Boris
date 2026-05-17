/*
 * llm.c - OpenAI-compatible chat completions client.
 *
 * Builds the JSON request body, POSTs to the configured endpoint, and parses
 * the response. Supports both non-streaming (JSON) and streaming (SSE) formats;
 * the response format is auto-detected from the body prefix.
 *
 * Tool call fragments in SSE streams are accumulated by index into
 * stream_tool_accum and assembled into a tool_call_list on [DONE].
 */

#define _POSIX_C_SOURCE 200809L

#include "llm.h"
#include "http_client.h"
#include "conversation.h"
#include "json.h"
#include "logger.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "metrics.h"
#include "arena.h"

/* Maximum size of accumulated streaming body before we stop appending */
#define MAX_STREAM_BODY (4 * 1024 * 1024) /* 4 MB */

/* Pre-allocated content buffer size; covers >99% of responses without realloc */
#define STREAM_CONTENT_ARENA_SIZE (256 * 1024)

/* Maximum number of parallel tool call slots in a streaming response */
#define MAX_STREAM_TOOL_CALLS 16

/* -------------------------------------------------------------------------
 * Thinking trace extraction
 *
 * Qwen3 and similar reasoning models embed chain-of-thought inside
 * <think>...</think> tags in the content field. Some llama.cpp builds
 * expose it separately as "reasoning_content". Either way we strip it
 * from the visible content so the user never sees raw CoT, and store it
 * in the response's thinking field for debug logging.
 * ---------------------------------------------------------------------- */

/*
 * Strip <think>...</think> from content. Returns a newly-allocated clean
 * string; sets *thinking_out to newly-allocated thinking text (or NULL).
 * Falls back to strdup(content) on any allocation failure.
 */
static char *strip_thinking(const char *content, char **thinking_out)
{
	*thinking_out = NULL;
	if (!content)
		return NULL;

	const char *open = "<think>";
	const char *close = "</think>";
	const char *start = strstr(content, open);
	if (!start)
		return strdup(content);

	const char *end = strstr(start + strlen(open), close);
	if (!end)
		return strdup(content); /* unclosed tag - pass through */

	/* Capture thinking, trimming a single leading newline */
	const char *think_start = start + strlen(open);
	if (*think_start == '\r')
		think_start++;
	if (*think_start == '\n')
		think_start++;
	size_t think_len = (size_t)(end - think_start);
	*thinking_out = malloc(think_len + 1);
	if (*thinking_out) {
		memcpy(*thinking_out, think_start, think_len);
		(*thinking_out)[think_len] = '\0';
	}

	/* Build clean content: before <think> + after </think> */
	size_t before_len = (size_t)(start - content);
	const char *after = end + strlen(close);
	while (*after == '\n' || *after == '\r' || *after == ' ' || *after == '\t')
		after++;
	size_t after_len = strlen(after);

	char *clean = malloc(before_len + after_len + 1);
	if (!clean) {
		free(*thinking_out);
		*thinking_out = NULL;
		return strdup(content);
	}
	if (before_len)
		memcpy(clean, content, before_len);
	if (after_len)
		memcpy(clean + before_len, after, after_len);
	clean[before_len + after_len] = '\0';
	return clean;
}

/* -------------------------------------------------------------------------
 * Internal helpers - LLM response constructors
 * ---------------------------------------------------------------------- */

static void normalise_finish_reason(struct llm_response *r)
{
	if (r->tool_calls.count > 0 &&
	    strcmp(r->finish_reason, LLM_FINISH_TOOL) != 0)
		snprintf(r->finish_reason, sizeof(r->finish_reason),
			 LLM_FINISH_TOOL);
}

static struct llm_response *alloc_response(void)
{
	struct llm_response *r = calloc(1, sizeof(struct llm_response));
	if (!r) {
		log_error("llm: calloc for llm_response failed");
		return NULL;
	}
	snprintf(r->finish_reason, sizeof(r->finish_reason), LLM_FINISH_STOP);
	return r;
}

static struct llm_response *error_response(const char *fmt, ...)
{
	struct llm_response *r = alloc_response();
	if (!r)
		return NULL;
	r->error = true;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(r->err.msg_storage, sizeof(r->err.msg_storage), fmt, ap);
	va_end(ap);
	r->err.msg = r->err.msg_storage;
	r->err.code = BORIS_ERROR_LLM;
	r->err.recoverable = true;
	log_error("llm: %s", r->err.msg);
	return r;
}

/* -------------------------------------------------------------------------
 * Tool schema serialisation
 *
 * Converts a tool_definition to the OpenAI function tool JSON object.
 * ---------------------------------------------------------------------- */

static struct cJSON *tool_to_json(const struct tool_definition *t)
{
	struct cJSON *obj = cJSON_CreateObject();
	if (!obj)
		return NULL;

	cJSON_AddStringToObject(obj, "type", "function");

	struct cJSON *fn = cJSON_CreateObject();
	if (!fn) {
		cJSON_Delete(obj);
		return NULL;
	}

	cJSON_AddStringToObject(fn, "name", t->name);
	cJSON_AddStringToObject(fn, "description", t->description);

	if (t->parameters_schema && t->parameters_schema[0]) {
		struct cJSON *params = json_parse(t->parameters_schema);
		if (params) {
			cJSON_AddItemToObject(fn, "parameters", params);
		} else {
			/* Provide a minimal valid schema as fallback */
			struct cJSON *fallback = cJSON_CreateObject();
			cJSON_AddStringToObject(fallback, "type", "object");
			cJSON_AddItemToObject(fn, "parameters", fallback);
		}
	} else {
		struct cJSON *empty = cJSON_CreateObject();
		cJSON_AddStringToObject(empty, "type", "object");
		cJSON_AddItemToObject(fn, "parameters", empty);
	}

	cJSON_AddItemToObject(obj, "function", fn);
	return obj;
}

/* -------------------------------------------------------------------------
 * Request builder
 * ---------------------------------------------------------------------- */

static char *build_request_body(const struct conversation_history *conv,
				const struct tool_definition *tools,
				size_t num_tools,
				const struct agent_configuration *cfg)
{
	struct cJSON *root = cJSON_CreateObject();
	if (!root)
		return NULL;

	/* model */
	cJSON_AddStringToObject(root, "model",
				cfg->model_name ? cfg->model_name : "default");

	/* messages */
	struct cJSON *messages = conversation_to_json(conv);
	if (!messages) {
		cJSON_Delete(root);
		return NULL;
	}
	cJSON_AddItemToObject(root, "messages", messages);

	/* tools - only include enabled tools */
	int tool_count = 0;
	if (num_tools > 0 && tools) {
		struct cJSON *tools_arr = cJSON_CreateArray();
		if (!tools_arr) {
			cJSON_Delete(root);
			return NULL;
		}

		for (size_t i = 0; i < num_tools; i++) {
			if (!(cfg->tools_enabled & tools[i].flag))
				continue;
			struct cJSON *tj = tool_to_json(&tools[i]);
			if (tj) {
				cJSON_AddItemToArray(tools_arr, tj);
				tool_count++;
			}
		}

		if (tool_count > 0) {
			cJSON_AddItemToObject(root, "tools", tools_arr);
			cJSON_AddStringToObject(root, "tool_choice", "auto");
		} else {
			cJSON_Delete(tools_arr);
		}
	}

	/* sampling parameters */
	cJSON_AddNumberToObject(root, "temperature", (double)cfg->temperature);
	cJSON_AddNumberToObject(root, "max_tokens", cfg->max_tokens);

	/* stream */
	cJSON_AddBoolToObject(root, "stream",
			      cfg->stream_responses ? cJSON_True : cJSON_False);

	char *body = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);

	log_debug("llm: request body %zu bytes, %d tools",
		  body ? strlen(body) : 0, tool_count);
	return body;
}

/* -------------------------------------------------------------------------
 * URL construction
 * ---------------------------------------------------------------------- */

static void build_endpoint_url(char *out, size_t out_size,
			       const char *endpoint)
{
	size_t elen = strlen(endpoint);

	/* Strip trailing slash */
	while (elen > 0 && endpoint[elen - 1] == '/')
		elen--;

	/* Check if the endpoint already includes the path */
	const char *suffix = "/v1/chat/completions";
	size_t slen = strlen(suffix);

	if (elen > slen &&
	    strcmp(endpoint + elen - slen, suffix) == 0) {
		snprintf(out, out_size, "%.*s", (int)elen, endpoint);
	} else {
		snprintf(out, out_size, "%.*s%s", (int)elen, endpoint, suffix);
	}
}

/* -------------------------------------------------------------------------
 * Tool call accumulator (for streaming)
 *
 * The streaming API sends tool call fragments across multiple delta events,
 * identified by index. We accumulate by index, then assemble the final
 * tool_call_list on [DONE].
 * ---------------------------------------------------------------------- */

struct stream_tool_slot {
	int index;
	char id[64];
	char name[128];
	char *args;
	size_t args_len;
	size_t args_cap;
};

struct stream_tool_accum {
	struct stream_tool_slot slots[MAX_STREAM_TOOL_CALLS];
	int count;
};

static void stream_accum_init(struct stream_tool_accum *a)
{
	memset(a, 0, sizeof(*a));
}

static void stream_accum_free(struct stream_tool_accum *a)
{
	for (int i = 0; i < a->count; i++) {
		free(a->slots[i].args);
		a->slots[i].args = NULL;
	}
	a->count = 0;
}

static struct stream_tool_slot *stream_accum_slot(struct stream_tool_accum *a,
						  int index)
{
	for (int i = 0; i < a->count; i++) {
		if (a->slots[i].index == index)
			return &a->slots[i];
	}
	if (a->count >= MAX_STREAM_TOOL_CALLS)
		return NULL;
	struct stream_tool_slot *s = &a->slots[a->count++];
	memset(s, 0, sizeof(*s));
	s->index = index;
	s->args_cap = 256;
	s->args = calloc(1, s->args_cap);
	return s;
}

static void stream_slot_append_args(struct stream_tool_slot *s,
				    const char *fragment)
{
	if (!fragment || !*fragment)
		return;
	size_t flen = strlen(fragment);

	if (s->args_len + flen + 1 > s->args_cap) {
		size_t new_cap = s->args_cap * 2;
		while (new_cap < s->args_len + flen + 1)
			new_cap *= 2;
		char *tmp = realloc(s->args, new_cap);
		if (!tmp)
			return;
		s->args = tmp;
		s->args_cap = new_cap;
	}
	memcpy(s->args + s->args_len, fragment, flen);
	s->args_len += flen;
	s->args[s->args_len] = '\0';
}

static struct tool_call_list stream_accum_to_list(
	const struct stream_tool_accum *a)
{
	struct tool_call_list list;
	memset(&list, 0, sizeof(list));

	if (a->count == 0)
		return list;

	list.calls = calloc((size_t)a->count, sizeof(struct tool_call));
	if (!list.calls)
		return list;

	for (int i = 0; i < a->count; i++) {
		const struct stream_tool_slot *s = &a->slots[i];
		struct tool_call *tc = &list.calls[i];

		snprintf(tc->id, sizeof(tc->id), "%s", s->id);
		snprintf(tc->name, sizeof(tc->name), "%s", s->name);
		tc->argument_json = s->args && s->args[0]
					    ? strdup(s->args)
					    : strdup("{}");
		list.count++;
	}

	return list;
}

/* -------------------------------------------------------------------------
 * Non-streaming response parser
 * ---------------------------------------------------------------------- */

static int parse_tool_calls_array(const struct cJSON *tc_arr,
				  struct tool_call_list *list)
{
	if (!tc_arr || !cJSON_IsArray(tc_arr))
		return 0;

	int n = cJSON_GetArraySize(tc_arr);
	if (n <= 0)
		return 0;

	list->calls = calloc((size_t)n, sizeof(struct tool_call));
	if (!list->calls)
		return 0;

	int parsed = 0;
	struct cJSON *item = NULL;
	cJSON_ArrayForEach(item, tc_arr)
	{
		if (parsed >= TOOL_CALL_LIST_MAX)
			break;

		struct tool_call *tc = &list->calls[parsed];

		const char *id = json_get_str(item, "id");
		snprintf(tc->id, sizeof(tc->id), "%s", id ? id : "");

		struct cJSON *fn = cJSON_GetObjectItemCaseSensitive(
			item, "function");
		if (fn) {
			const char *fname = json_get_str(fn, "name");
			snprintf(tc->name, sizeof(tc->name), "%s",
				 fname ? fname : "");

			const char *fargs = json_get_str(fn, "arguments");
			tc->argument_json = strdup(fargs ? fargs : "{}");
		} else {
			tc->argument_json = strdup("{}");
		}

		parsed++;
	}

	list->count = (size_t)parsed;
	return parsed;
}

static void parse_non_streaming(struct llm_response *r, const char *body)
{
	struct cJSON *root = json_parse(body);
	if (!root) {
		r->error = true;
		snprintf(r->err.msg_storage, sizeof(r->err.msg_storage),
			 "failed to parse response JSON");
		r->err.msg = r->err.msg_storage;
		r->err.code = BORIS_ERROR_PARSE;
		return;
	}

	/* Check for API-level error object */
	struct cJSON *err_obj = cJSON_GetObjectItemCaseSensitive(root, "error");
	if (err_obj && cJSON_IsObject(err_obj)) {
		const char *emsg = json_get_str(err_obj, "message");
		r->error = true;
		snprintf(r->err.msg_storage, sizeof(r->err.msg_storage),
			 "API error: %s", emsg ? emsg : "(no message)");
		r->err.msg = r->err.msg_storage;
		r->err.code = BORIS_ERROR_LLM;
		cJSON_Delete(root);
		return;
	}

	/* choices[0].message */
	struct cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
	if (!choices || !cJSON_IsArray(choices) ||
	    cJSON_GetArraySize(choices) == 0) {
		r->error = true;
		snprintf(r->err.msg_storage, sizeof(r->err.msg_storage),
			 "response missing 'choices' array");
		r->err.msg = r->err.msg_storage;
		r->err.code = BORIS_ERROR_PARSE;
		cJSON_Delete(root);
		return;
	}

	struct cJSON *choice = cJSON_GetArrayItem(choices, 0);
	if (!choice) {
		r->error = true;
		snprintf(r->err.msg_storage, sizeof(r->err.msg_storage),
			 "choices[0] is null");
		r->err.msg = r->err.msg_storage;
		r->err.code = BORIS_ERROR_PARSE;
		cJSON_Delete(root);
		return;
	}

	/* finish_reason */
	const char *fr = json_get_str(choice, "finish_reason");
	if (fr)
		snprintf(r->finish_reason, sizeof(r->finish_reason), "%s", fr);

	struct cJSON *message = cJSON_GetObjectItemCaseSensitive(choice,
								 "message");
	if (message) {
		/* content - strip thinking traces before storing */
		struct cJSON *content_item = cJSON_GetObjectItemCaseSensitive(
			message, "content");
		if (content_item && !cJSON_IsNull(content_item)) {
			const char *cstr = cJSON_GetStringValue(content_item);
			if (cstr) {
				/* Some llama.cpp builds expose thinking separately */
				struct cJSON *rc = cJSON_GetObjectItemCaseSensitive(
					message, "reasoning_content");
				if (rc && cJSON_IsString(rc) && rc->valuestring[0]) {
					r->thinking = strdup(rc->valuestring);
					r->content = strdup(cstr);
				} else {
					r->content = strip_thinking(cstr, &r->thinking);
				}
				if (r->thinking)
					log_debug("llm: thinking trace: %.120s...",
						  r->thinking);
			}
		}

		/* tool_calls */
		struct cJSON *tc_arr = cJSON_GetObjectItemCaseSensitive(
			message, "tool_calls");
		if (tc_arr && cJSON_IsArray(tc_arr)) {
			parse_tool_calls_array(tc_arr, &r->tool_calls);
			normalise_finish_reason(r);
		}
	}

	/* usage */
	struct cJSON *usage = cJSON_GetObjectItemCaseSensitive(root, "usage");
	if (usage) {
		r->prompt_tokens = json_get_int(usage, "prompt_tokens", 0);
		r->completion_tokens = json_get_int(usage, "completion_tokens", 0);
	}

	cJSON_Delete(root);
}

/* -------------------------------------------------------------------------
 * Streaming SSE parser
 * ---------------------------------------------------------------------- */

struct stream_state {
	struct memory_arena *arena;
	char *content;
	size_t content_len;
	size_t content_cap;
	int content_heap; /* 1 when content has spilled from arena to a malloc buffer */
	struct stream_tool_accum tc_accum;
	char finish_reason[32];
	int prompt_tokens;
	int completion_tokens;
};

static void stream_state_init(struct stream_state *ss)
{
	memset(ss, 0, sizeof(*ss));

	ss->arena = arena_create(STREAM_CONTENT_ARENA_SIZE);
	if (ss->arena) {
		ss->content = arena_allocate_zeroed(ss->arena, STREAM_CONTENT_ARENA_SIZE);
		if (ss->content) {
			ss->content_cap = STREAM_CONTENT_ARENA_SIZE;
		} else {
			arena_destroy(ss->arena);
			ss->arena = NULL;
		}
	}

	if (!ss->content) {
		ss->content_cap = 4096;
		ss->content = calloc(1, ss->content_cap);
	}

	snprintf(ss->finish_reason, sizeof(ss->finish_reason), LLM_FINISH_STOP);
	stream_accum_init(&ss->tc_accum);
}

static void stream_state_free(struct stream_state *ss)
{
	/* Free content: heap if it spilled from arena, or if there was no arena */
	if (ss->content_heap || !ss->arena)
		free(ss->content);
	if (ss->arena) {
		arena_destroy(ss->arena);
		ss->arena = NULL;
	}
	/* Slot args are always heap-allocated, stream_accum_free handles them */
	stream_accum_free(&ss->tc_accum);
}

static void stream_content_append(struct stream_state *ss, const char *delta)
{
	if (!delta || !*delta)
		return;
	size_t dlen = strlen(delta);

	if (ss->content_len + dlen + 1 <= ss->content_cap) {
		/* Fast path: fits in the pre-allocated buffer */
		memcpy(ss->content + ss->content_len, delta, dlen);
		ss->content_len += dlen;
		ss->content[ss->content_len] = '\0';
		return;
	}

	/* Slow path: overflow. Compute new capacity. */
	size_t new_cap = ss->content_cap * 2;
	while (new_cap < ss->content_len + dlen + 1)
		new_cap *= 2;
	if (new_cap > (size_t)MAX_STREAM_BODY)
		new_cap = MAX_STREAM_BODY;

	if (ss->arena && !ss->content_heap) {
		/* Content was arena-allocated; transition to a heap buffer */
		char *new_buf = malloc(new_cap);
		if (!new_buf)
			return;
		memcpy(new_buf, ss->content, ss->content_len);
		new_buf[ss->content_len] = '\0';
		ss->content = new_buf;
		ss->content_cap = new_cap;
		ss->content_heap = 1;
	} else {
		/* No arena, or already spilled to heap: realloc as before */
		char *tmp = realloc(ss->content, new_cap);
		if (!tmp)
			return;
		ss->content = tmp;
		ss->content_cap = new_cap;
	}

	memcpy(ss->content + ss->content_len, delta, dlen);
	ss->content_len += dlen;
	ss->content[ss->content_len] = '\0';
}

static int process_sse_data_line(struct stream_state *ss, const char *data)
{
	/* Skip leading whitespace */
	while (*data == ' ' || *data == '\t')
		data++;

	if (strcmp(data, "[DONE]") == 0)
		return 1;

	struct cJSON *chunk = json_parse(data);
	if (!chunk) {
		log_debug("llm: stream: failed to parse chunk JSON (len=%zu)",
			  strlen(data));
		return 0;
	}

	struct cJSON *choices = cJSON_GetObjectItemCaseSensitive(chunk,
								 "choices");
	if (!choices || !cJSON_IsArray(choices) ||
	    cJSON_GetArraySize(choices) == 0) {
		cJSON_Delete(chunk);
		return 0;
	}

	struct cJSON *choice = cJSON_GetArrayItem(choices, 0);
	if (!choice) {
		cJSON_Delete(chunk);
		return 0;
	}

	/* finish_reason */
	const char *fr = json_get_str(choice, "finish_reason");
	if (fr && fr[0] && strcmp(fr, "null") != 0)
		snprintf(ss->finish_reason, sizeof(ss->finish_reason), "%s", fr);

	struct cJSON *delta = cJSON_GetObjectItemCaseSensitive(choice, "delta");
	if (!delta) {
		cJSON_Delete(chunk);
		return 0;
	}

	/* Content delta */
	struct cJSON *content_item = cJSON_GetObjectItemCaseSensitive(
		delta, "content");
	if (content_item && !cJSON_IsNull(content_item)) {
		const char *cstr = cJSON_GetStringValue(content_item);
		if (cstr)
			stream_content_append(ss, cstr);
	}

	/* Tool call deltas */
	struct cJSON *tc_arr = cJSON_GetObjectItemCaseSensitive(delta,
								"tool_calls");
	if (tc_arr && cJSON_IsArray(tc_arr)) {
		struct cJSON *tc_item = NULL;
		cJSON_ArrayForEach(tc_item, tc_arr)
		{
			int index = json_get_int(tc_item, "index", 0);
			struct stream_tool_slot *slot =
				stream_accum_slot(&ss->tc_accum, index);
			if (!slot)
				continue;

			const char *id = json_get_str(tc_item, "id");
			if (id && id[0] && !slot->id[0])
				snprintf(slot->id, sizeof(slot->id), "%s", id);

			struct cJSON *fn = cJSON_GetObjectItemCaseSensitive(
				tc_item, "function");
			if (fn) {
				const char *fname = json_get_str(fn, "name");
				if (fname && fname[0] && !slot->name[0])
					snprintf(slot->name,
						 sizeof(slot->name), "%s", fname);
				const char *fargs = json_get_str(fn, "arguments");
				if (fargs)
					stream_slot_append_args(slot, fargs);
			}
		}
	}

	/* Optional: usage in the final chunk */
	struct cJSON *usage = cJSON_GetObjectItemCaseSensitive(chunk, "usage");
	if (usage) {
		ss->prompt_tokens = json_get_int(usage, "prompt_tokens", 0);
		ss->completion_tokens = json_get_int(usage, "completion_tokens", 0);
	}

	cJSON_Delete(chunk);
	return 0;
}

static void parse_streaming(struct llm_response *r, const char *body)
{
	struct stream_state ss;
	stream_state_init(&ss);

	const char *p = body;
	char line[8192];
	int done = 0;

	while (*p && !done) {
		const char *end = p;
		while (*end && *end != '\n')
			end++;

		size_t llen = (size_t)(end - p);

		/* Strip trailing '\r' */
		while (llen > 0 && (p[llen - 1] == '\r'))
			llen--;

		if (llen == 0) {
			p = (*end == '\n') ? end + 1 : end;
			continue;
		}

		if (llen >= sizeof(line))
			llen = sizeof(line) - 1;
		memcpy(line, p, llen);
		line[llen] = '\0';

		p = (*end == '\n') ? end + 1 : end;

		/* Skip SSE comment lines */
		if (line[0] == ':')
			continue;

		/* Process "data: ..." lines */
		if (strncmp(line, "data:", 5) == 0)
			done = process_sse_data_line(&ss, line + 5);
	}

	/* Assemble the llm_response from accumulated state */
	if (ss.content && ss.content_len > 0) {
		r->content = strip_thinking(ss.content, &r->thinking);
		if (r->thinking)
			log_debug("llm: thinking trace: %.120s...", r->thinking);
	}

	r->tool_calls = stream_accum_to_list(&ss.tc_accum);
	r->prompt_tokens = ss.prompt_tokens;
	r->completion_tokens = ss.completion_tokens;
	snprintf(r->finish_reason, sizeof(r->finish_reason), "%s",
		 ss.finish_reason);

	normalise_finish_reason(r);

	stream_state_free(&ss);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

struct llm_response *llm_complete(const struct conversation_history *conv,
				  const struct tool_definition *tools,
				  size_t num_tools,
				  const struct agent_configuration *config)
{
	if (!conv || !config)
		return error_response("llm_complete: NULL argument");

	if (!config->model_endpoint)
		return error_response("No model endpoint configured. "
				      "Run 'boris init' to set up.");

	log_info("Sending request to %s (model: %s)",
		 config->model_endpoint,
		 config->model_name ? config->model_name : "unknown");

	/* Build request body */
	char *body = build_request_body(conv, tools, num_tools, config);
	if (!body)
		return error_response("Out of memory building request");

	/* Build URL */
	char url[640];
	build_endpoint_url(url, sizeof(url), config->model_endpoint);
	log_debug("llm: POST %s (%zu bytes)", url, strlen(body));

	/* Send HTTP request (with timing) */
	struct timespec t_start, t_end;
	clock_gettime(CLOCK_MONOTONIC, &t_start);

	struct http_response http_resp = http_post(
		url, body, "application/json",
		config->api_key,
		config->request_timeout_seconds > 0 ? config->request_timeout_seconds : 120,
		config->verify_ssl);

	clock_gettime(CLOCK_MONOTONIC, &t_end);
	double latency_ms = (t_end.tv_sec - t_start.tv_sec) * 1000.0 +
			    (t_end.tv_nsec - t_start.tv_nsec) / 1000000.0;
	free(body);

	if (http_resp.error) {
		metrics_record_request(latency_ms, 0, 0, true);
		struct llm_response *r = error_response("HTTP error: %s",
							http_resp.error);
		http_response_free(&http_resp);
		return r;
	}

	log_debug("llm: HTTP %ld, body %zu bytes",
		  http_resp.status_code, http_resp.body_length);

	if (http_resp.status_code < 200 || http_resp.status_code >= 300) {
		/* Extract error message from body if possible */
		char detail[256] = "";
		if (http_resp.body && http_resp.body_length > 0) {
			struct cJSON *err = json_parse(http_resp.body);
			if (err) {
				struct cJSON *eo = cJSON_GetObjectItemCaseSensitive(
					err, "error");
				if (eo) {
					const char *em = json_get_str(eo,
								      "message");
					if (em)
						snprintf(detail, sizeof(detail),
							 ": %s", em);
				}
				cJSON_Delete(err);
			}
		}
		struct llm_response *r = error_response("HTTP %ld%s",
							http_resp.status_code, detail);
		http_response_free(&http_resp);
		return r;
	}

	if (!http_resp.body || http_resp.body_length == 0) {
		http_response_free(&http_resp);
		return error_response("Empty response from model");
	}

	/* Allocate result */
	struct llm_response *r = alloc_response();
	if (!r) {
		http_response_free(&http_resp);
		return NULL;
	}

	/* Parse - auto-detect SSE vs JSON format */
	if (config->stream_responses ||
	    (http_resp.body && http_resp.body_length > 0 &&
	     strncmp(http_resp.body, "data:", 5) == 0)) {
		parse_streaming(r, http_resp.body);
	} else {
		parse_non_streaming(r, http_resp.body);
	}

	http_response_free(&http_resp);

	metrics_record_request(latency_ms, r->prompt_tokens,
			       r->completion_tokens, r->error);

	if (!r->error) {
		log_info("Received response (%d prompt tokens, "
			 "%d completion tokens)",
			 r->prompt_tokens, r->completion_tokens);
	}

	return r;
}

void llm_response_free(struct llm_response *reply)
{
	if (!reply)
		return;

	free(reply->content);
	free(reply->thinking);

	for (size_t i = 0; i < reply->tool_calls.count; i++)
		free(reply->tool_calls.calls[i].argument_json);
	free(reply->tool_calls.calls);

	free(reply);
}
