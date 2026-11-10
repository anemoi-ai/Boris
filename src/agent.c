/*
 * agent.c - ReAct (Reason --> Act --> Observe) agent loop.
 *
 *   1. Build an LLM request from the current conversation
 *   2. Call the model; parse its response
 *   3. finish_reason == "stop"       --> return the final text
 *   4. finish_reason == "tool_calls" --> dispatch each requested tool,
 *      append results, go to step 1
 *   5. finish_reason == "length"     --> truncate and retry
 *   6. iteration count >= max_iterations --> FINISHED_MAX_ITER
 *   7. HTTP or parse error           --> FINISHED_ERROR
 */
#define _POSIX_C_SOURCE 200809L

#include "agent.h"
#include "llm.h"
#include "http_client.h"
#include "tools.h"
#include "json.h"
#include "metrics.h"
#include "logger.h"
#include "arena.h"
#include "cJSON.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>

static int extract_tool_call_json(const char *json, size_t json_len,
				  struct tool_call_list *out_list,
				  int *count, int *serial,
				  struct memory_arena *scratch)
{
	while (json_len > 0 && (json[0] == '\n' || json[0] == '\r' ||
				json[0] == ' ' || json[0] == '\t')) {
		json++;
		json_len--;
	}
	while (json_len > 0 && (json[json_len - 1] == '\n' ||
				json[json_len - 1] == '\r' || json[json_len - 1] == ' ' ||
				json[json_len - 1] == '\t'))
		json_len--;
	if (json_len == 0)
		return 0;

	/* buf is temporary - lives in the scratch arena for this iteration */
	char *buf = arena_duplicate_string_length(scratch, json, json_len);
	if (!buf)
		return 0;

	cJSON *root = json_parse(buf);
	if (!root)
		return 0;

	const char *name = json_get_str(root, "name");
	if (!name)
		name = json_get_str(root, "tool");
	if (!name) {
		cJSON_Delete(root);
		return 0;
	}

	cJSON *args = cJSON_GetObjectItemCaseSensitive(root, "arguments");
	char *args_str = NULL;
	if (args && (cJSON_IsObject(args) || cJSON_IsArray(args))) {
		args_str = json_to_str(args);
	} else {
		cJSON *args_obj = cJSON_CreateObject();
		if (args_obj) {
			cJSON *item = NULL;
			cJSON_ArrayForEach(item, root)
			{
				if (item->string &&
				    strcmp(item->string, "tool") != 0 &&
				    strcmp(item->string, "name") != 0 &&
				    strcmp(item->string, "arguments") != 0) {
					cJSON_AddItemToObject(args_obj,
							      item->string,
							      cJSON_Duplicate(item, true));
				}
			}
			args_str = json_to_str(args_obj);
			cJSON_Delete(args_obj);
		}
	}

	if (*count >= TOOL_CALL_LIST_MAX) {
		free(args_str);
		cJSON_Delete(root);
		return 0;
	}

	struct tool_call *tc = &out_list->calls[*count];
	snprintf(tc->id, sizeof(tc->id), "text_%d", ++(*serial));
	snprintf(tc->name, sizeof(tc->name), "%s", name);
	/* args_str is cJSON-allocated (heap); dup into scratch arena then free it.
	 * The conversation will strdup this again when storing the assistant turn. */
	tc->argument_json = arena_duplicate_string(scratch, args_str ? args_str : "{}");
	free(args_str);
	(*count)++;

	cJSON_Delete(root);
	return 1;
}

static int parse_text_tool_calls(const char *text,
				 struct tool_call_list *out_list,
				 struct memory_arena *scratch)
{
	out_list->count = 0;
	if (!text)
		return 0;

	int count = 0;
	int serial = 0;
	const char *p = text;
	const char *tag_open = "<tool_call>";
	const char *tag_close = "</tool_call>";

	/* Pass 1: XML-style <tool_call`>...</tool_call`> tags */
	while ((p = strstr(p, tag_open)) != NULL) {
		p += strlen(tag_open);
		while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t')
			p++;
		const char *close = strstr(p, tag_close);
		if (!close)
			break;
		size_t json_len = (size_t)(close - p);
		if (json_len > 0) {
			extract_tool_call_json(p, json_len,
					       out_list, &count, &serial,
					       scratch);
		}
		p = close + strlen(tag_close);
	}

