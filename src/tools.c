/*
 * tools.c - Tool registry and dispatcher.
 *
 * Each built-in tool lives in src/tools/<name>.c and exports one
 * const struct tool_definition (name, description, schema, function,
 * formatter, flag, confirmation class); see tool_builtins.h. BUILTIN_TOOLS
 * below lists them. The registry is a static array of tool_definition
 * structs; it is not thread-safe but is only written at startup before any
 * agent loop begins. Schemas are copied at registration so each entry is
 * self-contained.
 */

#define _POSIX_C_SOURCE 200809L

#include "tools.h"
#include "tool_builtins.h"
#include "sandbox.h"
#include "arena.h"
#include "boris_errors.h"
#include "logger.h"
#include "json.h"
#include "terminal.h"
#include "platform.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* -------------------------------------------------------------------------
 * Built-in tools and bitmask utilities
 * ---------------------------------------------------------------------- */

/* The built-in tools, in the order they are offered to the model. This list
 * is the single source for names, flags and schemas: --tools parsing,
 * tools_mask_to_string() and tools_register_builtins() all derive from it. */
static const struct tool_definition *const BUILTIN_TOOLS[] = {
	&tool_read_definition,
	&tool_write_definition,
	&tool_edit_definition,
	&tool_list_dir_definition,
	&tool_search_definition,
	&tool_move_definition,
	&tool_delete_definition,
	&tool_memory_definition,
	&tool_run_definition,
	&tool_plan_definition,
	&tool_fetch_definition,
};

#define BUILTIN_TOOLS_COUNT (sizeof(BUILTIN_TOOLS) / sizeof(BUILTIN_TOOLS[0]))

const struct tool_definition *const *tools_builtins(size_t *out_count)
{
	if (out_count)
		*out_count = BUILTIN_TOOLS_COUNT;
	return BUILTIN_TOOLS;
}

unsigned int tools_flag_from_name(const char *name)
{
	if (!name)
		return 0;
	for (size_t i = 0; i < BUILTIN_TOOLS_COUNT; i++)
		if (strcmp(name, BUILTIN_TOOLS[i]->name) == 0)
			return BUILTIN_TOOLS[i]->flag;
	return 0;
}

void tools_mask_to_string(unsigned int mask, char *buf, size_t size)
{
	if (!buf || size == 0)
		return;
	if (mask == TOOL_NONE) {
		snprintf(buf, size, "none");
		return;
	}
	if (mask == TOOL_ALL) {
		snprintf(buf, size, "all");
		return;
	}

	size_t pos = 0;
	buf[0] = '\0';
	for (size_t i = 0; i < BUILTIN_TOOLS_COUNT && pos < size - 1; i++) {
		if (!(mask & BUILTIN_TOOLS[i]->flag))
			continue;
		int n = snprintf(buf + pos, size - pos, "%s%s",
				 pos ? "," : "", BUILTIN_TOOLS[i]->name);
		if (n < 0)
			break;
		/* On truncation snprintf returns the untruncated length;
		 * clamp so pos never runs past the buffer. */
		pos += (size_t)n < size - pos ? (size_t)n : size - pos - 1;
	}
	if (pos == 0)
		snprintf(buf, size, "none");
}

int tools_sandbox_resolve(const struct agent_configuration *cfg,
			  const char *path, char *resolved, size_t size,
			  struct tool_result *err_out)
{
	if (!cfg->sandbox_root || !cfg->sandbox_root[0]) {
		*err_out = tool_result_errorf("sandbox",
					      "Sandbox root not configured");
		return -1;
	}
	if (sandbox_resolve(cfg->sandbox_root, path, resolved, size) != 0) {
		*err_out = tool_result_errorf("sandbox",
					      "Access denied: '%s' is outside the sandbox",
					      path);
		return -1;
	}
	return 0;
}

int tools_sandbox_resolve_entry(const struct agent_configuration *cfg,
				const char *path, char *resolved, size_t size,
				struct tool_result *err_out)
{
	char buf[PATH_MAX];
	if (!path || !path[0] || strlen(path) >= sizeof(buf)) {
		*err_out = tool_result_errorf("sandbox", "Invalid path");
		return -1;
	}
	snprintf(buf, sizeof(buf), "%s", path);
	platform_path_normalize(buf);
	size_t len = strlen(buf);
	while (len > 1 && buf[len - 1] == '/')
		buf[--len] = '\0';

