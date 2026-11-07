/*
 * tools/edit.c - Replace an exact string in a file within the sandbox.
 *
 * old_string must occur exactly once unless replace_all is set, so an edit
 * cannot land somewhere the model did not intend. Matching is byte-exact:
 * whitespace, indentation and line endings all count. The file is rewritten
 * atomically (temp file + rename) and keeps its permission bits.
 *
 * This is what keeps small changes small: without it, changing one line
 * means sending the whole file back through write.
 */

#define _POSIX_C_SOURCE 200809L

#include "tools.h"
#include "tool_builtins.h"
#include "arena.h"
#include "fs.h"
#include "json.h"
#include "logger.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define EDIT_MAX_FILE	(4 * 1024 * 1024)
#define EDIT_MAX_STRING 131072

struct edit_args {
	char path[4096];
	const char *old_string; /* point into the parsed JSON */
	const char *new_string;
	bool replace_all;
};

/* Returns NULL on success, or a message describing the problem. */
static const char *parse_edit_args(const cJSON *root, struct edit_args *args)
{
	memset(args, 0, sizeof(*args));
	if (!root)
		return "Could not parse arguments as JSON. Quotes, backslashes and "
		       "newlines inside old_string and new_string must be escaped.";
	if (tools_arg_path(root, args->path, sizeof(args->path)) != 0)
		return "\"path\" is required";

	args->old_string = json_get_str(root, "old_string");
	args->new_string = json_get_str(root, "new_string");
	args->replace_all = json_get_bool(root, "replace_all", 0);

	if (!args->old_string)
		return "\"old_string\" is required";
	if (!args->old_string[0])
		return "\"old_string\" must not be empty; use write to create or "
		       "replace a whole file";
	if (!args->new_string)
		return "\"new_string\" is required (use \"\" to delete old_string)";
	if (strlen(args->old_string) > EDIT_MAX_STRING ||
	    strlen(args->new_string) > EDIT_MAX_STRING)
		return "old_string and new_string are limited to 131072 bytes each";
	if (strcmp(args->old_string, args->new_string) == 0)
		return "old_string and new_string are identical; nothing to change";
	return NULL;
}

/* Non-overlapping occurrences of needle in hay; *first gets the offset of
 * the first one. */
static size_t count_occurrences(const char *hay, const char *needle, size_t *first)
{
	size_t count = 0;
	size_t nlen = strlen(needle);
	for (const char *p = strstr(hay, needle); p; p = strstr(p + nlen, needle)) {
		if (count == 0)
			*first = (size_t)(p - hay);
		count++;
	}
	return count;
}

static size_t line_of_offset(const char *text, size_t offset)
{
	size_t line = 1;
	for (size_t i = 0; i < offset; i++)
		if (text[i] == '\n')
			line++;
	return line;
}

/* Build the edited text: every (or the first) occurrence replaced. */
static char *replace_occurrences(const char *text, size_t len,
				 const char *old_s, const char *new_s,
				 size_t count, size_t *out_len)
{
	size_t old_len = strlen(old_s);
	size_t new_len = strlen(new_s);
	size_t total = len - count * old_len + count * new_len;
	char *out = malloc(total + 1);
	if (!out)
		return NULL;

	const char *src = text;
	char *dst = out;
	for (size_t i = 0; i < count; i++) {
		const char *hit = strstr(src, old_s);
		memcpy(dst, src, (size_t)(hit - src));
		dst += hit - src;
		memcpy(dst, new_s, new_len);
		dst += new_len;
		src = hit + old_len;
	}
	size_t rest = len - (size_t)(src - text);
	memcpy(dst, src, rest);
	dst[rest] = '\0';
	*out_len = total;
	return out;
}

/* Explain a failed match, pointing at line endings when they differ. */
static struct tool_result not_found(const struct edit_args *args, const char *text)
{
	bool file_crlf = strstr(text, "\r\n") != NULL;
	bool needle_crlf = strstr(args->old_string, "\r\n") != NULL;
	if (file_crlf && !needle_crlf && strchr(args->old_string, '\n'))
		return tool_result_errorf("edit",
					  "old_string not found in %s. The file uses CRLF "
					  "(\\r\\n) line endings; old_string must use them too.",
					  args->path);
	return tool_result_errorf("edit",
				  "old_string not found in %s. It must match the file "
				  "exactly, including whitespace and indentation. Read "
				  "the file to check the current text.",
				  args->path);
}