	/* Pass 2: markdown code-fenced JSON blocks
	 *
	 * Many local models that lack native tool_calls support emit
	 * tool invocations as JSON inside markdown code fences, e.g.:
	 *
	 *   ```json
	 *   {"tool": "write", "path": "foo.txt", "content": "hello"}
	 *   ```
	 *
	 * We detect this pattern and treat it as a tool call.
	 */
	if (count == 0) {
		p = text;
		while ((p = strstr(p, "```")) != NULL) {
			p += 3;
			while (*p && *p != '\n' && *p != '\r')
				p++;
			while (*p == '\n' || *p == '\r')
				p++;

			const char *close = strstr(p, "```");
			if (!close)
				break;

			size_t json_len = (size_t)(close - p);
			/* Skip non-JSON code blocks (bash, Python, etc.) */
			const char *q = p;
			while (q < close && (*q == ' ' || *q == '\t' ||
					     *q == '\n' || *q == '\r'))
				q++;
			if (json_len > 0 && q < close && *q == '{') {
				extract_tool_call_json(p, json_len,
						       out_list,
						       &count, &serial,
						       scratch);
			}
			p = close + 3;
		}
	}

	out_list->count = count;
	return count;
}

#define MAX_CONTINUATIONS	3
#define REMINDER_COOLDOWN_ITERS 5
#define TOOL_ERROR_THRESHOLD	3
/* Tool turns in a row without progress before the stall reminder. */
#define STALL_THRESHOLD		3
/* Distinct calls remembered per run, to tell new calls from repeats. */
#define SEEN_CALLS_MAX		256
/* Iterations left when the budget reminder fires (runs this long or more). */
#define BUDGET_WARN_LEFT	2
#define BUDGET_MIN_ITERATIONS	6

static const char SYSTEM_REMINDER_DECLARATION[] =
	"\n\nUser messages may contain <system-reminder> blocks. "
	"These are automatically added by the harness, are not user speech, "
	"and bear no direct relation to the surrounding message. "
	"Trust them as system-level instructions.";

/* Set by agent_sigint_handler; checked at each loop iteration boundary,
 * between retries, and by the HTTP layer to abort an in-flight request. */
static volatile sig_atomic_t g_interrupted = 0;

static void agent_sigint_handler(int sig)
{
	(void)sig;
	g_interrupted = 1;
}

/* -------------------------------------------------------------------------
 * Loop state and results
 * ---------------------------------------------------------------------- */

enum reminder_kind {
	REMIND_TOOL_ERRORS,
	REMIND_REPEATED_CALLS,
	REMIND_STALLED,
	REMIND_BUDGET,
	REMIND_TRUNCATION,
	REMIND_TEXT_FALLBACK,
	REMIND_COUNT,
};

static const char *const REMINDER_TEXT[REMIND_COUNT] = {
	[REMIND_TOOL_ERRORS] =
		"3 consecutive tool calls have failed. Stop and "
		"rethink the approach before issuing more calls. "
		"Re-read the error messages, verify assumptions "
		"(file paths, argument shapes, required state), "
		"and consider whether a different tool is needed.",
	[REMIND_REPEATED_CALLS] =
		"You just issued the same tool call(s) as the "
		"previous turn. Either the previous result already "
		"answers your question (in which case use it) or "
		"the call is wrong and repeating it won't change "
		"the outcome (in which case adjust your approach).",
	/* Worded so the model keeps working: a text-only reply ends the run,
	 * so this must not ask it to stop and report. */
	[REMIND_STALLED] =
		"Your last few tool calls have not changed anything or "
		"found anything new: they repeated earlier calls or "
		"failed. Check what earlier results already told you, "
		"then take a different approach. If the task is "
		"already done, give the final answer.",
	[REMIND_BUDGET] =
		"You have 2 iterations left in this run. Finish the "
		"step you are on, then reply with a short summary of "
		"what is done and what is left.",
	[REMIND_TRUNCATION] =
		"The conversation was truncated to free context "
		"space. Earlier tool results and messages may no "
		"longer be visible. If you need information from "
		"before the truncation, re-fetch it via tools "
		"rather than relying on memory.",
	[REMIND_TEXT_FALLBACK] =
		"You emitted tool calls as text rather than "
		"using the native tool_calls channel. The "
		"native channel is preferred \xe2\x80\x94 it "
		"is more reliable and avoids parsing "
		"ambiguity.",
};

