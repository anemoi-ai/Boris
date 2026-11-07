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
#include "trace.h"
#include "conversation.h"
#include "json.h"
#include "logger.h"
#include "cJSON.h"
#include "metrics.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/*
 * Mark r as failed. recoverable tells the agent whether retrying the same
 * request could succeed (timeouts, rate limits, server errors) or not
 * (bad request, auth failure, malformed configuration).
 */
static void vset_error(struct llm_response *r, enum boris_error code,
		       bool recoverable, const char *fmt, va_list ap)
{
	vsnprintf(r->err.msg_storage, sizeof(r->err.msg_storage), fmt, ap);
	r->error = true;
	r->err.msg = r->err.msg_storage;
	r->err.code = code;
	r->err.recoverable = recoverable;
	log_error("llm: %s", r->err.msg);
}

static void set_error(struct llm_response *r, enum boris_error code,
		      bool recoverable, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vset_error(r, code, recoverable, fmt, ap);
	va_end(ap);
}

/* Allocate a response already marked as failed. NULL only on OOM. */
static struct llm_response *error_response(enum boris_error code,
					   bool recoverable,
					   const char *fmt, ...)
{
	struct llm_response *r = alloc_response();
	if (!r)
		return NULL;
	va_list ap;
	va_start(ap, fmt);
	vset_error(r, code, recoverable, fmt, ap);
	va_end(ap);
	return r;
}

/* HTTP statuses worth retrying: timeout, too early, rate limit, server side. */
static bool http_status_is_transient(long status)
{
	return status == 408 || status == 425 || status == 429 || status >= 500;
}

/* -------------------------------------------------------------------------
 * Tool schema serialisation
 *
 * Converts a tool_definition to the OpenAI function tool JSON object.
 * ---------------------------------------------------------------------- */

static struct cJSON *tool_to_json(const struct tool_definition *t)
{
	struct cJSON *obj = cJSON_CreateObject();
	struct cJSON *fn = cJSON_CreateObject();
	if (!obj || !fn) {
		cJSON_Delete(obj);
		cJSON_Delete(fn);
		return NULL;
	}

	cJSON_AddStringToObject(obj, "type", "function");
	cJSON_AddStringToObject(fn, "name", t->name);
	cJSON_AddStringToObject(fn, "description", t->description);

	struct cJSON *params = NULL;
	if (t->parameters_schema && t->parameters_schema[0])
		params = json_parse(t->parameters_schema);
	if (!params) {
		/* Missing or unparsable schema: minimal valid fallback */
		params = cJSON_CreateObject();
		cJSON_AddStringToObject(params, "type", "object");
	}
	cJSON_AddItemToObject(fn, "parameters", params);

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

	/* stream. (cJSON_AddBoolToObject takes a C boolean; cJSON_True and
	 * cJSON_False are type tags, both non-zero, which made this always true.)
	 * Streams carry no usage unless asked for it. */
	cJSON_AddBoolToObject(root, "stream", cfg->stream_responses);
	if (cfg->stream_responses) {
		struct cJSON *opts = cJSON_AddObjectToObject(root, "stream_options");
		cJSON_AddBoolToObject(opts, "include_usage", 1);
	}

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
	struct stream_tool_slot slots[TOOL_CALL_LIST_MAX];
	int count;
};

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
	if (a->count >= TOOL_CALL_LIST_MAX)
		return NULL;
	struct stream_tool_slot *s = &a->slots[a->count];
	memset(s, 0, sizeof(*s));
	s->args_cap = 256;
	s->args = calloc(1, s->args_cap);
	if (!s->args)
		return NULL;
	s->index = index;
	a->count++;
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
		struct tool_call *tc = &list.calls[list.count];

		snprintf(tc->id, sizeof(tc->id), "%s", s->id);
		snprintf(tc->name, sizeof(tc->name), "%s", s->name);
		tc->argument_json = strdup(s->args[0] ? s->args : "{}");
		if (tc->argument_json)
			list.count++;
	}

	return list;
}

/* -------------------------------------------------------------------------
 * Non-streaming response parser
 * ---------------------------------------------------------------------- */

static void parse_tool_calls_array(const struct cJSON *tc_arr,
				   struct tool_call_list *list)
{
	int n = cJSON_GetArraySize(tc_arr);
	if (n <= 0)
		return;
	if (n > TOOL_CALL_LIST_MAX)
		n = TOOL_CALL_LIST_MAX;

	list->calls = calloc((size_t)n, sizeof(struct tool_call));
	if (!list->calls)
		return;

	struct cJSON *item = NULL;
	cJSON_ArrayForEach(item, tc_arr)
	{
		if (list->count >= (size_t)n)
			break;
		if (tool_call_from_json(item, &list->calls[list->count]) == 0)
			list->count++;
	}
}

