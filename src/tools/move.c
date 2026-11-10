/*
 * tools/move.c - Rename or move a file or directory within the sandbox.
 *
 * Both paths are resolved with tools_sandbox_resolve_entry(), so a symlink
 * is moved itself rather than whatever it points to, and neither end can
 * be outside the sandbox. An existing destination is only replaced when
 * overwrite is set.
 */

#define _POSIX_C_SOURCE 200809L

#include "tools.h"
#include "tool_builtins.h"
#include "json.h"
#include "logger.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "platform.h"

/* "dir/" as a destination means "into dir, keeping the name". */
static const char *destination_path(const char *from, const char *to,
				    char *buf, size_t size)
{
	size_t tl = strlen(to);
	if (tl == 0 || (to[tl - 1] != '/' && !(PLATFORM_WINDOWS && to[tl - 1] == '\\')))
		return to;
	char name[PATH_MAX];
	snprintf(name, sizeof(name), "%s", from);
	platform_path_normalize(name);
	size_t nl = strlen(name);
	while (nl > 1 && name[nl - 1] == '/')
		name[--nl] = '\0';
	const char *slash = strrchr(name, '/');
	snprintf(buf, size, "%s%s", to, slash ? slash + 1 : name);
	return buf;
}

static struct tool_result do_move(const char *from, const char *to, bool overwrite,
				  const struct agent_configuration *cfg)
{
	char to_buf[PATH_MAX];
	to = destination_path(from, to, to_buf, sizeof(to_buf));

	char src[PATH_MAX], dst[PATH_MAX];
	struct tool_result err;
	if (tools_sandbox_resolve_entry(cfg, from, src, sizeof(src), &err) != 0 ||
	    tools_sandbox_resolve_entry(cfg, to, dst, sizeof(dst), &err) != 0)
		return err;

	struct stat st;
	if (lstat(src, &st) != 0)
		return tool_result_errorf("move", "Not found: %s", from);
	if (strcmp(src, dst) == 0)
		return tool_result_errorf("move", "Source and destination are the same");
	size_t src_len = strlen(src);
	if (S_ISDIR(st.st_mode) && strncmp(dst, src, src_len) == 0 && dst[src_len] == '/')
		return tool_result_errorf("move", "Cannot move a directory into itself");
	if (!overwrite && lstat(dst, &st) == 0)
		return tool_result_errorf("move", "Destination exists: %s. Set "
						  "overwrite=true to replace it.", to);

	if (rename(src, dst) != 0) {
		switch (errno) {
		case ENOENT:
			return tool_result_errorf("move", "The directory for %s does not "
							  "exist; create it first", to);
		case EXDEV:
			return tool_result_errorf("move", "Cannot move across filesystems");
		case EINVAL:
			return tool_result_errorf("move", "Cannot move a directory into "
							  "itself");
		case EISDIR:
		case ENOTDIR:
		case ENOTEMPTY:
		case EEXIST:
			return tool_result_errorf("move", "Cannot replace %s: %s", to,
						  strerror(errno));
		default:
			return tool_result_errorf("move", "Cannot move %s: %s", from,
						  strerror(errno));
		}
	}

	char msg[2 * PATH_MAX];
	snprintf(msg, sizeof(msg), "Moved %s to %s", from, to);
	return tool_result_ok_copy("move", msg);
}

struct tool_result tool_move_fn(const char *arguments_json,
				const struct agent_configuration *cfg,
				struct memory_arena *scratch)
{
	(void)scratch;
	cJSON *root = json_parse(arguments_json ? arguments_json : "");
	const char *from = json_get_str(root, "from");
	const char *to = json_get_str(root, "to");
	struct tool_result r;
	if (!root)
		r = tool_result_errorf("move", "Could not parse arguments as JSON");
	else if (!from || !from[0] || !to || !to[0])
		r = tool_result_errorf("move", "\"from\" and \"to\" are required");
	else
		r = do_move(from, to, json_get_bool(root, "overwrite", 0), cfg);
	cJSON_Delete(root);
	return r;
}

/* "old.txt -> archive/old.txt". */
void tool_move_format(const char *argument_json, char *buf, size_t size)
{
	cJSON *root = json_parse(argument_json);
	const char *from = json_get_str(root, "from");
	const char *to = json_get_str(root, "to");
	if (!from || !to)
		snprintf(buf, size, "(invalid arguments)");
	else
		snprintf(buf, size, "%s -> %s%s", from, to,
			 json_get_bool(root, "overwrite", 0) ? " (overwrite)" : "");
	cJSON_Delete(root);
}

/* -------------------------------------------------------------------------
 * Definition
 * ---------------------------------------------------------------------- */

static const char SCHEMA_MOVE[] =
	"{"
	"\"type\":\"object\","
	"\"properties\":{"
	"\"from\":{\"type\":\"string\","
	"\"description\":\"File or directory to move.\"},"
	"\"to\":{\"type\":\"string\","
	"\"description\":\"New path; its directory must already exist. End it "
	"with / to move into that directory keeping the name.\"},"
	"\"overwrite\":{\"type\":\"boolean\","
	"\"default\":false,"
	"\"description\":\"Replace an existing destination.\"}"
	"},"
	"\"required\":[\"from\",\"to\"]"
	"}";

const struct tool_definition tool_move_definition = {
	.name = "move",
	.description = "Rename or move a file or directory within the sandbox. "
		       "Fails if the destination exists unless overwrite is set.",
	.parameters_schema = SCHEMA_MOVE,
	.fn = tool_move_fn,
	.format_call = tool_move_format,
	.flag = TOOL_MOVE,
	.confirm = TOOL_CONFIRM_WRITES,
};