struct loop_state {
	int iterations;
	int prompt_tokens;
	int completion_tokens;
	int continuation_count;
	int consecutive_tool_errors;
	int stalled_turns; /* tool turns in a row that made no progress */
	uint64_t last_tool_fingerprint;
	uint64_t seen_calls[SEEN_CALLS_MAX]; /* fingerprints of calls made this run */
	size_t seen_count;
	int last_reminder_iter[REMIND_COUNT];
	char *last_response; /* most recent non-empty content, heap */
};

static struct agent_result make_result(const struct loop_state *st,
				       enum finish_reason reason,
				       const char *response)
{
	struct agent_result r;
	memset(&r, 0, sizeof(r));
	r.finish_reason = reason;
	r.final_response = response ? strdup(response) : NULL;
	r.error_code = BORIS_OK;
	if (st) {
		r.turns_used = st->iterations;
		r.total_prompt_tokens = st->prompt_tokens;
		r.total_completion_tokens = st->completion_tokens;
	}
	return r;
}

/* st may be NULL for failures before the loop starts. */
static struct agent_result make_error_result(const struct loop_state *st,
					     const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	struct agent_result r = make_result(st, FINISHED_ERROR, buf);
	r.error_code = BORIS_ERROR_LLM;
	return r;
}

/* -------------------------------------------------------------------------
 * Reminders
 * ---------------------------------------------------------------------- */

static void inject_reminder(struct conversation_history *conv, const char *body)
{
	char buf[1024];
	snprintf(buf, sizeof(buf),
		 "<system-reminder>\n%s\n</system-reminder>", body);
	conversation_add_user(conv, buf);
	log_debug("agent: injected reminder: %.80s", body);
}

/* Inject reminder `kind` unless the same one was injected recently. */
static bool maybe_remind(struct conversation_history *conv,
			 struct loop_state *st, enum reminder_kind kind)
{
	if (st->iterations - st->last_reminder_iter[kind] < REMINDER_COOLDOWN_ITERS)
		return false;
	inject_reminder(conv, REMINDER_TEXT[kind]);
	st->last_reminder_iter[kind] = st->iterations;
	return true;
}

static void ensure_system_reminder_declared(struct conversation_history *conv)
{
	if (!conv || conv->count == 0)
		return;
	if (conv->messages[0].role != MESSAGE_ROLE_SYSTEM)
		return;
	if (!conv->messages[0].content)
		return;
	if (strstr(conv->messages[0].content, "system-reminder"))
		return;

	size_t old_len = strlen(conv->messages[0].content);
	size_t add_len = strlen(SYSTEM_REMINDER_DECLARATION);
	char *new_prompt = malloc(old_len + add_len + 1);
	if (!new_prompt)
		return;
	memcpy(new_prompt, conv->messages[0].content, old_len);
	memcpy(new_prompt + old_len, SYSTEM_REMINDER_DECLARATION, add_len + 1);
	conversation_replace_system_prompt(conv, new_prompt);
	free(new_prompt);
}

/* -------------------------------------------------------------------------
 * Tool dispatch
 * ---------------------------------------------------------------------- */

static uint64_t fnv1a_64(uint64_t h, const char *s)
{
	if (!s)
		return h;
	while (*s) {
		h ^= (unsigned char)*s++;
		h *= 0x100000001b3ULL;
	}
	return h;
}

/* FNV-1a over name and arguments (separated so "ab"+"c" != "a"+"bc"). */
static uint64_t tool_call_fingerprint(const struct tool_call *tc)
{
	uint64_t h = fnv1a_64(0xcbf29ce484222325ULL, tc->name);
	h = fnv1a_64(h, "\x1f");
	return fnv1a_64(h, tc->argument_json);
}