static void parse_non_streaming(struct llm_response *r, const char *body)
{
	/* A 200 whose body does not parse, or has no choices, is almost always
	 * a transfer cut short or a proxy's error page: worth retrying. A
	 * well-formed API error object (below) is not. */
	struct cJSON *root = json_parse(body);
	if (!root) {
		set_error(r, BORIS_ERROR_PARSE, true,
			  "failed to parse response JSON");
		return;
	}

	/* Check for API-level error object */
	struct cJSON *err_obj = cJSON_GetObjectItemCaseSensitive(root, "error");
	if (err_obj && cJSON_IsObject(err_obj)) {
		const char *emsg = json_get_str(err_obj, "message");
		set_error(r, BORIS_ERROR_LLM, false, "API error: %s",
			  emsg ? emsg : "(no message)");
		cJSON_Delete(root);
		return;
	}

	/* choices[0].message */
	struct cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
	struct cJSON *choice = cJSON_IsArray(choices)
				       ? cJSON_GetArrayItem(choices, 0)
				       : NULL;
	if (!choice) {
		set_error(r, BORIS_ERROR_PARSE, true,
			  "response missing 'choices' array");
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
		const char *cstr = json_get_str(message, "content");
		if (cstr) {
			/* Some llama.cpp builds expose thinking separately */
			const char *rc = json_get_str(message, "reasoning_content");
			if (rc && rc[0]) {
				r->thinking = strdup(rc);
				r->content = strdup(cstr);
			} else {
				r->content = strip_thinking(cstr, &r->thinking);
			}
			if (r->thinking)
				log_debug("llm: thinking trace: %.120s...",
					  r->thinking);
		}

		/* tool_calls */
		struct cJSON *tc_arr = cJSON_GetObjectItemCaseSensitive(
			message, "tool_calls");
		if (cJSON_IsArray(tc_arr)) {
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
	char *content; /* heap, NUL-terminated; NULL until the first delta */
	size_t content_len;
	size_t content_cap;
	struct stream_tool_accum tc_accum;
	char finish_reason[32];
	char error[256]; /* set if the stream carried an "error" object */
	int prompt_tokens;
	int completion_tokens;
};

static void stream_content_append(struct stream_state *ss, const char *delta)
{
	if (!delta || !*delta)
		return;
	size_t dlen = strlen(delta);
	size_t need = ss->content_len + dlen + 1;

	if (need > ss->content_cap) {
		size_t new_cap = ss->content_cap ? ss->content_cap : 4096;
		while (new_cap < need)
			new_cap *= 2;
		char *tmp = realloc(ss->content, new_cap);
		if (!tmp)
			return;
		ss->content = tmp;
		ss->content_cap = new_cap;
	}

	memcpy(ss->content + ss->content_len, delta, dlen + 1);
	ss->content_len += dlen;
}

/*
 * Handle the payload of one "data:" line (not NUL-terminated; len bytes).
 * Returns 1 when the stream signals [DONE], otherwise 0.
 */
static int process_sse_data(struct stream_state *ss, const char *data, size_t len)
{
	while (len > 0 && (*data == ' ' || *data == '\t')) {
		data++;
		len--;
	}
	if (len == 0)
		return 0;

	if (len == 6 && memcmp(data, "[DONE]", 6) == 0)
		return 1;

	struct cJSON *chunk = json_parse_with_len(data, len);
	if (!chunk) {
		log_debug("llm: stream: failed to parse chunk JSON (len=%zu)", len);
		return 0;
	}

	struct cJSON *err_obj = cJSON_GetObjectItemCaseSensitive(chunk, "error");
	if (cJSON_IsObject(err_obj)) {
		const char *emsg = json_get_str(err_obj, "message");
		snprintf(ss->error, sizeof(ss->error), "%s",
			 emsg ? emsg : "(no message)");
	}

	/* Optional: usage, usually in the final chunk */
	struct cJSON *usage = cJSON_GetObjectItemCaseSensitive(chunk, "usage");
	if (cJSON_IsObject(usage)) {
		ss->prompt_tokens = json_get_int(usage, "prompt_tokens", 0);
		ss->completion_tokens = json_get_int(usage, "completion_tokens", 0);
	}

	struct cJSON *choices = cJSON_GetObjectItemCaseSensitive(chunk, "choices");
	struct cJSON *choice = cJSON_IsArray(choices)
				       ? cJSON_GetArrayItem(choices, 0)
				       : NULL;
	if (!choice) {
		cJSON_Delete(chunk);
		return 0;
	}

	const char *fr = json_get_str(choice, "finish_reason");
	if (fr && fr[0])
		snprintf(ss->finish_reason, sizeof(ss->finish_reason), "%s", fr);

	struct cJSON *delta = cJSON_GetObjectItemCaseSensitive(choice, "delta");
	if (!delta) {
		cJSON_Delete(chunk);
		return 0;
	}

	stream_content_append(ss, json_get_str(delta, "content"));

	/* Tool call deltas */
	struct cJSON *tc_arr = cJSON_GetObjectItemCaseSensitive(delta,
								"tool_calls");
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
		const char *fname = json_get_str(fn, "name");
		if (fname && fname[0] && !slot->name[0])
			snprintf(slot->name, sizeof(slot->name), "%s", fname);
		stream_slot_append_args(slot, json_get_str(fn, "arguments"));
	}

	cJSON_Delete(chunk);
	return 0;
}

static void parse_streaming(struct llm_response *r, const char *body)
{
	struct stream_state ss;
	memset(&ss, 0, sizeof(ss));
	snprintf(ss.finish_reason, sizeof(ss.finish_reason), LLM_FINISH_STOP);

	/* Walk lines in place: no copying, so no line-length limit. */
	const char *p = body;
	int done = 0;
	while (*p && !done) {
		const char *end = strchr(p, '\n');
		if (!end)
			end = p + strlen(p);

		size_t llen = (size_t)(end - p);
		while (llen > 0 && p[llen - 1] == '\r')
			llen--;

		/* Only "data:" lines matter; comments (":") and other SSE
		 * fields ("event:", "id:") are ignored. */
		if (llen >= 5 && memcmp(p, "data:", 5) == 0)
			done = process_sse_data(&ss, p + 5, llen - 5);

		p = (*end == '\n') ? end + 1 : end;
	}

	if (ss.error[0]) {
		set_error(r, BORIS_ERROR_LLM, false, "API error: %s", ss.error);
	} else {
		/* Assemble the llm_response from accumulated state */
		if (ss.content_len > 0) {
			r->content = strip_thinking(ss.content, &r->thinking);
			if (r->thinking)
				log_debug("llm: thinking trace: %.120s...", r->thinking);
		}

		r->tool_calls = stream_accum_to_list(&ss.tc_accum);
		snprintf(r->finish_reason, sizeof(r->finish_reason), "%s",
			 ss.finish_reason);
		normalise_finish_reason(r);
	}
	r->prompt_tokens = ss.prompt_tokens;
	r->completion_tokens = ss.completion_tokens;

	free(ss.content);
	stream_accum_free(&ss.tc_accum);
}

/*
 * A body is SSE if its first non-blank line is an SSE field. Detection is
 * by content, not by cfg->stream_responses, because servers that do not
 * support streaming ignore the flag and reply with plain JSON.
 */
static bool body_is_sse(const char *body)
{
	while (*body == ' ' || *body == '\t' || *body == '\r' || *body == '\n')
		body++;
	return strncmp(body, "data:", 5) == 0 ||
	       strncmp(body, "event:", 6) == 0 ||
	       body[0] == ':';
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
		return error_response(BORIS_ERROR_BAD_ARGUMENT, false,
				      "llm_complete: NULL argument");

	if (!config->model_endpoint)
		return error_response(BORIS_ERROR_CONFIG, false,
				      "No model endpoint configured. "
				      "Run 'boris init' to set up.");

	log_info("Sending request to %s (model: %s)",
		 config->model_endpoint,
		 config->model_name ? config->model_name : "unknown");

	/* Build request body */
	char *body = build_request_body(conv, tools, num_tools, config);
	if (!body)
		return error_response(BORIS_ERROR_OUT_OF_MEMORY, false,
				      "Out of memory building request");

	/* Build URL */
	char url[640];
	build_endpoint_url(url, sizeof(url), config->model_endpoint);
	log_debug("llm: POST %s (%zu bytes)", url, strlen(body));

	/* Send HTTP request (with timing) */
	int trace_seq = trace_request(url, body);
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
	trace_response(trace_seq, http_resp.status_code, http_resp.body,
		       http_resp.body_length, http_resp.error, latency_ms);

	struct llm_response *r = NULL;

	if (http_resp.error) {
		/* Transport failure: timeout, connection refused, DNS... */
		r = error_response(BORIS_ERROR_HTTP, true, "HTTP error: %s",
				   http_resp.error);
	} else if (http_resp.status_code < 200 || http_resp.status_code >= 300) {
		/* Extract error message from body if possible */
		char detail[256] = "";
		struct cJSON *err = http_resp.body ? json_parse(http_resp.body) : NULL;
		const char *em = json_get_str(
			cJSON_GetObjectItemCaseSensitive(err, "error"), "message");
		if (em)
			snprintf(detail, sizeof(detail), ": %s", em);
		cJSON_Delete(err);
		r = error_response(BORIS_ERROR_HTTP,
				   http_status_is_transient(http_resp.status_code),
				   "HTTP %ld%s", http_resp.status_code, detail);
	} else if (!http_resp.body || http_resp.body_length == 0) {
		r = error_response(BORIS_ERROR_LLM, true, "Empty response from model");
	} else {
		log_debug("llm: HTTP %ld, body %zu bytes",
			  http_resp.status_code, http_resp.body_length);
		r = alloc_response();
		if (r) {
			if (body_is_sse(http_resp.body))
				parse_streaming(r, http_resp.body);
			else
				parse_non_streaming(r, http_resp.body);
		}
	}

	http_response_free(&http_resp);

	if (!r)
		return NULL;

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
	tool_call_list_free(&reply->tool_calls);
	free(reply);
}
