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
#include "tools.h"
#include "json.h"
#include "metrics.h"
#include "logger.h"
#include "arena.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <ctype.h>
#include <time.h>
#include <stdint.h>

#define LAST_RESPONSE_MAX 8192

static struct agent_result make_result(enum finish_reason reason,
				       const char *response,
				       int iterations,
				       int prompt_tok,
				       int comp_tok)
{
	struct agent_result r;
	r.finish_reason = reason;
	r.final_response = response ? strdup(response) : NULL;
	r.turns_used = iterations;
	r.total_prompt_tokens = prompt_tok;
	r.total_completion_tokens = comp_tok;
	r.error_code = BORIS_OK;
	return r;
}

static struct agent_result make_error_result(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	struct agent_result r = make_result(FINISHED_ERROR, buf, 0, 0, 0);
	r.error_code = BORIS_ERROR_LLM;
	return r;
}

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

static void append_assistant_turn(struct conversation_history *conv,
				  const struct llm_response *resp)
{
	conversation_add_assistant_response(conv, resp);
}

static void append_tool_result(struct conversation_history *conv,
			       const struct tool_result *result)
{
	conversation_add_tool_result(conv, result->tool_call_id,
				     result->tool_name, result->content);
}

static int is_transient_error(const struct llm_response *resp)
{
	if (!resp->err.recoverable)
		return 0;

	const char *msg = resp->err.msg ? resp->err.msg : "";

	if (strstr(msg, "No model endpoint") ||
	    strstr(msg, "NULL argument") ||
	    strstr(msg, "Out of memory building"))
		return 0;

	return 1;
}

