/*
 * tools/plan.c - The model's own checklist for multi-step tasks.
 *
 * "update" replaces the whole plan; "get" returns it. Each call returns the
 * rendered checklist, so the latest plan is always the most recent tool
 * result. The plan is kept here, outside the conversation history, so it
 * survives history truncation; tool_plan_reset() clears it (the REPL does
 * so on /clear and /reset).
 */

#define _POSIX_C_SOURCE 200809L

#include "tools.h"
#include "tool_builtins.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLAN_MAX_STEPS 30
#define PLAN_STEP_MAX  240

enum step_status { STEP_PENDING, STEP_IN_PROGRESS, STEP_DONE };

static const char *const STATUS_NAMES[] = {"pending", "in_progress", "done"};
static const char *const STATUS_MARKS[] = {"[ ]", "[>]", "[x]"};

struct plan_step {
	char text[PLAN_STEP_MAX];
	enum step_status status;
};

static struct plan_step g_steps[PLAN_MAX_STEPS];
static int g_step_count = 0;

void tool_plan_reset(void)
{
	g_step_count = 0;
}

static int parse_status(const char *s)
{
	if (!s)
		return STEP_PENDING;
	for (int i = 0; i < 3; i++)
		if (strcmp(s, STATUS_NAMES[i]) == 0)
			return i;
	return -1;
}

static char *render_plan(void)
{
	if (g_step_count == 0)
		return strdup("No plan yet. Use action=update with a list of steps.");

	int done = 0;
	for (int i = 0; i < g_step_count; i++)
		done += g_steps[i].status == STEP_DONE;

	size_t cap = 64 + (size_t)g_step_count * (PLAN_STEP_MAX + 16);
	char *out = malloc(cap);
	if (!out)
		return NULL;
	size_t pos = (size_t)snprintf(out, cap, "Plan (%d of %d done):\n", done, g_step_count);
	for (int i = 0; i < g_step_count && pos < cap; i++)
		pos += (size_t)snprintf(out + pos, cap - pos, "%s %d. %s\n",
					STATUS_MARKS[g_steps[i].status], i + 1,
					g_steps[i].text);
	return out;
}

/* Validate every step before replacing the plan, so a bad call changes nothing. */
static struct tool_result update_plan(const cJSON *steps)
{
	if (!cJSON_IsArray(steps))
		return tool_result_errorf("plan", "\"steps\" must be an array of "
						  "{\"step\": text, \"status\": pending|in_progress|done}");
	int n = cJSON_GetArraySize(steps);
	if (n > PLAN_MAX_STEPS)
		return tool_result_errorf("plan", "A plan can have at most %d steps; "
						  "group smaller steps together", PLAN_MAX_STEPS);

	struct plan_step next[PLAN_MAX_STEPS];
	int i = 0;
	const cJSON *item = NULL;
	cJSON_ArrayForEach(item, steps)
	{
		const char *text = cJSON_IsString(item) ? item->valuestring
							: json_get_str(item, "step");
		if (!text || !text[0])
			return tool_result_errorf("plan", "Step %d has no text", i + 1);
		int status = parse_status(cJSON_IsString(item) ? NULL
							       : json_get_str(item, "status"));
		if (status < 0)
			return tool_result_errorf("plan", "Step %d: status must be pending, "
							  "in_progress or done", i + 1);
		snprintf(next[i].text, sizeof(next[i].text), "%s", text);
		next[i].status = (enum step_status)status;
		i++;
	}

	memcpy(g_steps, next, sizeof(next[0]) * (size_t)n);
	g_step_count = n;
	char *rendered = render_plan();
	return rendered ? tool_result_ok("plan", rendered)
			: tool_result_errorf("plan", "Out of memory");
}

struct tool_result tool_plan_fn(const char *arguments_json,
				const struct agent_configuration *cfg,
				struct memory_arena *scratch)
{
	(void)cfg;
	(void)scratch;
	cJSON *root = json_parse(arguments_json ? arguments_json : "");
	const char *action = json_get_str(root, "action");
	struct tool_result r;
	if (!root) {
		r = tool_result_errorf("plan", "Could not parse arguments as JSON");
	} else if (action && strcmp(action, "get") == 0) {
		char *rendered = render_plan();
		r = rendered ? tool_result_ok("plan", rendered)
			     : tool_result_errorf("plan", "Out of memory");
	} else if (action && strcmp(action, "update") == 0) {
		r = update_plan(cJSON_GetObjectItemCaseSensitive(root, "steps"));
	} else {
		r = tool_result_errorf("plan", "\"action\" must be update or get");
	}
	cJSON_Delete(root);
	return r;
}

/* "update (5 steps, 2 done)" or "get". */
void tool_plan_format(const char *argument_json, char *buf, size_t size)
{
	cJSON *root = json_parse(argument_json);
	const char *action = json_get_str(root, "action");
	const cJSON *steps = cJSON_GetObjectItemCaseSensitive(root, "steps");
	if (action && strcmp(action, "update") == 0 && cJSON_IsArray(steps)) {
		int done = 0;
		const cJSON *item = NULL;
		cJSON_ArrayForEach(item, steps)
		{
			const char *st = json_get_str(item, "status");
			done += st && strcmp(st, "done") == 0;
		}
		snprintf(buf, size, "update (%d steps, %d done)", cJSON_GetArraySize(steps),
			 done);
	} else {
		snprintf(buf, size, "%s", action ? action : "(invalid arguments)");
	}
	cJSON_Delete(root);
}

/* -------------------------------------------------------------------------
 * Definition
 * ---------------------------------------------------------------------- */

static const char SCHEMA_PLAN[] =
	"{"
	"\"type\":\"object\","
	"\"properties\":{"
	"\"action\":{\"type\":\"string\","
	"\"enum\":[\"update\",\"get\"]},"
	"\"steps\":{\"type\":\"array\","
	"\"description\":\"update only: the whole plan, replacing the previous one.\","
	"\"items\":{\"type\":\"object\","
	"\"properties\":{"
	"\"step\":{\"type\":\"string\"},"
	"\"status\":{\"type\":\"string\",\"enum\":[\"pending\",\"in_progress\",\"done\"]}"
	"},"
	"\"required\":[\"step\",\"status\"]}}"
	"},"
	"\"required\":[\"action\"]"
	"}";

const struct tool_definition tool_plan_definition = {
	.name = "plan",
	.description = "Keep a checklist for a task with several steps. Call update "
		       "with every step and its status when you start, and again as "
		       "steps finish; call get if you lose track. Skip it for simple "
		       "one-step requests.",
	.parameters_schema = SCHEMA_PLAN,
	.fn = tool_plan_fn,
	.format_call = tool_plan_format,
	.flag = TOOL_PLAN,
	.confirm = TOOL_CONFIRM_NEVER,
};