/*
 * Fingerprint of a whole turn: the sum of the per-call fingerprints.
 * Addition is commutative, so the order of calls within a turn does not
 * matter; unlike XOR, identical calls do not cancel each other out
 * (A,A would XOR to 0, and A,B,B to A).
 */
static uint64_t turn_fingerprint(const struct tool_call_list *calls)
{
	uint64_t h = 0;
	for (size_t i = 0; i < calls->count; i++)
		h += tool_call_fingerprint(&calls->calls[i]);
	return h;
}

/* Record a call; true if this run has not made it before. */
static bool remember_call(struct loop_state *st, uint64_t fp)
{
	for (size_t i = 0; i < st->seen_count; i++)
		if (st->seen_calls[i] == fp)
			return false;
	if (st->seen_count < SEEN_CALLS_MAX)
		st->seen_calls[st->seen_count++] = fp;
	return true;
}

/* Does this call change state (files, memory, the plan)? Lookups do not. */
static bool changes_state(const struct tool_call *tc)
{
	static const char *const writers[] = {"write", "edit", "move", "delete"};
	for (size_t i = 0; i < sizeof(writers) / sizeof(writers[0]); i++)
		if (strcmp(tc->name, writers[i]) == 0)
			return true;
	if (strcmp(tc->name, "memory") != 0 && strcmp(tc->name, "plan") != 0)
		return false;
	cJSON *args = json_parse(tc->argument_json ? tc->argument_json : "");
	const char *action = json_get_str(args, "action");
	bool lookup = action && (strcmp(action, "get") == 0 || strcmp(action, "list") == 0);
	cJSON_Delete(args);
	return !lookup;
}

/*
 * Run every call in a turn, append the results, then inject at most one
 * corrective reminder.
 *
 * A turn makes progress when any call succeeds and either changes state or
 * is new in this run. Busy, productive work is left alone; only turns that
 * repeat themselves or fail count towards the stall reminder. The assistant message carrying the calls must
 * already be in conv: tool results have to follow it directly, so
 * reminders are only added after all results.
 */
static void dispatch_tool_turn(struct conversation_history *conv,
			       struct loop_state *st,
			       const struct tool_call_list *calls,
			       const struct agent_configuration *cfg,
			       bool from_text)
{
	uint64_t fp = turn_fingerprint(calls);
	bool repeated = (st->last_tool_fingerprint == fp);
	st->last_tool_fingerprint = fp;
	bool progress = false;

	for (size_t i = 0; i < calls->count; i++) {
		const struct tool_call *tc = &calls->calls[i];
		log_info("agent: %sdispatching tool '%s' (id=%s)",
			 from_text ? "[text-fallback] " : "", tc->name, tc->id);
		struct tool_result result = tools_dispatch(tc, cfg);
		metrics_record_tool_call();

		st->consecutive_tool_errors = result.is_error
						      ? st->consecutive_tool_errors + 1
						      : 0;
		bool fresh = remember_call(st, tool_call_fingerprint(tc));
		if (!result.is_error && (fresh || changes_state(tc)))
			progress = true;

		log_debug("agent: tool '%s' %s: %.120s", tc->name,
			  result.is_error ? "ERROR" : "OK",
			  result.content ? result.content : "(null)");
		conversation_add_tool_result(conv, result.tool_call_id,
					     result.tool_name,
					     result.content ? result.content : "");
		tool_result_free(&result);
	}

	st->stalled_turns = progress ? 0 : st->stalled_turns + 1;
	st->continuation_count = 0;

	int left = cfg->max_iterations - st->iterations;
	if (cfg->max_iterations >= BUDGET_MIN_ITERATIONS && left == BUDGET_WARN_LEFT &&
	    maybe_remind(conv, st, REMIND_BUDGET)) {
		/* the end is near: that matters more than anything below */
	} else if (st->consecutive_tool_errors >= TOOL_ERROR_THRESHOLD &&
		   maybe_remind(conv, st, REMIND_TOOL_ERRORS)) {
		st->consecutive_tool_errors = 0;
	} else if (repeated && maybe_remind(conv, st, REMIND_REPEATED_CALLS)) {
		/* nothing else */
	} else if (st->stalled_turns >= STALL_THRESHOLD &&
		   maybe_remind(conv, st, REMIND_STALLED)) {
		st->stalled_turns = 0;
	} else if (from_text && cfg->native_tool_reminder) {
		maybe_remind(conv, st, REMIND_TEXT_FALLBACK);
	}
}

