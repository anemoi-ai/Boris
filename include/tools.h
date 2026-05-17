/*
 * tools.h - Tool registry and dispatcher.
 *
 * Tools are registered at startup via tools_register_builtins() and
 * dispatched by name via tools_dispatch(). The tools_enabled bitmask in
 * agent_configuration controls which tools are active at runtime.
 */

#ifndef TOOLS_H
#define TOOLS_H

#include <stddef.h>
#include "boris_types.h"

struct cJSON;

#define REQUIRE_SANDBOX(cfg)                                                      	\
	do {                                                                      		\
		if (!(cfg)->sandbox_root || !(cfg)->sandbox_root[0])              			\
			return tool_result_errorf("sandbox",                      				\
						  "Sandbox root not configured"); 							\
	} while (0)

/* -------------------------------------------------------------------------
 * Registry API
 * ---------------------------------------------------------------------- */

/*
 * Register all built-in tools.
 *
 * Call once at startup before agent_run(). This registers read, write,
 * list_dir, memory, and run tools.
 */
void tools_register_builtins(void);

/*
 * Register a custom tool.
 *
 * The struct is copied; the caller retains ownership of pointer fields in the
 * original. Returns 0 on success, -1 if the registry is full or name is taken.
 */
int tools_register(struct tool_definition t);

/*
 * Return a read-only pointer to the full registry array and its length.
 *
 * Used by llm.c to build the tool-schema section of the LLM request.
 */
const struct tool_definition *tools_get_all(size_t *out_count);

/*
 * Dispatch a single tool call against the registry.
 *
 * Looks up the tool by name, verifies it is enabled in cfg->tools_enabled,
 * and calls its function. If the tool is unknown or disabled, returns a
 * tool_result with is_error = 1 and an explanatory message.
 *
 * The returned tool_result.tool_call_id is populated from call->id.
 * The caller must call tool_result_free() on the result.
 */
struct tool_result tools_dispatch(const struct tool_call *call,
				  const struct agent_configuration *cfg);

/* -------------------------------------------------------------------------
 * Memory management
 * ---------------------------------------------------------------------- */

/* Free the content string inside a tool_result. Does not free the struct. */
void tool_result_free(struct tool_result *r);

/* Free all heap members of a tool_call_list. Does not free the struct. */
void tool_call_list_free(struct tool_call_list *list);

/* -------------------------------------------------------------------------
 * Convenience constructors (used by tool implementations)
 * ---------------------------------------------------------------------- */

/*
 * Build a success tool_result from a heap-allocated string.
 *
 * Takes ownership of content_heap - do not free it after calling this.
 * The tool_call_id is stamped by tools_dispatch(), not by the tool itself.
 */
struct tool_result tool_result_ok(const char *tool_name, char *content_heap);

/*
 * Build a success tool_result by copying a string.
 *
 * Use this when your content is a stack buffer or literal.
 */
struct tool_result tool_result_ok_copy(const char *tool_name,
				       const char *content);

/*
 * Build a formatted error tool_result.
 *
 * Like printf, but returns a tool_result with is_error = 1.
 */
struct tool_result tool_result_errorf(const char *tool_name,
				      const char *fmt, ...);

/*
 * Convert a tools_enabled bitmask to a human-readable comma-separated string.
 *
 * Writes "none", "all", or "read,write,..." into buf. buf is always
 * NUL-terminated.
 */
void tools_mask_to_string(unsigned int mask, char *buf, size_t size);

/*
 * Validate path against the sandbox and resolve it to an absolute path.
 *
 * Checks that cfg->sandbox_root is set, then calls sandbox_resolve().
 * On success returns 0 and fills resolved. On failure returns -1 and
 * writes a ready-to-return tool_result into *err_out.
 */
int tools_sandbox_resolve(const struct agent_configuration *cfg,
			  const char *path, char *resolved, size_t size,
			  struct tool_result *err_out);

#endif /* TOOLS_H */
