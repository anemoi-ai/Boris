/*
 * tools.c - Tool registry and dispatcher.
 *
 * All built-in tool implementations are forward-declared here and called
 * via the tool_fn function pointer stored in the registry. The registry is
 * a static array of tool_definition structs; it is not thread-safe but is
 * only written at startup before any agent loop begins.
 *
 * Parameter schemas are stored as static string literals. They are copied
 * into tool_definition.parameters_schema during registration so each tool
 * entry is self-contained and the registry can be queried at any time.
 */

#define _POSIX_C_SOURCE 200809L

#include "tools.h"
#include "sandbox.h"
#include "arena.h"
#include "boris_errors.h"
#include "logger.h"
#include "json.h"
#include "terminal.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* -------------------------------------------------------------------------
 * Forward declarations - implemented in tools/read.c, write.c, etc.
 * ---------------------------------------------------------------------- */

struct tool_result tool_read_fn(const char *arguments_json,
				const struct agent_configuration *cfg,
				struct memory_arena *scratch);
struct tool_result tool_write_fn(const char *arguments_json,
				 const struct agent_configuration *cfg,
				 struct memory_arena *scratch);
struct tool_result tool_list_dir_fn(const char *arguments_json,
				    const struct agent_configuration *cfg,
				    struct memory_arena *scratch);
struct tool_result tool_memory_fn(const char *arguments_json,
				  const struct agent_configuration *cfg,
				  struct memory_arena *scratch);
struct tool_result tool_run_fn(const char *arguments_json,
			       const struct agent_configuration *cfg,
			       struct memory_arena *scratch);

void tool_read_format(const char *argument_json, char *buf, size_t size);
void tool_write_format(const char *argument_json, char *buf, size_t size);
void tool_list_dir_format(const char *argument_json, char *buf, size_t size);
void tool_memory_format(const char *argument_json, char *buf, size_t size);
void tool_run_format(const char *argument_json, char *buf, size_t size);

/* -------------------------------------------------------------------------
 * Parameter schemas (JSON Schema objects)
 * ---------------------------------------------------------------------- */

static const char SCHEMA_READ[] =
	"{"
	"\"type\":\"object\","
	"\"properties\":{"
	"\"path\":{\"type\":\"string\","
	"\"description\":\"File path to read. Relative paths are resolved within the sandbox root.\"},"
	"\"offset\":{\"type\":\"integer\","
	"\"description\":\"Byte offset to start reading from.\","
	"\"default\":0},"
	"\"length\":{\"type\":\"integer\","
	"\"description\":\"Maximum bytes to read (capped at 131072).\","
	"\"default\":65536}"
	"},"
	"\"required\":[\"path\"]"
	"}";

static const char SCHEMA_WRITE[] =
	"{"
	"\"type\":\"object\","
	"\"properties\":{"
	"\"path\":{\"type\":\"string\","
	"\"description\":\"Destination file path.\"},"
	"\"content\":{\"type\":\"string\","
	"\"description\":\"Content to write.\"},"
	"\"mode\":{\"type\":\"string\","
	"\"enum\":[\"overwrite\",\"append\",\"create_new\"],"
	"\"default\":\"overwrite\","
	"\"description\":\"Write mode. create_new fails if the file exists.\"},"
	"\"create_dirs\":{\"type\":\"boolean\","
	"\"default\":false,"
	"\"description\":\"Create parent directories if they do not exist.\"}"
	"},"
	"\"required\":[\"path\",\"content\"]"
	"}";

static const char SCHEMA_LIST_DIR[] =
	"{"
	"\"type\":\"object\","
	"\"properties\":{"
	"\"path\":{\"type\":\"string\","
	"\"description\":\"Directory path to list.\"},"
	"\"show_hidden\":{\"type\":\"boolean\","
	"\"default\":false,"
	"\"description\":\"Include hidden files and directories.\"}"
	"},"
	"\"required\":[\"path\"]"
	"}";

static const char SCHEMA_RUN[] =
	"{"
	"\"type\":\"object\","
	"\"properties\":{"
	"\"path\":{\"type\":\"string\","
	"\"description\":\"Path to the script file to execute. "
	"Relative paths are resolved within the sandbox root.\"},"
	"\"args\":{\"type\":\"array\","
	"\"items\":{\"type\":\"string\"},"
	"\"description\":\"Command-line arguments to pass to the script.\","
	"\"default\":[]},"
	"\"stdin\":{\"type\":\"string\","
	"\"description\":\"Data to send to the process on stdin.\","
	"\"default\":\"\"},"
	"\"timeout_seconds\":{\"type\":\"integer\","
	"\"description\":\"Maximum execution time in seconds (1-120).\","
	"\"default\":30}"
	"},"
	"\"required\":[\"path\"]"
	"}";