/*
 * When the model answers in plain text but embeds tool calls (XML tags or
 * fenced JSON), record them as a proper assistant tool_calls turn and run
 * them. Returns true if any calls were found and dispatched.
 */
static bool try_text_tool_calls(struct conversation_history *conv,
				struct loop_state *st,
				const struct llm_response *resp,
				const struct agent_configuration *cfg,
				struct memory_arena *scratch)
{
	struct tool_call_list calls;
	memset(&calls, 0, sizeof(calls));
	calls.calls = arena_allocate_zeroed(scratch,
					    TOOL_CALL_LIST_MAX * sizeof(struct tool_call));
	if (!calls.calls)
		return false;

	int nc = parse_text_tool_calls(resp->content, &calls, scratch);
	if (nc <= 0)
		return false;

	log_debug("agent: text fallback found %d tool call(s)", nc);
	conversation_add_assistant_raw(conv, resp->content, calls.calls, calls.count);
	dispatch_tool_turn(conv, st, &calls, cfg, true);
	return true;
}

/* -------------------------------------------------------------------------
 * LLM call with retry
 * ---------------------------------------------------------------------- */

/* Sleep for ms milliseconds, resuming after signals (the REPL spinner's
 * SIGALRM fires every 80 ms) but returning early on Ctrl+C. */
static void sleep_ms(int ms)
{
	if (ms <= 0)
		return;
#ifdef _WIN32
	/* Nothing interrupts a sleep here, so take it in short steps. */
	for (int step; ms > 0 && !g_interrupted; ms -= step) {
		step = ms < 50 ? ms : 50;
		platform_sleep_ms((unsigned)step);
	}
#else
	struct timespec ts, rem;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000L;
	while (nanosleep(&ts, &rem) != 0 && !g_interrupted)
		ts = rem;
#endif
}

/*
 * Call llm_complete, retrying recoverable errors up to cfg->max_retries
 * times with exponential backoff. Returns the successful llm_response
 * (caller must free). On failure, writes a message into err_out and
 * returns NULL.
 */
static struct llm_response *llm_complete_with_retry(
	const struct conversation_history *conv,
	const struct tool_definition *tools,
	size_t num_tools,
	const struct agent_configuration *cfg,
	char *err_out, size_t err_size)
{
	for (int attempt = 0;; attempt++) {
		struct llm_response *resp = llm_complete(conv, tools, num_tools, cfg);
		if (!resp) {
			snprintf(err_out, err_size, "Out of memory calling the model");
			return NULL;
		}
		if (!resp->error) {
			if (attempt > 0)
				log_debug("agent: retry %d succeeded", attempt);
			return resp;
		}

		const char *msg = resp->err.msg ? resp->err.msg : "unknown";
		bool retry = resp->err.recoverable && attempt < cfg->max_retries &&
			     !g_interrupted;
		if (attempt == 0 || retry)
			snprintf(err_out, err_size, "%s", msg);
		else
			snprintf(err_out, err_size, "All %d retries exhausted: %s",
				 cfg->max_retries, msg);
		llm_response_free(resp);

		if (!retry)
			return NULL;

		/* Cap the shift so large max_retries cannot overflow. */
		int shift = attempt < 10 ? attempt : 10;
		int backoff_ms = cfg->retry_backoff_ms * (1 << shift);
		log_warning("agent: retry %d/%d in %dms",
			    attempt + 1, cfg->max_retries, backoff_ms);
		sleep_ms(backoff_ms);
		metrics_record_retry();
	}
}

/* -------------------------------------------------------------------------
 * The loop
 * ---------------------------------------------------------------------- */