	const char *dir = ".";
	const char *base = buf;
	char *slash = strrchr(buf, '/');
	if (slash == buf) {
		dir = "/";
		base = buf + 1;
	} else if (slash) {
		*slash = '\0';
		dir = buf;
		base = slash + 1;
	}
	if (!base[0] || strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
		*err_out = tool_result_errorf("sandbox",
					      "'%s' does not name a file or directory "
					      "inside the sandbox",
					      path);
		return -1;
	}

	char parent[PATH_MAX];
	if (tools_sandbox_resolve(cfg, dir, parent, sizeof(parent), err_out) != 0) {
		tool_result_free(err_out);
		*err_out = tool_result_errorf("sandbox",
					      "Access denied: '%s' is outside the sandbox",
					      path);
		return -1;
	}
	int n = snprintf(resolved, size, "%s/%s",
			 strcmp(parent, "/") == 0 ? "" : parent, base);
	if (n < 0 || (size_t)n >= size) {
		*err_out = tool_result_errorf("sandbox", "Path too long: %s", path);
		return -1;
	}
	return 0;
}

/* -------------------------------------------------------------------------
 * Registry
 * ---------------------------------------------------------------------- */

static struct tool_definition g_registry[TOOLS_REGISTRY_MAX];
static size_t g_registry_count = 0;

static struct tool_observer g_observer; /* zeroed: no callbacks */

/* -------------------------------------------------------------------------
 * Convenience constructors
 * ---------------------------------------------------------------------- */

struct tool_result tool_result_ok(const char *tool_name, char *content_heap)
{
	struct tool_result r;
	memset(&r, 0, sizeof(r));
	snprintf(r.tool_name, sizeof(r.tool_name), "%s",
		 tool_name ? tool_name : "");
	r.content = content_heap;
	r.is_error = false;
	return r;
}

struct tool_result tool_result_ok_copy(const char *tool_name,
				       const char *content)
{
	return tool_result_ok(tool_name, strdup(content ? content : ""));
}

struct tool_result tool_result_errorf(const char *tool_name,
				      const char *fmt, ...)
{
	struct tool_result r;
	memset(&r, 0, sizeof(r));
	snprintf(r.tool_name, sizeof(r.tool_name), "%s",
		 tool_name ? tool_name : "");
	r.is_error = true;

	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	r.content = strdup(buf);
	return r;
}

/* -------------------------------------------------------------------------
 * Memory management
 * ---------------------------------------------------------------------- */

void tool_result_free(struct tool_result *r)
{
	if (!r)
		return;
	free(r->content);
	r->content = NULL;
}

void tool_call_list_free(struct tool_call_list *list)
{
	if (!list)
		return;
	if (list->calls) {
		for (size_t i = 0; i < list->count; i++) {
			free(list->calls[i].argument_json);
			list->calls[i].argument_json = NULL;
		}
		free(list->calls);
		list->calls = NULL;
	}
	list->count = 0;
}

/* -------------------------------------------------------------------------
 * Registry API
 * ---------------------------------------------------------------------- */

static const struct tool_definition *find_tool(const char *name)
{
	for (size_t i = 0; i < g_registry_count; i++)
		if (strcmp(g_registry[i].name, name) == 0)
			return &g_registry[i];
	return NULL;
}

int tools_register(struct tool_definition t)
{
	if (!t.name[0] || !t.fn) {
		log_error("tools: refusing to register a tool without name or function");
		return -1;
	}
	if (g_registry_count >= TOOLS_REGISTRY_MAX) {
		log_error("tools: registry full, cannot register '%s'", t.name);
		return -1;
	}
	if (find_tool(t.name)) {
		log_error("tools: '%s' already registered", t.name);
		return -1;
	}

	struct tool_definition *slot = &g_registry[g_registry_count];
	*slot = t;
	/* Duplicate the schema string so this entry is self-contained */
	if (t.parameters_schema) {
		slot->parameters_schema = strdup(t.parameters_schema);
		if (!slot->parameters_schema)
			return -1;
	}
	g_registry_count++;

	log_debug("tools: registered '%s' (flag=0x%02x)", t.name, t.flag);
	return 0;
}

