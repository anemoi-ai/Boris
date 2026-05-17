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
	"\"description\":\"Key name (required for get, set, delete).\"},"
	"\"value\":{\"type\":\"string\","
	"\"description\":\"Value to store (required for set).\"}"
	"},"
	"\"required\":[\"action\"]"
	"}";

/* -------------------------------------------------------------------------
 * Bitmask utilities
 * ---------------------------------------------------------------------- */

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

	static const struct {
		unsigned int flag;
		const char *name;
	} kv[] = {
		{TOOL_READ, "read"},
		{TOOL_WRITE, "write"},
		{TOOL_LIST_DIR, "list_dir"},
		{TOOL_MEMORY, "memory"},
		{TOOL_RUN, "run"},
	};

	size_t pos = 0;
	int first = 1;
	for (size_t i = 0; i < sizeof(kv) / sizeof(kv[0]); i++) {
		if (!(mask & kv[i].flag))
			continue;
		int n = snprintf(buf + pos, size - pos,
				 "%s%s", first ? "" : ",", kv[i].name);
		if (n > 0)
			pos += (size_t)n;
		first = 0;
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
	return tool_result_ok(tool_name, content ? strdup(content) : strdup(""));
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
	list->cap = 0;
}

/* -------------------------------------------------------------------------
 * Registry API
 * ---------------------------------------------------------------------- */

/*
 * register_one - internal helper used by tools_register_builtins().
 * Copies the tool_definition struct and duplicates the schema string.
 */
static void register_one(const char *name,
			 const char *description,
			 const char *schema_json,
			 tool_fn fn,
			 int flag)
{
	if (g_registry_count >= TOOLS_REGISTRY_MAX) {
		log_error("tools: registry full, cannot register '%s'", name);
		return;
	}

	/* Check for duplicate names */
	for (size_t i = 0; i < g_registry_count; i++) {
		if (strcmp(g_registry[i].name, name) == 0) {
			log_warning("tools: '%s' already registered, skipping", name);
			return;
		}
	}

	struct tool_definition *t = &g_registry[g_registry_count++];
	memset(t, 0, sizeof(*t));
	snprintf(t->name, sizeof(t->name), "%s", name);
	snprintf(t->description, sizeof(t->description), "%s", description);
	t->parameters_schema = schema_json ? strdup(schema_json) : NULL;
	t->fn = fn;
	t->flag = flag;

	log_debug("tools: registered '%s' (flag=0x%02x)", name, flag);
}

void tools_register_builtins(void)
{
	register_one("read",
		     "Read the contents of a file within the sandbox. "
		     "Use offset and length to page through large files.",
		     SCHEMA_READ, tool_read_fn, TOOL_READ);

	register_one("write",
		     "Write or append content to a file within the sandbox. "
		     "Use mode=create_new to avoid overwriting existing files. "
		     "For large files (e.g. long HTML or source code), write in "
		     "multiple smaller chunks: first call with mode=overwrite to "
		     "create the file, then mode=append for each subsequent chunk. "
		     "This avoids JSON encoding errors from embedding large strings.",
		     SCHEMA_WRITE, tool_write_fn, TOOL_WRITE);

	register_one("list_dir",
		     "List the contents of a directory within the sandbox, "
		     "formatted showing names, types and sizes.",
		     SCHEMA_LIST_DIR, tool_list_dir_fn, TOOL_LIST_DIR);

	register_one("memory",
		     "Persistent key-value store. Use to remember facts across turns. "
		     "Actions: get, set, delete, list.",
		     SCHEMA_MEMORY, tool_memory_fn, TOOL_MEMORY);

	register_one("run",
		     "Execute a script file within the sandbox and return its output. "
		     "The interpreter is detected from the shebang line or file extension "
		     "(.py, .js, .sh, .rb, .lua, .pl, .php). "
		     "Returns the exit code and combined stdout/stderr.",
		     SCHEMA_RUN, tool_run_fn, TOOL_RUN);
}

int tools_register(struct tool_definition t)
{
	if (g_registry_count >= TOOLS_REGISTRY_MAX) {
		log_error("tools: registry full");
		return -1;
	}
	for (size_t i = 0; i < g_registry_count; i++) {
		if (strcmp(g_registry[i].name, t.name) == 0) {
			log_error("tools: '%s' already registered", t.name);
			return -1;
		}
	}

	struct tool_definition *slot = &g_registry[g_registry_count++];
	*slot = t;
	/* Duplicate the schema string so this entry is self-contained */
	if (t.parameters_schema) {
		slot->parameters_schema = strdup(t.parameters_schema);
	}

	log_debug("tools: custom tool '%s' registered (flag=0x%02x)",
		  t.name, t.flag);
	return 0;
}

const struct tool_definition *tools_get_all(size_t *out_count)
{
	if (out_count)
		*out_count = g_registry_count;
	return g_registry;
}

struct tool_result tools_dispatch(const struct tool_call *call,
				  const struct agent_configuration *cfg)
{
	struct tool_result result;

	if (!call || !cfg)
		return tool_result_errorf("(unknown)",
					  "tools_dispatch: NULL argument");

	log_debug("tools: dispatch '%s' (id=%s)", call->name, call->id);

	/* Find the tool in the registry */
	for (size_t i = 0; i < g_registry_count; i++) {
		if (strcmp(g_registry[i].name, call->name) != 0)
			continue;

		if (!(cfg->tools_enabled & g_registry[i].flag)) {
			result = tool_result_errorf(call->name,
						    "Tool '%s' is not enabled. "
						    "Enable it with --tools %s or in the config file.",
						    call->name, call->name);
		} else {
			struct memory_arena *scratch = arena_create(32768);
			if (!scratch) {
				result = tool_result_errorf(call->name, "Out of memory");
				snprintf(result.tool_call_id,
					 sizeof(result.tool_call_id),
					 "%s", call->id);
				snprintf(result.tool_name,
					 sizeof(result.tool_name),
					 "%.*s", (int)sizeof(result.tool_name) - 1,
					 call->name);
				return result;
			}
			const char *args = call->argument_json ? call->argument_json : "{}";
			result = g_registry[i].fn(args, cfg, scratch);
			arena_destroy(scratch);
			log_debug("tools: '%s' %s (content_len=%zu)",
				  call->name,
				  result.is_error ? "ERROR" : "OK",
				  result.content ? strlen(result.content) : 0);
		}

		snprintf(result.tool_call_id, sizeof(result.tool_call_id),
			 "%s", call->id);
		snprintf(result.tool_name, sizeof(result.tool_name),
			 "%.*s", (int)sizeof(result.tool_name) - 1, call->name);
		return result;
	}

	/* Tool not found in registry */
	char available[256];
	size_t pos = 0;
	for (size_t i = 0; i < g_registry_count; i++) {
		int n = snprintf(available + pos, sizeof(available) - pos,
				 "%s%s", i > 0 ? ", " : "", g_registry[i].name);
		if (n > 0)
			pos += (size_t)n;
		if (pos >= sizeof(available) - 1)
			break;
	}
	result = tool_result_errorf(call->name,
				    "Unknown tool '%s'. Available tools: %s.",
				    call->name, available);
	snprintf(result.tool_call_id, sizeof(result.tool_call_id),
		 "%s", call->id);
	snprintf(result.tool_name, sizeof(result.tool_name),
		 "%.*s", (int)sizeof(result.tool_name) - 1, call->name);
	return result;
}