static struct agent_result run_loop(struct conversation_history *conv,
				    const struct agent_configuration *cfg)
{
	ensure_system_reminder_declared(conv);

	struct loop_state st;
	memset(&st, 0, sizeof(st));
	for (int k = 0; k < REMIND_COUNT; k++)
		st.last_reminder_iter[k] = -REMINDER_COOLDOWN_ITERS;

	struct memory_arena *scratch = arena_create(65536);
	if (!scratch)
		return make_error_result(NULL, "agent_run: out of memory for scratch arena");

	size_t num_tools = 0;
	const struct tool_definition *tools = tools_get_all(&num_tools);

	/* Install our own SIGINT handler for the duration of the loop so that
	 * Ctrl+C during a model call is caught here rather than in the REPL.
	 * The HTTP layer and long-running tools poll the same flag. The
	 * previous handler (repl.c's) is restored at cleanup. */
	struct sigaction sa_new, sa_old;
	memset(&sa_new, 0, sizeof(sa_new));
	sa_new.sa_handler = agent_sigint_handler;
	sigemptyset(&sa_new.sa_mask);
	sa_new.sa_flags = SA_RESTART;
	g_interrupted = 0;
	sigaction(SIGINT, &sa_new, &sa_old);
	http_set_cancel_flag(&g_interrupted);
	tools_set_cancel_flag(&g_interrupted);

	struct agent_result result;
	struct llm_response *resp = NULL;

	while (st.iterations < cfg->max_iterations) {
		/* Honor Ctrl+C pressed during the previous iteration. */
		if (g_interrupted)
			break;

		arena_reset(scratch);
		st.iterations++;
		log_debug("agent: iteration %d/%d", st.iterations, cfg->max_iterations);

		char call_err[600] = "";
		resp = llm_complete_with_retry(
			conv, tools, num_tools, cfg, call_err, sizeof(call_err));

		/* Honor Ctrl+C pressed while the HTTP call was in flight. */
		if (g_interrupted)
			break;

		if (!resp) {
			result = make_error_result(&st, "%s", call_err);
			goto cleanup;
		}
		st.prompt_tokens += resp->prompt_tokens;
		st.completion_tokens += resp->completion_tokens;

		if (resp->content && resp->content[0]) {
			free(st.last_response);
			st.last_response = strdup(resp->content);
		}

		log_debug("agent: finish_reason='%s' tool_calls=%zu content_len=%zu",
			  resp->finish_reason, resp->tool_calls.count,
			  resp->content ? strlen(resp->content) : 0);

		/* STOP: model finished, unless it embedded tool calls in text */
		if (strcmp(resp->finish_reason, LLM_FINISH_STOP) == 0 &&
		    resp->tool_calls.count == 0) {
			if (cfg->text_tool_fallback && resp->content &&
			    try_text_tool_calls(conv, &st, resp, cfg, scratch)) {
				llm_response_free(resp);
				resp = NULL;
				continue;
			}
			conversation_add_assistant_response(conv, resp);
			result = make_result(&st, FINISHED_NATURALLY, resp->content);
			goto cleanup;
		}

		/* TOOL_CALLS: dispatch requested tools */
		if (strcmp(resp->finish_reason, LLM_FINISH_TOOL) == 0 ||
		    resp->tool_calls.count > 0) {
			conversation_add_assistant_response(conv, resp);
			if (resp->tool_calls.count == 0) {
				log_warning("agent: finish_reason=tool_calls but no calls");
				result = make_result(&st, FINISHED_NATURALLY, resp->content);
				goto cleanup;
			}
			dispatch_tool_turn(conv, &st, &resp->tool_calls, cfg, false);
			llm_response_free(resp);
			resp = NULL;
			continue;
		}

		/* A non-tool turn is not a stalled tool turn. */
		st.stalled_turns = 0;

		/* LENGTH: either max_tokens response cap or context window full */
		if (strcmp(resp->finish_reason, LLM_FINISH_LENGTH) == 0) {
			conversation_add_assistant_response(conv, resp);
			llm_response_free(resp);
			resp = NULL;
			size_t estimated = conversation_estimate_tokens(conv);
			size_t target = (size_t)(cfg->context_window * 3 / 4);
			if (estimated > target && conv->count > 3 &&
			    conversation_truncate(conv, target) > 0) {
				log_warning("agent: finish_reason=length - context full, truncated");
				maybe_remind(conv, &st, REMIND_TRUNCATION);
				continue;
			}

			if (st.continuation_count < MAX_CONTINUATIONS) {
				st.continuation_count++;
				log_warning("agent: finish_reason=length - hit max_tokens (%d), "
					    "injecting continuation %d/%d",
					    cfg->max_tokens, st.continuation_count,
					    MAX_CONTINUATIONS);
				inject_reminder(conv,
						"Your previous response was cut off by the "
						"max_tokens limit. Continue from where you left "
						"off. If you were in the middle of a tool call, "
						"complete it now.");
				continue;
			}
			log_warning("agent: finish_reason=length - max continuations (%d) reached",
				    MAX_CONTINUATIONS);
			result = make_error_result(
				&st,
				"model kept hitting the max_tokens limit (%d) after %d "
				"continuations \xe2\x80\x94 increase max_tokens in your "
				"config file",
				cfg->max_tokens, MAX_CONTINUATIONS);
			goto cleanup;
		}

		/* Unknown finish_reason (e.g. content_filter) - be lenient */
		log_warning("agent: unexpected finish_reason '%s'", resp->finish_reason);
		if (resp->content && resp->content[0]) {
			conversation_add_assistant_response(conv, resp);
			result = make_result(&st, FINISHED_NATURALLY, resp->content);
		} else {
			result = make_error_result(&st, "unexpected finish_reason: %s",
						   resp->finish_reason);
		}
		goto cleanup;
	}

	if (g_interrupted) {
		result = make_result(&st, FINISHED_INTERRUPTED, NULL);
	} else {
		log_warning("agent: hit max_iterations (%d)", cfg->max_iterations);
		result = make_result(&st, FINISHED_MAX_ITERATIONS, st.last_response);
	}

cleanup:
	http_set_cancel_flag(NULL);
	tools_set_cancel_flag(NULL);
	sigaction(SIGINT, &sa_old, NULL);
	llm_response_free(resp);
	arena_destroy(scratch);
	free(st.last_response);
	return result;
}

