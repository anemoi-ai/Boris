/*
 * exec.c - Non-interactive mode: run one task and report the result.
 *
 * Used by `boris exec` and by the eval harness, which reads the --json
 * report to see which tools a run used. Everything is written to the
 * streams in exec_options, so tests can capture it.
 */

#define _POSIX_C_SOURCE 200809L

#include "exec.h"
#include "agent.h"
#include "cJSON.h"
#include "conversation.h"
#include "tools.h"
#include <string.h>
#include "platform.h"

struct exec_state {
	const struct exec_options *opts;
	cJSON *calls; /* array of {name, summary, is_error} */
};

/* Nobody can answer a confirmation prompt: refuse, and say how to allow it. */
static bool exec_confirm(const struct tool_call *call, const char *summary, void *ctx)
{
	struct exec_state *st = ctx;
	(void)summary;
	fprintf(st->opts->err,
		"boris exec: refused '%s': it needs confirmation, which only "
		"'boris chat' can ask for. Turn the matching behaviour.confirm_* "
		"setting off to allow it here.\n",
		call->name);
	return false;
}

static void exec_on_result(const struct tool_call *call, const char *summary,
			   const struct tool_result *result, void *ctx)
{
	struct exec_state *st = ctx;
	cJSON *entry = cJSON_CreateObject();
	if (entry) {
		cJSON_AddStringToObject(entry, "name", call->name);
		cJSON_AddStringToObject(entry, "summary", summary ? summary : "");
		cJSON_AddBoolToObject(entry, "is_error", result->is_error);
		cJSON_AddItemToArray(st->calls, entry);
	}
	if (st->opts->show_tools)
		fprintf(st->opts->err, "[%s] %s%s\n", call->name, summary ? summary : "",
			result->is_error ? "  (error)" : "");
}

static const char *finish_name(enum finish_reason r)
{
	switch (r) {
	case FINISHED_NATURALLY:
		return "natural";
	case FINISHED_MAX_ITERATIONS:
		return "max_iterations";
	case FINISHED_INTERRUPTED:
		return "interrupted";
	case FINISHED_ERROR:
	default:
		return "error";
	}
}

static int exit_status(enum finish_reason r)
{
	switch (r) {
	case FINISHED_NATURALLY:
		return EXEC_EXIT_OK;
	case FINISHED_MAX_ITERATIONS:
		return EXEC_EXIT_MAX_ITERATIONS;
	case FINISHED_INTERRUPTED:
		return EXEC_EXIT_INTERRUPTED;
	case FINISHED_ERROR:
	default:
		return EXEC_EXIT_ERROR;
	}
}

static void print_json(const struct exec_options *opts, const struct agent_result *r,
		       cJSON *calls)
{
	bool failed = r->finish_reason == FINISHED_ERROR;
	cJSON *root = cJSON_CreateObject();
	if (!root)
		return;
	if (!failed && r->final_response)
		cJSON_AddStringToObject(root, "response", r->final_response);
	else
		cJSON_AddNullToObject(root, "response");
	cJSON_AddStringToObject(root, "finish_reason", finish_name(r->finish_reason));
	cJSON_AddNumberToObject(root, "iterations", r->turns_used);
	cJSON *usage = cJSON_AddObjectToObject(root, "usage");
	if (usage) {
		cJSON_AddNumberToObject(usage, "prompt_tokens", r->total_prompt_tokens);
		cJSON_AddNumberToObject(usage, "completion_tokens", r->total_completion_tokens);
	}
	cJSON_AddItemReferenceToObject(root, "tool_calls", calls);
	if (failed)
		cJSON_AddStringToObject(root, "error",
					r->final_response ? r->final_response : "unknown error");
	else
		cJSON_AddNullToObject(root, "error");

	char *text = cJSON_PrintUnformatted(root);
	if (text) {
		fprintf(opts->out, "%s\n", text);
		cJSON_free(text);
	}
	cJSON_Delete(root);
}

struct conversation_history *exec_conversation(const char *session_path,
					       const char *system_prompt,
					       const char *task, FILE *err)
{
	struct conversation_history *conv = NULL;
	if (session_path && session_path[0] && access(session_path, F_OK) == 0) {
		conv = conversation_load(session_path);
		if (!conv) {
			fprintf(err, "boris exec: cannot load session %s\n", session_path);
			return NULL;
		}
		if (conv->count > 0 && conv->messages[0].role == MESSAGE_ROLE_SYSTEM)
			conversation_replace_system_prompt(conv, system_prompt);
	} else {
		conv = conversation_create();
		if (!conv || conversation_add_system(conv, system_prompt) != BORIS_OK) {
			conversation_destroy(conv);
			fprintf(err, "boris exec: out of memory\n");
			return NULL;
		}
	}
	if (conversation_add_user(conv, task) != BORIS_OK) {
		conversation_destroy(conv);
		fprintf(err, "boris exec: out of memory\n");
		return NULL;
	}
	return conv;
}

int exec_run(struct conversation_history *conv, const struct agent_configuration *cfg,
	     const struct exec_options *opts)
{
	struct exec_state st = {.opts = opts, .calls = cJSON_CreateArray()};
	if (!st.calls) {
		fprintf(opts->err, "boris exec: out of memory\n");
		return EXEC_EXIT_ERROR;
	}
	struct tool_observer observer = {
		.confirm = exec_confirm,
		.on_result = exec_on_result,
		.ctx = &st,
	};
	tools_set_observer(&observer);

	struct agent_result r = agent_run_conv(conv, cfg);
	tools_set_observer(NULL);

	if (opts->json) {
		print_json(opts, &r, st.calls);
	} else if (r.finish_reason == FINISHED_ERROR) {
		fprintf(opts->err, "boris exec: %s\n",
			r.final_response ? r.final_response : "unknown error");
	} else {
		if (r.final_response)
			fprintf(opts->out, "%s\n", r.final_response);
		if (r.finish_reason == FINISHED_MAX_ITERATIONS)
			fprintf(opts->err, "boris exec: stopped after %d iterations "
					   "(agent.max_iterations)\n",
				r.turns_used);
		else if (r.finish_reason == FINISHED_INTERRUPTED)
			fprintf(opts->err, "boris exec: interrupted\n");
	}
	fflush(opts->out);

	int status = exit_status(r.finish_reason);
	agent_result_free(&r);
	cJSON_Delete(st.calls);
	return status;
}