/* Register every built-in tool. Already-registered names are skipped
 * quietly so the call is idempotent. */
void tools_register_builtins(void)
{
	for (size_t i = 0; i < BUILTIN_TOOLS_COUNT; i++)
		if (!find_tool(BUILTIN_TOOLS[i]->name))
			tools_register(*BUILTIN_TOOLS[i]);
}

const struct tool_definition *tools_get_all(size_t *out_count)
{
	if (out_count)
		*out_count = g_registry_count;
	return g_registry;
}

void tools_set_observer(const struct tool_observer *observer)
{
	if (observer)
		g_observer = *observer;
	else
		memset(&g_observer, 0, sizeof(g_observer));
}

/* Longest summary passed to observers (they may shorten it further). */
#define TOOL_SUMMARY_MAX 256

void tools_format_call(const struct tool_call *call, char *buf, size_t size)
{
	if (!buf || size == 0)
		return;
	buf[0] = '\0';
	if (!call)
		return;

	const char *args = call->argument_json ? call->argument_json : "{}";
	const struct tool_definition *t = find_tool(call->name);
	char raw[TOOL_SUMMARY_MAX * 2];

	if (t && t->format_call) {
		raw[0] = '\0';
		t->format_call(args, raw, sizeof(raw));
		term_sanitize(raw, buf, size);
	} else {
		term_sanitize(args, buf, size);
	}
}

static bool needs_confirmation(const struct tool_definition *t,
			       const struct agent_configuration *cfg)
{
	switch (t->confirm) {
	case TOOL_CONFIRM_WRITES:
		return cfg->confirm_writes;
	case TOOL_CONFIRM_RUN:
		return cfg->confirm_bash;
	case TOOL_CONFIRM_FETCH:
		return cfg->confirm_fetch;
	case TOOL_CONFIRM_NEVER:
	default:
		return false;
	}
}

static struct tool_result unknown_tool_result(const char *name)
{
	char available[256];
	size_t pos = 0;
	available[0] = '\0';
	for (size_t i = 0; i < g_registry_count && pos < sizeof(available) - 1; i++) {
		int n = snprintf(available + pos, sizeof(available) - pos,
				 "%s%s", i > 0 ? ", " : "", g_registry[i].name);
		if (n < 0)
			break;
		pos += (size_t)n;
	}
	return tool_result_errorf(name, "Unknown tool '%s'. Available tools: %s.",
				  name, available);
}

static struct tool_result run_tool(const struct tool_definition *t,
				   const struct tool_call *call,
				   const struct agent_configuration *cfg)
{
	struct memory_arena *scratch = arena_create(32768);
	if (!scratch)
		return tool_result_errorf(call->name, "Out of memory");

	const char *args = call->argument_json ? call->argument_json : "{}";
	struct tool_result result = t->fn(args, cfg, scratch);
	arena_destroy(scratch);
	log_debug("tools: '%s' %s (content_len=%zu)", call->name,
		  result.is_error ? "ERROR" : "OK",
		  result.content ? strlen(result.content) : 0);
	return result;
}

/* Shortest cap honoured, so the note always has some output before it. */
#define TOOL_RESULT_MIN_CAP 256

/*
 * Cut r->content to at most limit bytes (on a UTF-8 character boundary)
 * and append a note saying so. Every tool result passes through here, so
 * no tool can flood the context window, whatever it forgets to limit.
 */
static void cap_result(struct tool_result *r, size_t limit)
{
	if (!r->content)
		return;
	size_t len = strlen(r->content);
	if (len <= limit)
		return;

	size_t cut = limit;
	while (cut > 0 && ((unsigned char)r->content[cut] & 0xC0) == 0x80)
		cut--; /* do not split a multi-byte character */

	char note[192];
	int n = snprintf(note, sizeof(note),
			 "\n[Output truncated: showing %zu of %zu bytes. Request "
			 "a smaller range or narrow the query.]",
			 cut, len);
	if (n < 0)
		n = 0;
	char *capped = realloc(r->content, cut + (size_t)n + 1);
	if (!capped) {
		r->content[cut] = '\0'; /* still bounded, just without the note */
		return;
	}
	memcpy(capped + cut, note, (size_t)n + 1);
	r->content = capped;
	log_debug("tools: '%s' result capped at %zu of %zu bytes",
		  r->tool_name, cut, len);
}

