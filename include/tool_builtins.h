/*
 * tool_builtins.h - The built-in tools.
 *
 * Each tool lives in src/tools/<name>.c and exports its definition and its
 * functions. tools.c lists the definitions in BUILTIN_TOOLS; tests call the
 * functions directly.
 *
 * Adding a tool:
 *   1. a TOOL_* flag in boris_types.h (and add it to TOOL_ALL)
 *   2. src/tools/<name>.c with the function, formatter and definition
 *   3. its declarations below and one line in BUILTIN_TOOLS (src/tools.c)
 */

#ifndef TOOL_BUILTINS_H
#define TOOL_BUILTINS_H

#include <stddef.h>
#include "boris_types.h"

#define DECLARE_BUILTIN_TOOL(name)                                              \
	extern const struct tool_definition tool_##name##_definition;          \
	struct tool_result tool_##name##_fn(const char *arguments_json,        \
					    const struct agent_configuration *cfg, \
					    struct memory_arena *scratch);     \
	void tool_##name##_format(const char *argument_json, char *buf, size_t size)

DECLARE_BUILTIN_TOOL(read);
DECLARE_BUILTIN_TOOL(write);
DECLARE_BUILTIN_TOOL(list_dir);
DECLARE_BUILTIN_TOOL(memory);
DECLARE_BUILTIN_TOOL(run);

/* The built-in definitions in registration order, and how many there are. */
const struct tool_definition *const *tools_builtins(size_t *out_count);

#endif /* TOOL_BUILTINS_H */