static const char SCHEMA_MEMORY[] =
	"{"
	"\"type\":\"object\","
	"\"properties\":{"
	"\"action\":{\"type\":\"string\","
	"\"enum\":[\"get\",\"set\",\"delete\",\"list\"],"
	"\"description\":\"Operation to perform on the key-value store.\"},"
	"\"key\":{\"type\":\"string\","
	"\"description\":\"Key name, case-sensitive (required for get, set, delete).\"},"
	"\"value\":{\"type\":\"string\","
	"\"description\":\"Value to store (required for set).\"},"
	"\"prefix\":{\"type\":\"string\","
	"\"description\":\"list only: return keys starting with this prefix.\"},"
	"\"limit\":{\"type\":\"integer\","
	"\"description\":\"list only: maximum entries to return (1-500).\","
	"\"default\":50}"
	"},"
	"\"required\":[\"action\"]"
	"}";


/* -------------------------------------------------------------------------
 * Bitmask utilities
 * ---------------------------------------------------------------------- */

/* The one table mapping tool names to TOOL_* flags. */
static const struct {
	unsigned int flag;
	const char *name;
} TOOL_FLAG_NAMES[] = {
	{TOOL_READ, "read"},
	{TOOL_WRITE, "write"},
	{TOOL_LIST_DIR, "list_dir"},
	{TOOL_MEMORY, "memory"},
	{TOOL_RUN, "run"},
};

#define TOOL_FLAG_NAMES_COUNT (sizeof(TOOL_FLAG_NAMES) / sizeof(TOOL_FLAG_NAMES[0]))

unsigned int tools_flag_from_name(const char *name)
{
	if (!name)
		return 0;
	for (size_t i = 0; i < TOOL_FLAG_NAMES_COUNT; i++)
		if (strcmp(name, TOOL_FLAG_NAMES[i].name) == 0)
			return TOOL_FLAG_NAMES[i].flag;
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
	for (size_t i = 0; i < TOOL_FLAG_NAMES_COUNT && pos < size - 1; i++) {
		if (!(mask & TOOL_FLAG_NAMES[i].flag))
			continue;
		int n = snprintf(buf + pos, size - pos, "%s%s",
				 pos ? "," : "", TOOL_FLAG_NAMES[i].name);
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

/*
 * register_one - internal helper used by tools_register_builtins().
 * Already-registered names are skipped quietly so the call is idempotent.
 */
static void register_one(const char *name,
			 const char *description,
			 const char *schema_json,
			 tool_fn fn,
			 tool_format_fn format_call,
			 int flag)
{
	if (find_tool(name))
		return;

	struct tool_definition t;
	memset(&t, 0, sizeof(t));
	snprintf(t.name, sizeof(t.name), "%s", name);
	snprintf(t.description, sizeof(t.description), "%s", description);
	t.parameters_schema = (char *)schema_json; /* copied by tools_register */
	t.fn = fn;
	t.format_call = format_call;
	t.flag = flag;
	tools_register(t);
}

void tools_register_builtins(void)
{
	register_one("read",
		     "Read the contents of a file within the sandbox. "
		     "Use offset and length to page through large files.",
		     SCHEMA_READ, tool_read_fn, tool_read_format, TOOL_READ);

	register_one("write",
		     "Write or append content to a file within the sandbox. "
		     "Use mode=create_new to avoid overwriting existing files. "
		     "For large files (e.g. long HTML or source code), write in "
		     "multiple smaller chunks: first call with mode=overwrite to "
		     "create the file, then mode=append for each subsequent chunk. "
		     "This avoids JSON encoding errors from embedding large strings.",
		     SCHEMA_WRITE, tool_write_fn, tool_write_format, TOOL_WRITE);

	register_one("list_dir",
		     "List the contents of a directory within the sandbox, "
		     "formatted showing names, types and sizes.",
		     SCHEMA_LIST_DIR, tool_list_dir_fn, tool_list_dir_format, TOOL_LIST_DIR);

	register_one("memory",
		     "Persistent key-value store. Use to remember facts across turns. "
		     "Actions: get, set, delete, list.",
		     SCHEMA_MEMORY, tool_memory_fn, tool_memory_format, TOOL_MEMORY);

	register_one("run",
		     "Execute a script file within the sandbox and return its output. "
		     "The interpreter is detected from the shebang line or file extension "
		     "(.py, .js, .sh, .rb, .lua, .pl, .php). "
		     "Returns the exit code and combined stdout/stderr.",
		     SCHEMA_RUN, tool_run_fn, tool_run_format, TOOL_RUN);
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
	return ((t->flag & TOOL_WRITE) && cfg->confirm_writes) ||
	       ((t->flag & TOOL_RUN) && cfg->confirm_bash);
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
	} else if (!(cfg->tools_enabled & (unsigned int)t->flag)) {
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