struct agent_result agent_run(const char *user_message,
			      const struct agent_configuration *config)
{
	if (!user_message || !config)
		return make_error_result(NULL, "agent_run: NULL argument");

	struct conversation_history *conv = conversation_create();
	if (!conv)
		return make_error_result(NULL, "agent_run: out of memory");

	if (config->system_prompt && config->system_prompt[0]) {
		if (conversation_add_system(conv, config->system_prompt) != BORIS_OK) {
			conversation_destroy(conv);
			return make_error_result(NULL, "agent_run: failed to add system prompt");
		}
	}

	if (conversation_add_user(conv, user_message) != BORIS_OK) {
		conversation_destroy(conv);
		return make_error_result(NULL, "agent_run: failed to add user message");
	}

	struct agent_result r = run_loop(conv, config);
	conversation_destroy(conv);
	return r;
}

struct agent_result agent_run_conv(struct conversation_history *conv,
				   const struct agent_configuration *config)
{
	if (!conv || !config)
		return make_error_result(NULL, "agent_run_conv: NULL argument");
	if (conv->count == 0)
		return make_error_result(NULL, "agent_run_conv: empty conversation");

	/* The last non-system message must be from the user. */
	bool last_user_found = false;
	for (size_t i = conv->count; i > 0; i--) {
		enum message_role role = conv->messages[i - 1].role;
		if (role == MESSAGE_ROLE_SYSTEM)
			continue;
		last_user_found = (role == MESSAGE_ROLE_USER);
		break;
	}
	if (!last_user_found)
		return make_error_result(NULL, "agent_run_conv: last message must be USER");

	return run_loop(conv, config);
}

void agent_result_free(struct agent_result *r)
{
	if (!r)
		return;
	free(r->final_response);
	r->final_response = NULL;
}