static void sleep_ms(int ms)
{
	if (ms <= 0)
		return;
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

/*
 * Call llm_complete, retrying on transient errors up to cfg->max_retries
 * times with exponential backoff. Returns the successful llm_response on
 * success (caller must free). On permanent failure, writes a message into
 * err_out and returns NULL.
 */
static struct llm_response *llm_complete_with_retry(
	const struct conversation_history *conv,
	const struct tool_definition *tools,
	size_t num_tools,
	const struct agent_configuration *cfg,
	char *err_out, size_t err_size)
{
	struct llm_response *resp = llm_complete(conv, tools, num_tools, cfg);
	if (!resp) {
		snprintf(err_out, err_size, "llm_complete returned NULL");
		return NULL;
	}
	if (!resp->error)
		return resp;

	log_error("agent: LLM error: %s",
		  resp->err.msg ? resp->err.msg : "unknown");
	snprintf(err_out, err_size, "%s",
		 resp->err.msg ? resp->err.msg : "unknown");
	int transient = is_transient_error(resp);
	llm_response_free(resp);

	if (!transient || cfg->max_retries <= 0)
		return NULL;

	for (int i = 0; i < cfg->max_retries; i++) {
		int backoff_ms = cfg->retry_backoff_ms * (1 << i);
		log_warning("agent: retry %d/%d in %dms",
			    i + 1, cfg->max_retries, backoff_ms);
		sleep_ms(backoff_ms);
		metrics_record_retry();

		resp = llm_complete(conv, tools, num_tools, cfg);
		if (!resp) {
			snprintf(err_out, err_size,
				 "llm_complete returned NULL on retry %d", i + 1);
			return NULL;
		}
		if (!resp->error) {
			log_debug("agent: retry %d succeeded", i + 1);
			return resp;
		}
		snprintf(err_out, err_size, "All %d retries exhausted: %s",
			 cfg->max_retries,
			 resp->err.msg ? resp->err.msg : "unknown");
		llm_response_free(resp);
	}

	return NULL;
}

#define MAX_CONTINUATIONS	3
#define REMINDER_COOLDOWN_ITERS 5
#define DRIFT_STUCK_THRESHOLD	6

static const char SYSTEM_REMINDER_DECLARATION[] =
	"\n\nUser messages may contain <system-reminder> blocks. "
	"These are automatically added by the harness, are not user speech, "
	"and bear no direct relation to the surrounding message. "
	"Trust them as system-level instructions.";

static uint64_t fnv1a_64(const char *s)
{
	uint64_t h = 0xcbf29ce484222325ULL;
	if (!s)
		return h;
	while (*s) {
		h ^= (unsigned char)*s++;
		h *= 0x100000001b3ULL;
	}
	return h;
}

static uint64_t tool_call_fingerprint(const struct tool_call *tc)
{
	uint64_t h = fnv1a_64(tc->name);
	h ^= fnv1a_64(tc->argument_json);
	h *= 0x100000001b3ULL;
	return h;
}

static void inject_reminder(struct conversation_history *conv, const char *body)
{
	char buf[1024];
	snprintf(buf, sizeof(buf),
		 "<system-reminder>\n%s\n</system-reminder>", body);
	conversation_add_user(conv, buf);
	log_debug("agent: injected reminder: %.80s", body);
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
	memcpy(new_prompt + old_len, SYSTEM_REMINDER_DECLARATION, add_len);
	new_prompt[old_len + add_len] = '\0';
	conversation_replace_system_prompt(conv, new_prompt);
	free(new_prompt);
}

/* Set by agent_sigint_handler; checked at each loop iteration boundary. */
static volatile sig_atomic_t g_interrupted = 0;

static void agent_sigint_handler(int sig)
{
	(void)sig;
	g_interrupted = 1;
}

static struct agent_result run_loop(struct conversation_history *conv,
				    const struct agent_configuration *cfg)
{
	ensure_system_reminder_declared(conv);

	int iterations = 0;
	int total_prompt_tok = 0;
	int total_completion_tok = 0;
	int continuation_count = 0;

	int consecutive_tool_errors = 0;
	int consecutive_toolcall_turns = 0;
	int last_error_reminder_iter = -REMINDER_COOLDOWN_ITERS;
	int last_truncation_reminder_iter = -REMINDER_COOLDOWN_ITERS;
	int last_textfallback_reminder_iter = -REMINDER_COOLDOWN_ITERS;
	int last_repeat_reminder_iter = -REMINDER_COOLDOWN_ITERS;
	int last_drift_reminder_iter = -REMINDER_COOLDOWN_ITERS;
	uint64_t last_tool_fingerprint = 0;

	char *last_response = malloc(LAST_RESPONSE_MAX);
	if (!last_response) {
		return make_error_result("agent_run: out of memory");
	}
	last_response[0] = '\0';

	struct memory_arena *scratch = arena_create(65536);
	if (!scratch) {
		free(last_response);
		return make_error_result("agent_run: out of memory for scratch arena");
	}

	size_t num_tools = 0;
	const struct tool_definition *tools = tools_get_all(&num_tools);

	/* Install our own SIGINT handler for the duration of the loop so that
	 * Ctrl+C during a model call is caught here rather than in the REPL.
	 * The previous handler (repl.c's) is restored at cleanup. */
	struct sigaction sa_new, sa_old;
	memset(&sa_new, 0, sizeof(sa_new));
	sa_new.sa_handler = agent_sigint_handler;
	sigemptyset(&sa_new.sa_mask);
	sa_new.sa_flags = SA_RESTART;
	g_interrupted = 0;
	sigaction(SIGINT, &sa_new, &sa_old);

	struct agent_result result;
	struct llm_response *resp = NULL;

	while (iterations < cfg->max_iterations) {
		/* Honor Ctrl+C pressed during the previous iteration. */
		if (g_interrupted) {
			result = make_result(FINISHED_INTERRUPTED, NULL,
					     iterations, total_prompt_tok,
					     total_completion_tok);
			goto cleanup;
		}

		arena_reset(scratch);
		iterations++;
		log_debug("agent: iteration %d/%d", iterations, cfg->max_iterations);

		char call_err[600] = "";
		resp = llm_complete_with_retry(
			conv, tools, num_tools, cfg, call_err, sizeof(call_err));

		/* Honor Ctrl+C pressed while the HTTP call was in flight. */
		if (g_interrupted) {
			result = make_result(FINISHED_INTERRUPTED, NULL,
					     iterations, total_prompt_tok,
					     total_completion_tok);
			goto cleanup;
		}

		if (!resp) {
			result = make_error_result("%s", call_err);
			goto cleanup;
		}
		total_prompt_tok += resp->prompt_tokens;
		total_completion_tok += resp->completion_tokens;

		if (resp->content) {
			snprintf(last_response, LAST_RESPONSE_MAX, "%s", resp->content);
		}

		log_debug("agent: finish_reason='%s' tool_calls=%zu content_len=%zu",
			  resp->finish_reason, resp->tool_calls.count,
			  resp->content ? strlen(resp->content) : 0);

		/* STOP: model finished */
		if (strcmp(resp->finish_reason, LLM_FINISH_STOP) == 0 &&
		    resp->tool_calls.count == 0) {
			if (cfg->text_tool_fallback && resp->content) {
				struct tool_call_list text_calls;
				memset(&text_calls, 0, sizeof(text_calls));
				text_calls.calls = arena_allocate_zeroed(scratch,
									 TOOL_CALL_LIST_MAX * sizeof(struct tool_call));
				if (!text_calls.calls) {
					llm_response_free(resp);
					resp = NULL;
					continue;
				}
				int nc = parse_text_tool_calls(resp->content, &text_calls,
							       scratch);
				if (nc > 0) {
					log_debug("agent: text fallback found %d tool call(s)", nc);
					struct llm_response synthetic = *resp;
					synthetic.tool_calls = text_calls;
					snprintf(synthetic.finish_reason,
						 sizeof(synthetic.finish_reason), LLM_FINISH_TOOL);
					append_assistant_turn(conv, &synthetic);
					llm_response_free(resp);
					resp = NULL;

					if (iterations - last_textfallback_reminder_iter >= REMINDER_COOLDOWN_ITERS) {
						inject_reminder(conv,
								"You emitted tool calls as text rather than "
								"using the native tool_calls channel. The "
								"native channel is preferred \xe2\x80\x94 it "
								"is more reliable and avoids parsing "
								"ambiguity.");
						last_textfallback_reminder_iter = iterations;
					}

					for (int ti = 0; ti < nc; ti++) {
						struct tool_call *tc = &text_calls.calls[ti];
						log_info("agent: [text-fallback] tool=%s id=%s",
							 tc->name, tc->id);
						struct tool_result result_tc = tools_dispatch(tc, cfg);
						metrics_record_tool_call();
						consecutive_tool_errors = result_tc.is_error
										  ? consecutive_tool_errors + 1
										  : 0;
						append_tool_result(conv, &result_tc);
						tool_result_free(&result_tc);
					}
					consecutive_toolcall_turns++;
					continue;
				}
			}
			append_assistant_turn(conv, resp);
			result = make_result(FINISHED_NATURALLY,
					     resp->content,
					     iterations,
					     total_prompt_tok,
					     total_completion_tok);
			goto cleanup;
		}

		/* TOOL_CALLS: dispatch requested tools */
		if (strcmp(resp->finish_reason, LLM_FINISH_TOOL) == 0 ||
		    resp->tool_calls.count > 0) {
			append_assistant_turn(conv, resp);
			if (resp->tool_calls.count == 0) {
				log_warning("agent: finish_reason=tool_calls but no calls");
				result = make_result(FINISHED_NATURALLY,
						     resp->content,
						     iterations,
						     total_prompt_tok,
						     total_completion_tok);
				goto cleanup;
			}

			uint64_t this_turn_fp = 0;
			for (size_t ti = 0; ti < resp->tool_calls.count; ti++)
				this_turn_fp ^= tool_call_fingerprint(
					&resp->tool_calls.calls[ti]);

			bool repeated_calls = (last_tool_fingerprint != 0 &&
					       this_turn_fp == last_tool_fingerprint);
			last_tool_fingerprint = this_turn_fp;

			for (size_t ti = 0; ti < resp->tool_calls.count; ti++) {
				struct tool_call *tc = &resp->tool_calls.calls[ti];
				log_info("agent: dispatching tool '%s' (id=%s)",
					 tc->name, tc->id);
				struct tool_result result_tc = tools_dispatch(tc, cfg);
				metrics_record_tool_call();

				consecutive_tool_errors = result_tc.is_error
								  ? consecutive_tool_errors + 1
								  : 0;

				log_debug("agent: tool '%s' %s: %.120s",
					  tc->name,
					  result_tc.is_error ? "ERROR" : "OK",
					  result_tc.content ? result_tc.content
							    : "(null)");
				append_tool_result(conv, &result_tc);
				tool_result_free(&result_tc);
			}
			llm_response_free(resp);
			resp = NULL;
			consecutive_toolcall_turns++;

			if (consecutive_tool_errors >= 3 &&
			    iterations - last_error_reminder_iter >= REMINDER_COOLDOWN_ITERS) {
				inject_reminder(conv,
						"3 consecutive tool calls have failed. Stop and "
						"rethink the approach before issuing more calls. "
						"Re-read the error messages, verify assumptions "
						"(file paths, argument shapes, required state), "
						"and consider whether a different tool is needed.");
				last_error_reminder_iter = iterations;
				consecutive_tool_errors = 0;
			} else if (repeated_calls &&
				   iterations - last_repeat_reminder_iter >= REMINDER_COOLDOWN_ITERS) {
				inject_reminder(conv,
						"You just issued the same tool call(s) as the "
						"previous turn. Either the previous result already "
						"answers your question (in which case use it) or "
						"the call is wrong and repeating it won't change "
						"the outcome (in which case adjust your approach).");
				last_repeat_reminder_iter = iterations;
			} else if (consecutive_toolcall_turns >= DRIFT_STUCK_THRESHOLD &&
				   iterations - last_drift_reminder_iter >= REMINDER_COOLDOWN_ITERS) {
				inject_reminder(conv,
						"You have been executing tools for several turns "
						"without summarising findings. Pause: state what "
						"you have learned so far, what still needs to be "
						"done, and whether the current approach is working. "
						"Then continue.");
				last_drift_reminder_iter = iterations;
				consecutive_toolcall_turns = 0;
			}
			continue;
		}

		/* Any non-tool_calls turn resets the drift counter. */
		consecutive_toolcall_turns = 0;

		/* LENGTH: either max_tokens response cap or context window full */
		if (strcmp(resp->finish_reason, LLM_FINISH_LENGTH) == 0) {
			append_assistant_turn(conv, resp);
			llm_response_free(resp);
			resp = NULL;
			size_t estimated = conversation_estimate_tokens(conv);
			size_t target = (size_t)(cfg->context_window * 3 / 4);
			if (estimated > target && conv->count > 3) {
				log_warning("agent: finish_reason=length - context full, truncating");
				conversation_truncate(conv, target);

				if (iterations - last_truncation_reminder_iter >= REMINDER_COOLDOWN_ITERS) {
					inject_reminder(conv,
							"The conversation was truncated to free context "
							"space. Earlier tool results and messages may no "
							"longer be visible. If you need information from "
							"before the truncation, re-fetch it via tools "
							"rather than relying on memory.");
					last_truncation_reminder_iter = iterations;
				}
				continue;
			}

			if (continuation_count < MAX_CONTINUATIONS) {
				continuation_count++;
				log_warning("agent: finish_reason=length - hit max_tokens (%d), "
					    "injecting continuation %d/%d",
					    cfg->max_tokens, continuation_count,
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
				"model kept hitting the max_tokens limit (%d) after %d "
				"continuations \xe2\x80\x94 increase max_tokens in your "
				"config file",
				cfg->max_tokens, MAX_CONTINUATIONS);
			goto cleanup;
		}

		continuation_count = 0;

		/* Unknown finish_reason - be lenient */
		log_warning("agent: unexpected finish_reason '%s'", resp->finish_reason);
		if (resp->content && resp->content[0]) {
			append_assistant_turn(conv, resp);
			result = make_result(FINISHED_NATURALLY,
					     resp->content,
					     iterations,
					     total_prompt_tok,
					     total_completion_tok);
			goto cleanup;
		}
		{
			char saved_reason[32];
			snprintf(saved_reason, sizeof(saved_reason), "%s",
				 resp->finish_reason);
			result = make_error_result("unexpected finish_reason: %s",
						   saved_reason);
			goto cleanup;
		}
	}

	log_warning("agent: hit max_iterations (%d)", cfg->max_iterations);
	result = make_result(FINISHED_MAX_ITERATIONS,
			     last_response[0] ? last_response : NULL,
			     iterations, total_prompt_tok, total_completion_tok);

cleanup:
	sigaction(SIGINT, &sa_old, NULL);
	if (resp)
		llm_response_free(resp);
	arena_destroy(scratch);
	free(last_response);
	return result;
}

struct agent_result agent_run(const char *user_message,
			      const struct agent_configuration *config)
{
	if (!user_message || !config) {
		return make_error_result("agent_run: NULL argument");
	}

	struct conversation_history *conv = conversation_create();
	if (!conv) {
		return make_error_result("agent_run: out of memory");
	}

	if (config->system_prompt && config->system_prompt[0]) {
		if (conversation_add_system(conv, config->system_prompt) != BORIS_OK) {
			conversation_destroy(conv);
			return make_error_result("agent_run: failed to add system prompt");
		}
	}

	if (conversation_add_user(conv, user_message) != BORIS_OK) {
		conversation_destroy(conv);
		return make_error_result("agent_run: failed to add user message");
	}

	struct agent_result r = run_loop(conv, config);
	conversation_destroy(conv);
	return r;
}

struct agent_result agent_run_conv(struct conversation_history *conv,
				   const struct agent_configuration *config)
{
	if (!conv || !config) {
		return make_error_result("agent_run_conv: NULL argument");
	}
	if (conv->count == 0) {
		return make_error_result("agent_run_conv: empty conversation");
	}

	int last_user_found = 0;
	for (int i = (int)conv->count - 1; i >= 0; i--) {
		if (conv->messages[i].role == MESSAGE_ROLE_SYSTEM)
			continue;
		if (conv->messages[i].role == MESSAGE_ROLE_USER)
			last_user_found = 1;
		break;
	}
	if (!last_user_found) {
		return make_error_result("agent_run_conv: last message must be USER");
	}

	return run_loop(conv, config);
}

void agent_result_free(struct agent_result *r)
{
	if (!r)
		return;
	free(r->final_response);
	r->final_response = NULL;
}