/* Keys shorter than this are placeholders ("none", "ollama"), not secrets;
 * redacting them would only mangle ordinary output. */
#define SECRET_MIN_LEN 8
#define SECRET_MASK    "[REDACTED: API key]"

/*
 * Replace every occurrence of Boris's own API key in a tool result. A run
 * script is not confined to the sandbox and can read ~/.boris/config.ini
 * (or anything else holding the key); whatever it prints must not reach
 * the model, the session file or a trace.
 */
static void redact_api_key(struct tool_result *r, const char *key)
{
	if (!r->content || !key)
		return;
	size_t klen = strlen(key);
	if (klen < SECRET_MIN_LEN || !strstr(r->content, key))
		return;

	size_t mlen = strlen(SECRET_MASK), count = 0;
	for (const char *p = strstr(r->content, key); p; p = strstr(p + klen, key))
		count++;
	size_t len = strlen(r->content);
	char *out = malloc(len - count * klen + count * mlen + 1);
	if (!out) {
		/* Cannot build the redacted copy: drop the output entirely. */
		free(r->content);
		r->content = strdup("[output withheld: it contained the API key]");
		return;
	}
	const char *src = r->content;
	char *dst = out;
	for (const char *hit = strstr(src, key); hit; hit = strstr(src, key)) {
		memcpy(dst, src, (size_t)(hit - src));
		dst += hit - src;
		memcpy(dst, SECRET_MASK, mlen);
		dst += mlen;
		src = hit + klen;
	}
	strcpy(dst, src);
	free(r->content);
	r->content = out;
	log_warning("tools: '%s' output contained the API key; redacted %zu time%s",
		    r->tool_name, count, count == 1 ? "" : "s");
}

struct tool_result tools_dispatch(const struct tool_call *call,
				  const struct agent_configuration *cfg)
{
	if (!call || !cfg)
		return tool_result_errorf("(unknown)",
					  "tools_dispatch: NULL argument");

	log_debug("tools: dispatch '%s' (id=%s)", call->name, call->id);

	const struct tool_definition *t = find_tool(call->name);
	struct tool_result result;

	/* Only built when someone will see it */
	char summary[TOOL_SUMMARY_MAX] = "";
	if (g_observer.confirm || g_observer.on_result)
		tools_format_call(call, summary, sizeof(summary));

	if (!t) {
		result = unknown_tool_result(call->name);
	} else if (!(cfg->tools_enabled & t->flag)) {
		result = tool_result_errorf(call->name,
					    "Tool '%s' is not enabled. "
					    "Enable it with --tools %s or in the config file.",
					    call->name, call->name);
	} else if (needs_confirmation(t, cfg) &&
		   !(g_observer.confirm &&
		     g_observer.confirm(call, summary, g_observer.ctx))) {
		result = tool_result_errorf(call->name,
					    "The user declined this '%s' call. Do not "
					    "retry it unchanged; ask the user or take a "
					    "different approach.",
					    call->name);
	} else {
		result = run_tool(t, call, cfg);
	}

	/* Link the result back to its call */
	snprintf(result.tool_call_id, sizeof(result.tool_call_id), "%s", call->id);
	snprintf(result.tool_name, sizeof(result.tool_name), "%.*s",
		 (int)sizeof(result.tool_name) - 1, call->name);

	redact_api_key(&result, cfg->api_key);

	size_t limit = cfg->max_result_bytes > 0 ? (size_t)cfg->max_result_bytes
						 : TOOL_RESULT_MAX_DEFAULT;
	cap_result(&result, limit < TOOL_RESULT_MIN_CAP ? TOOL_RESULT_MIN_CAP : limit);

	if (g_observer.on_result)
		g_observer.on_result(call, summary, &result, g_observer.ctx);
	return result;
}

int tools_arg_path(const struct cJSON *args, char *out, size_t size)
{
	const char *path = json_get_str(args, "path");
	if (!path || strlen(path) >= size)
		return -1;
	memcpy(out, path, strlen(path) + 1);
	return 0;
}

static const volatile sig_atomic_t *g_cancel_flag = NULL;

void tools_set_cancel_flag(const volatile sig_atomic_t *flag)
{
	g_cancel_flag = flag;
}

bool tools_cancelled(void)
{
	return g_cancel_flag && *g_cancel_flag;
}
