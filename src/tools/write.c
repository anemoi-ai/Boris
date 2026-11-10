/*
 * tools/write.c - Write file contents within the sandbox.
 *
 * Supports overwrite, append, and create_new modes.
 * Optionally creates parent directories.
 */

#define _POSIX_C_SOURCE 200809L

#include "tools.h"
#include "tool_builtins.h"
#include "arena.h"
#include "configuration.h"
#include "fs.h"
#include "json.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include "platform.h"

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

#define WRITE_MAX_CONTENT 131072

enum write_mode {
	WRITE_OVERWRITE,
	WRITE_APPEND,
	WRITE_CREATE_NEW,
};

struct write_args {
	char path[4096];
	char *content; /* scratch-arena copy */
	enum write_mode mode;
	int create_dirs;
};

/* Returns NULL on success, or a message describing the problem. */
static const char *parse_write_args(const char *json, struct write_args *args,
				    struct memory_arena *scratch)
{
	memset(args, 0, sizeof(*args));
	args->mode = WRITE_OVERWRITE;

	cJSON *root = json_parse(json);
	if (!root)
		return "Could not parse arguments as JSON. If the content "
		       "contains double quotes (e.g. HTML attributes), every "
		       "quote inside the content string must be escaped as \\\" "
		       "in the JSON. Consider writing the file in smaller chunks "
		       "to reduce encoding complexity.";

	const char *err = NULL;
	const char *content = json_get_str(root, "content");
	const char *mode = json_get_str(root, "mode");

	if (tools_arg_path(root, args->path, sizeof(args->path)) != 0)
		err = "\"path\" is required";
	else if (!content)
		err = "\"content\" is required";
	else if (strlen(content) > WRITE_MAX_CONTENT)
		err = "Content too large (max 131072 bytes); write in chunks with mode=append";
	else if (!(args->content = arena_duplicate_string(scratch, content)))
		err = "Out of memory";
	else if (!mode || strcmp(mode, "overwrite") == 0)
		args->mode = WRITE_OVERWRITE;
	else if (strcmp(mode, "append") == 0)
		args->mode = WRITE_APPEND;
	else if (strcmp(mode, "create_new") == 0)
		args->mode = WRITE_CREATE_NEW;
	else
		err = "\"mode\" must be overwrite, append or create_new";

	args->create_dirs = json_get_bool(root, "create_dirs", 0);
	cJSON_Delete(root);
	return err;
}

/* Create the parent directory of resolved, then re-check the sandbox:
 * once the directories exist, symlinks or ".." in the new part resolve
 * for real. Returns 0, or -1 with *err set. */
static int create_parent_dirs(const struct agent_configuration *cfg,
			      const struct write_args *args,
			      const char *resolved, struct tool_result *err)
{
	if (fs_mkdirp_parent(resolved, 0755) != 0) {
		*err = tool_result_errorf("write", "Failed to create parent directories");
		return -1;
	}

	char recheck[PATH_MAX];
	if (tools_sandbox_resolve(cfg, args->path, recheck, sizeof(recheck), err) != 0)
		return -1;
	if (strcmp(recheck, resolved) != 0) {
		*err = tool_result_errorf("sandbox",
					  "Access denied: '%s' is outside the sandbox",
					  args->path);
		return -1;
	}
	return 0;
}

static struct tool_result write_success(const char *verb, size_t bytes,
					const char *resolved)
{
	char *msg = malloc(64 + PATH_MAX);
	if (!msg)
		return tool_result_errorf("write", "Out of memory");
	snprintf(msg, 64 + PATH_MAX, "%s %zu bytes to %s", verb, bytes, resolved);
	return tool_result_ok("write", msg);
}

struct tool_result tool_write_fn(const char *arguments_json,
				 const struct agent_configuration *cfg,
				 struct memory_arena *scratch)
{
	struct write_args args;

	if (!arguments_json)
		return tool_result_errorf("write", "Missing arguments");

	const char *parse_err = parse_write_args(arguments_json, &args, scratch);
	if (parse_err)
		return tool_result_errorf("write", "%s", parse_err);

	char resolved[PATH_MAX];
	struct tool_result err;
	if (tools_sandbox_resolve(cfg, args.path, resolved, sizeof(resolved), &err) != 0)
		return err;

	if (args.create_dirs && create_parent_dirs(cfg, &args, resolved, &err) != 0)
		return err;

	size_t content_len = strlen(args.content);

	/* Append mode: write directly — cannot be made atomic */
	if (args.mode == WRITE_APPEND) {
		int fd = open(resolved, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0644);
		if (fd < 0)
			return tool_result_errorf("write", "Cannot open file for append: %s",
						  args.path);
		int rc = fs_write_all(fd, args.content, content_len);
		if (close(fd) != 0)
			rc = -1;
		if (rc != 0)
			return tool_result_errorf("write", "Write error: %s", strerror(errno));
		return write_success("Appended", content_len, resolved);
	}

	/* Overwrite / create_new: temp file + rename (or link for create_new,
	 * which refuses to replace a file created in the meantime). */
	if (fs_write_atomic(resolved, args.content, content_len, 0644,
			    args.mode == WRITE_CREATE_NEW) != 0) {
		if (errno == EEXIST)
			return tool_result_errorf("write", "File already exists: %s", args.path);
		return tool_result_errorf("write", "Cannot write %s: %s",
					  args.path, strerror(errno));
	}
	return write_success("Written", content_len, resolved);
}

/* "notes.md (append, 812 bytes)". Measures the content in place: the
 * formatter runs before dispatch and must not copy up to 128 KB. */
void tool_write_format(const char *argument_json, char *buf, size_t size)
{
	cJSON *root = json_parse(argument_json);
	const char *path = json_get_str(root, "path");
	const char *content = json_get_str(root, "content");
	const char *mode = json_get_str(root, "mode");
	if (!path)
		snprintf(buf, size, "(invalid arguments)");
	else
		snprintf(buf, size, "%s (%s, %zu bytes)", path,
			 mode ? mode : "overwrite", content ? strlen(content) : 0);
	cJSON_Delete(root);
}

/* -------------------------------------------------------------------------
 * Definition
 * ---------------------------------------------------------------------- */

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

const struct tool_definition tool_write_definition = {
	.name = "write",
	.description = "Write or append content to a file within the sandbox. "
		       "Use mode=create_new to avoid overwriting existing files. "
		       "For large files (e.g. long HTML or source code), write in "
		       "multiple smaller chunks: first call with mode=overwrite to "
		       "create the file, then mode=append for each subsequent chunk. "
		       "This avoids JSON encoding errors from embedding large strings.",
	.parameters_schema = SCHEMA_WRITE,
	.fn = tool_write_fn,
	.format_call = tool_write_format,
	.flag = TOOL_WRITE,
	.confirm = TOOL_CONFIRM_WRITES,
};