static struct tool_result do_edit(const struct edit_args *args,
				  const struct agent_configuration *cfg)
{
	char resolved[PATH_MAX];
	struct tool_result err;
	if (tools_sandbox_resolve(cfg, args->path, resolved, sizeof(resolved), &err) != 0)
		return err;

	struct stat st;
	if (stat(resolved, &st) != 0)
		return tool_result_errorf("edit", "File not found: %s (use write to "
						  "create a new file)", args->path);
	if (!S_ISREG(st.st_mode))
		return tool_result_errorf("edit", "Not a regular file: %s", args->path);
	if (st.st_size > EDIT_MAX_FILE)
		return tool_result_errorf("edit", "File too large to edit (%lld bytes, "
						  "limit %d)", (long long)st.st_size,
					  EDIT_MAX_FILE);

	size_t len = 0;
	char *text = file_read_all(resolved, EDIT_MAX_FILE, &len);
	if (!text)
		return tool_result_errorf("edit", "Cannot read %s: %s", args->path,
					  strerror(errno));
	if (strlen(text) != len) {
		free(text);
		return tool_result_errorf("edit", "%s looks like a binary file", args->path);
	}

	size_t first = 0;
	size_t count = count_occurrences(text, args->old_string, &first);
	if (count == 0) {
		struct tool_result r = not_found(args, text);
		free(text);
		return r;
	}
	if (count > 1 && !args->replace_all) {
		free(text);
		return tool_result_errorf("edit",
					  "old_string occurs %zu times in %s. Include more "
					  "surrounding lines to make it unique, or set "
					  "replace_all=true to change every occurrence.",
					  count, args->path);
	}

	size_t replace = args->replace_all ? count : 1;
	size_t out_len = 0;
	char *out = replace_occurrences(text, len, args->old_string, args->new_string,
					replace, &out_len);
	size_t line = line_of_offset(text, first);
	free(text);
	if (!out)
		return tool_result_errorf("edit", "Out of memory");

	int rc = fs_write_atomic(resolved, out, out_len, 0644, false);
	int saved = errno;
	free(out);
	if (rc != 0)
		return tool_result_errorf("edit", "Cannot write %s: %s", args->path,
					  strerror(saved));

	char msg[PATH_MAX + 128];
	snprintf(msg, sizeof(msg), "Edited %s: replaced %zu occurrence%s (first at line %zu).",
		 args->path, replace, replace == 1 ? "" : "s", line);
	return tool_result_ok_copy("edit", msg);
}

struct tool_result tool_edit_fn(const char *arguments_json,
				const struct agent_configuration *cfg,
				struct memory_arena *scratch)
{
	(void)scratch;
	if (!arguments_json)
		return tool_result_errorf("edit", "Missing arguments");

	cJSON *root = json_parse(arguments_json);
	struct edit_args args;
	const char *parse_err = parse_edit_args(root, &args);
	struct tool_result r = parse_err ? tool_result_errorf("edit", "%s", parse_err)
					 : do_edit(&args, cfg);
	cJSON_Delete(root);
	return r;
}

/* "main.c (42 -> 57 bytes)" or "main.c (42 -> 57 bytes, all)". */
void tool_edit_format(const char *argument_json, char *buf, size_t size)
{
	cJSON *root = json_parse(argument_json);
	const char *path = json_get_str(root, "path");
	const char *old_s = json_get_str(root, "old_string");
	const char *new_s = json_get_str(root, "new_string");
	if (!path)
		snprintf(buf, size, "(invalid arguments)");
	else
		snprintf(buf, size, "%s (%zu -> %zu bytes%s)", path,
			 old_s ? strlen(old_s) : 0, new_s ? strlen(new_s) : 0,
			 json_get_bool(root, "replace_all", 0) ? ", all" : "");
	cJSON_Delete(root);
}

/* -------------------------------------------------------------------------
 * Definition
 * ---------------------------------------------------------------------- */

static const char SCHEMA_EDIT[] =
	"{"
	"\"type\":\"object\","
	"\"properties\":{"
	"\"path\":{\"type\":\"string\","
	"\"description\":\"File to edit.\"},"
	"\"old_string\":{\"type\":\"string\","
	"\"description\":\"Exact text to replace, including whitespace and "
	"indentation. Include enough surrounding lines to make it unique.\"},"
	"\"new_string\":{\"type\":\"string\","
	"\"description\":\"Replacement text. Use an empty string to delete old_string.\"},"
	"\"replace_all\":{\"type\":\"boolean\","
	"\"default\":false,"
	"\"description\":\"Replace every occurrence instead of requiring exactly one.\"}"
	"},"
	"\"required\":[\"path\",\"old_string\",\"new_string\"]"
	"}";

const struct tool_definition tool_edit_definition = {
	.name = "edit",
	.description = "Replace exact text in an existing file within the sandbox. "
		       "old_string must match the file exactly and occur once "
		       "(or set replace_all). Prefer this to write for changing "
		       "part of a file: only the changed text is sent. Read the "
		       "file first so old_string matches.",
	.parameters_schema = SCHEMA_EDIT,
	.fn = tool_edit_fn,
	.format_call = tool_edit_format,
	.flag = TOOL_EDIT,
	.confirm = TOOL_CONFIRM_WRITES,
};
