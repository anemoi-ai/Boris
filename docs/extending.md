# Adding a New Tool to Boris

This is the entry point for the Module 16 capstone and for contributors. It walks through adding a new built-in tool from scratch.

For context on how tools fit into the overall system, see [architecture.md](architecture.md).

---

## Overview

Each tool is:

1. A C function with a fixed signature in `src/tools/`
2. A JSON Schema string describing its parameters
3. A registration call in `tools_register_builtins()`
4. A bitmask flag in `include/boris_types.h`
5. A set of tests in `tests/test_tools.c`

The model sees the name, description, and schema. When it calls the tool, `tools_dispatch()` validates the bitmask, calls your function with the raw argument JSON, and returns your `tool_result` back into the conversation.

---

## Step 1 — Add a bitmask flag

Open `include/boris_types.h` and add a flag for your tool. The existing flags are:

```c
#define TOOL_READ        (1 << 0)
#define TOOL_WRITE       (1 << 1)
#define TOOL_BASH        (1 << 2)   /* reserved */
#define TOOL_WEB         (1 << 3)   /* reserved */
#define TOOL_LIST_DIR    (1 << 4)
#define TOOL_MEMORY      (1 << 5)
#define TOOL_RUN         (1 << 6)
#define TOOL_ALL         (0x7F)
```

Add yours at the next available bit:

```c
#define TOOL_MYTOOL      (1 << 7)
#define TOOL_ALL         (0xFF)     /* update this too */
```

Also add your tool name to `tools_mask_to_string()` in `src/tools.c` so it appears correctly in `/tools` and `boris status`:

```c
static const struct { unsigned int flag; const char *name; } kv[] = {
    { TOOL_READ,     "read"     },
    /* ... */
    { TOOL_RUN,      "run"      },
    { TOOL_MYTOOL,   "mytool"   },  /* add here */
};
```

---

## Step 2 — Create `src/tools/mytool.c`

Every tool file follows the same pattern. Here is a minimal working example:

```c
/*
 * tools/mytool.c - One-line description of what this tool does.
 */
#define _POSIX_C_SOURCE 200809L

#include "tools.h"
#include "arena.h"
#include "configuration.h"
#include "json.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>

struct tool_result tool_mytool_fn(const char *arguments_json,
				  const struct agent_configuration *cfg,
				  struct memory_arena *scratch)
{
	(void)scratch;  /* remove if you use the arena */

	if (!arguments_json)
		return tool_result_errorf("mytool", "Missing arguments");

	/* Parse arguments */
	cJSON *root = json_parse(arguments_json);
	if (!root)
		return tool_result_errorf("mytool", "Invalid JSON arguments");

	const char *input = json_get_str(root, "input");
	if (!input) {
		cJSON_Delete(root);
		return tool_result_errorf("mytool", "Missing required argument: input");
	}

	/* Do the work */
	char *result = /* ... heap-allocated result string ... */;

	cJSON_Delete(root);

	/* tool_result_ok() takes ownership of result — do not free it */
	return tool_result_ok("mytool", result);
}
```

**Function signature rules:**
- Name: `tool_<name>_fn`
- Parameters: always `(const char *arguments_json, const struct agent_configuration *cfg, struct memory_arena *scratch)`
- Return: always `struct tool_result`
- On success: return `tool_result_ok("mytool", heap_string)` — transfers ownership
- On error: return `tool_result_errorf("mytool", "message %s", ...)` — formats and copies

**If your tool accesses the filesystem**, validate the path through the sandbox first:

```c
#include "sandbox.h"
#include <limits.h>

REQUIRE_SANDBOX(cfg);    /* returns an error result if sandbox_root is unset */

char resolved[PATH_MAX];
if (sandbox_resolve(cfg->sandbox_root, user_path,
                    resolved, sizeof(resolved)) != 0)
    return tool_result_errorf("mytool",
        "Access denied: '%s' is outside the sandbox", user_path);

/* Now use resolved, not user_path */
```

---

## Step 3 — Write the JSON Schema

The schema is a JSON Schema object string that describes your tool's parameters. The model uses this to know what arguments to provide.

