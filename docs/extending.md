# Adding a New Tool to Boris

This is the entry point for the Module 16 capstone and for contributors. It walks through adding a new built-in tool from scratch.

For context on how tools fit into the overall system, see [architecture.md](architecture.md).

---

## Overview

Each built-in tool lives in one file, `src/tools/<name>.c`, which holds:

1. the tool function (fixed signature),
2. a formatter that summarises a call in one line,
3. a JSON Schema string for its parameters,
4. one `const struct tool_definition` tying them together with a description, a flag and a confirmation class.

Outside that file you touch three lines: a flag in `include/boris_types.h`, a declaration in `include/tool_builtins.h`, and an entry in the `BUILTIN_TOOLS` list in `src/tools.c`. Everything else is derived from that list: `--tools` / `tools.enabled` parsing, `boris status`, `/tools`, confirmation prompts and registration.

The model sees the name, description and schema. When it calls the tool, `tools_dispatch()` checks the flag is enabled, asks for confirmation if the tool's class requires it, calls your function with the raw argument JSON, caps the result at `tools.max_result_bytes`, and returns it to the conversation.

---

## Step 1 — Add a flag

Open `include/boris_types.h`. Add a flag at the next free bit and include it in `TOOL_ALL`:

```c
#define TOOL_FETCH    (1u << 10)
#define TOOL_MYTOOL   (1u << 11)   /* new */
#define TOOL_ALL                                                     \
	(TOOL_READ | TOOL_WRITE | ... | TOOL_FETCH | TOOL_MYTOOL)
```

`TOOL_DEFAULT` (what `tools.enabled = default` means) is `TOOL_ALL` without `fetch`. If your tool should be off unless asked for, remove it from `TOOL_DEFAULT` too.

A unit test (`test_builtin_definitions`) fails if two tools share a flag or if `TOOL_ALL` misses one.

---

## Step 2 — Create `src/tools/mytool.c`

A minimal working example:

```c
/*
 * tools/mytool.c - One-line description of what this tool does.
 */
#define _POSIX_C_SOURCE 200809L

#include "tools.h"
#include "tool_builtins.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct tool_result tool_mytool_fn(const char *arguments_json,
				  const struct agent_configuration *cfg,
				  struct memory_arena *scratch)
{
	(void)cfg;
	(void)scratch;

	cJSON *root = json_parse(arguments_json ? arguments_json : "");
	const char *input = json_get_str(root, "input");
	struct tool_result r;
	if (!root)
		r = tool_result_errorf("mytool", "Could not parse arguments as JSON");
	else if (!input)
		r = tool_result_errorf("mytool", "\"input\" is required");
	else
		r = tool_result_ok_copy("mytool", input); /* do the real work here */
	cJSON_Delete(root);
	return r;
}

/* "hello". Display only; never fails. */
void tool_mytool_format(const char *argument_json, char *buf, size_t size)
{
	cJSON *root = json_parse(argument_json);
	const char *input = json_get_str(root, "input");
	snprintf(buf, size, "%s", input ? input : "(invalid arguments)");
	cJSON_Delete(root);
}

/* -------------------------------------------------------------------------
 * Definition
 * ---------------------------------------------------------------------- */

static const char SCHEMA_MYTOOL[] =
	"{"
	"\"type\":\"object\","
	"\"properties\":{"
	"\"input\":{\"type\":\"string\","
	"\"description\":\"The input to process.\"}"
	"},"
	"\"required\":[\"input\"]"
	"}";

const struct tool_definition tool_mytool_definition = {
	.name = "mytool",
	.description = "One or two sentences: what it does and when to use it.",
	.parameters_schema = SCHEMA_MYTOOL,
	.fn = tool_mytool_fn,
	.format_call = tool_mytool_format,
	.flag = TOOL_MYTOOL,
	.confirm = TOOL_CONFIRM_NEVER,
};
```

**Function rules**
- Name it `tool_<name>_fn`; the signature is fixed.
- On success return `tool_result_ok("mytool", heap_string)` (takes ownership) or `tool_result_ok_copy()`.
- On error return `tool_result_errorf("mytool", "message %s", ...)`. Write errors the model can act on: say what was wrong and what to do instead.
- You do not need to limit output size for the context window; dispatch does that. Do limit work (files read, bytes downloaded, time spent) and check `tools_cancelled()` in long loops so Ctrl+C stops the tool.

**Formatter rules**
- One short line; the tool name is printed separately, so leave it out.
- Read argument strings in place. Never copy large values (file content) just to describe them; report a length instead, like `write` and `edit` do.
- No escaping needed: the registry runs the result through `term_sanitize()`.

**Description and schema**
These are sent with every request, so keep them short and precise. They are also part of the prompt prefix that providers cache: changing them later invalidates every cached conversation.

**Confirmation class** (`confirm`)

| Value | Asks first when |
|---|---|
| `TOOL_CONFIRM_NEVER` | never |
| `TOOL_CONFIRM_WRITES` | `behaviour.confirm_writes` is on (write, edit, move, delete) |
| `TOOL_CONFIRM_RUN` | `behaviour.confirm_run` is on |
| `TOOL_CONFIRM_FETCH` | `behaviour.confirm_fetch` is on (the default) |

### Filesystem access

Validate every path against the sandbox before touching it, and use only the resolved path afterwards:

```c
#include <limits.h>

char resolved[PATH_MAX];
struct tool_result err;
if (tools_sandbox_resolve(cfg, user_path, resolved, sizeof(resolved), &err) != 0)
	return err;   /* "Access denied: '...' is outside the sandbox" */
```

`tools_sandbox_resolve()` follows symlinks, which is right for reading and writing *content*. For operations on the directory entry itself (renaming, deleting) use `tools_sandbox_resolve_entry()`: it resolves the parent directory and appends the final name, so a symlink names the link rather than its target, and it refuses `.`, `..` and the sandbox root. When walking a directory tree, use `lstat()` and never follow symlinks (see `search.c` and `delete.c`).

---

## Step 3 — Declare and list it

In `include/tool_builtins.h`:

```c
DECLARE_BUILTIN_TOOL(mytool);
```

In `src/tools.c`, add it to `BUILTIN_TOOLS` where it should appear in the model's tool list:

```c
static const struct tool_definition *const BUILTIN_TOOLS[] = {
	&tool_read_definition,
	/* ... */
	&tool_mytool_definition,
};
```

The Makefile picks up `src/tools/*.c` with a wildcard, so no build change is needed.

Tools can also be registered at runtime with `tools_register()`, which takes the same `struct tool_definition`.

---

## Step 4 — Write tests

`tests/test_tools.c` includes `tool_builtins.h`, so `tool_mytool_fn` is already declared. Add a section:

```c
static void test_mytool_basic(void)
{
	printf("  Test: mytool - basic usage\n");

	struct agent_configuration cfg = make_cfg();
	struct tool_result r = tool_mytool_fn("{\"input\":\"hello\"}", &cfg, g_scratch);
	ASSERT(!r.is_error);
	ASSERT(r.content && strstr(r.content, "hello"));
	tool_result_free(&r);

	r = tool_mytool_fn("{}", &cfg, g_scratch);
	ASSERT(r.is_error);
	tool_result_free(&r);
	configuration_destroy(&cfg);
}
```

and call it from `main()`. Cover the happy path, missing arguments, and every sandbox or permission check the tool makes. `put_file()`, `get_file()` and `exists_in_sandbox()` help with file fixtures.

---

## Step 5 — Configuration (optional)

If the tool needs settings, add fields to `struct agent_configuration`, defaults in `configuration_set_defaults()`, keys in `configuration_set_key()` (and `configuration_destroy()` for strings), and document them in `config/boris.ini.example`.

---

## Checklist

- [ ] `TOOL_MYTOOL` flag in `boris_types.h`, added to `TOOL_ALL` (and to `TOOL_DEFAULT` unless it should be opt-in)
- [ ] `src/tools/mytool.c` with the function, formatter, schema and `tool_mytool_definition`
- [ ] `DECLARE_BUILTIN_TOOL(mytool)` in `tool_builtins.h`
- [ ] Entry in `BUILTIN_TOOLS` in `src/tools.c`
- [ ] Tests in `tests/test_tools.c`, passing under `make test`
- [ ] `boris.ini.example` updated if the tool has settings