```c
/* In src/tools.c, alongside the other SCHEMA_* constants */
static const char SCHEMA_MYTOOL[] =
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
      "\"input\":{"
        "\"type\":\"string\","
        "\"description\":\"The input to process.\""
      "},"
      "\"count\":{"
        "\"type\":\"integer\","
        "\"description\":\"How many results to return.\","
        "\"default\":1"
      "}"
    "},"
    "\"required\":[\"input\"]"
    "}";
```

Keep descriptions precise — they are part of every LLM request.

---

## Step 4 — Register the tool

Open `src/tools.c`. Add a forward declaration at the top alongside the others:

```c
struct tool_result tool_mytool_fn(const char *arguments_json,
				  const struct agent_configuration *cfg,
				  struct memory_arena *scratch);
```

Then add a `register_one()` call inside `tools_register_builtins()`:

```c
void tools_register_builtins(void)
{
    register_one("read",   "...", SCHEMA_READ,   tool_read_fn,   TOOL_READ);
    /* ... existing tools ... */
    register_one("mytool",
        "One sentence describing what this tool does and when to use it.",
        SCHEMA_MYTOOL, tool_mytool_fn, TOOL_MYTOOL);
}
```

The description is shown to the model. Be specific: describe the primary use case, not just the name.

---

## Step 5 — Add to the Makefile

The Makefile discovers tool sources via a wildcard, so no Makefile change is needed — `src/tools/mytool.c` will be compiled automatically on the next `make`.

---

## Step 6 — Write tests

Open `tests/test_tools.c`. Add a forward declaration at the top:

```c
struct tool_result tool_mytool_fn(const char *arguments_json,
				  const struct agent_configuration *cfg,
				  struct memory_arena *scratch);
```

Add test functions in a new section:

```c
/* -------------------------------------------------------------------------
 * Mytool
 * ---------------------------------------------------------------------- */

static void test_mytool_basic(void)
{
    printf("  Test: mytool - basic usage\n");

    struct agent_configuration cfg = make_cfg();
    struct tool_result r = tool_mytool_fn(
        "{\"input\":\"hello\"}", &cfg, g_scratch);

    ASSERT(r.is_error == false);
    ASSERT(r.content != NULL);
    if (r.content)
        ASSERT(strstr(r.content, "expected_output") != NULL);

    tool_result_free(&r);
    configuration_destroy(&cfg);
}

static void test_mytool_missing_arg(void)
{
    printf("  Test: mytool - missing required arg returns error\n");

    struct agent_configuration cfg = make_cfg();
    struct tool_result r = tool_mytool_fn("{}", &cfg, g_scratch);

    ASSERT(r.is_error == true);

    tool_result_free(&r);
    configuration_destroy(&cfg);
}
```

Cover at minimum: the happy path, missing required arguments, and any sandbox or permission checks your tool performs.

Call your tests from `main()` in the same file:

```c
printf("\n--- Mytool ---\n");
test_mytool_basic();
test_mytool_missing_arg();
```

Run with `make test` to verify.

---

## Step 7 — Enable by default (optional)

If your tool should be enabled by default, add it to the `tools.enabled` line in `config/boris.ini.example`:

```ini
[tools]
enabled = read,write,list_dir,memory,run,mytool
```

If it requires configuration (API keys, endpoints, etc.), add the relevant keys to `boris.ini.example` with comments explaining what they do.

---

## Step 8 — Wire up `--tools` and env var

The `--tools` CLI flag and `BORIS_TOOLS` environment variable both go through `configuration_parse_tools_mask()` in `src/configuration.c`. Add your tool name there:

```c
else if (strcmp(tok, "mytool") == 0)  mask |= TOOL_MYTOOL;
```

---

## Checklist

- [ ] `TOOL_MYTOOL` flag added to `boris_types.h`, `TOOL_ALL` updated
- [ ] Flag name added to `tools_mask_to_string()` in `tools.c`
- [ ] `src/tools/mytool.c` created with correct function signature
- [ ] `SCHEMA_MYTOOL` written in `tools.c`
- [ ] Forward declaration added to `tools.c`
- [ ] `register_one()` call added to `tools_register_builtins()`
- [ ] Forward declaration added to `tests/test_tools.c`
- [ ] Tests written and passing under `make test`
- [ ] `configuration_parse_tools_mask()` updated for `--tools`/`BORIS_TOOLS`
- [ ] `boris.ini.example` updated if needed
