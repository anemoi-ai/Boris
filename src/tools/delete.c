/*
 * tools/delete.c - Delete a file, symlink or directory within the sandbox.
 *
 * The path is resolved with tools_sandbox_resolve_entry(): a symlink is
 * removed itself, never its target, and the sandbox root cannot be named.
 * A directory must be empty unless recursive is set; the recursive walk
 * uses lstat() and never follows symlinks, so it cannot reach outside the
 * tree it was given.
 */

#define _POSIX_C_SOURCE 200809L

#include "tools.h"
#include "tool_builtins.h"
#include "json.h"
#include "logger.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DELETE_MAX_DEPTH 64

struct delete_counts {
	long files; /* files, symlinks and other non-directories */
	long dirs;
};

/* Remove path and everything under it. Returns 0, or -1 with errno set. */
static int remove_tree(const char *path, int depth, struct delete_counts *n)
{
	struct stat st;
	if (lstat(path, &st) != 0)
		return -1;
	if (!S_ISDIR(st.st_mode)) {
		if (unlink(path) != 0)
			return -1;
		n->files++;
		return 0;
	}
	if (depth >= DELETE_MAX_DEPTH) {
		errno = ELOOP;
		return -1;
	}

	DIR *d = opendir(path);
	if (!d)
		return -1;
	int rc = 0;
	struct dirent *e;
	while (rc == 0 && (e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		if (tools_cancelled()) {
			errno = EINTR;
			rc = -1;
			break;
		}
		char child[PATH_MAX];
		int w = snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
		if (w < 0 || (size_t)w >= sizeof(child)) {
			errno = ENAMETOOLONG;
			rc = -1;
			break;
		}
		rc = remove_tree(child, depth + 1, n);
	}
	int saved = errno;
	closedir(d);
	errno = saved;
	if (rc != 0)
		return -1;
	if (rmdir(path) != 0)
		return -1;
	n->dirs++;
	return 0;
}

static struct tool_result do_delete(const char *path, bool recursive,
				    const struct agent_configuration *cfg)
{
	char target[PATH_MAX];
	struct tool_result err;
	if (tools_sandbox_resolve_entry(cfg, path, target, sizeof(target), &err) != 0)
		return err;

	struct stat st;
	if (lstat(target, &st) != 0)
		return tool_result_errorf("delete", "Not found: %s", path);

	char msg[PATH_MAX + 128];
	if (!S_ISDIR(st.st_mode)) {
		if (unlink(target) != 0)
			return tool_result_errorf("delete", "Cannot delete %s: %s", path,
						  strerror(errno));
		snprintf(msg, sizeof(msg), "Deleted %s", path);
		return tool_result_ok_copy("delete", msg);
	}

	if (!recursive) {
		if (rmdir(target) == 0) {
			snprintf(msg, sizeof(msg), "Deleted empty directory %s", path);
			return tool_result_ok_copy("delete", msg);
		}
		if (errno == ENOTEMPTY || errno == EEXIST)
			return tool_result_errorf("delete", "Directory not empty: %s. Set "
							    "recursive=true to delete it and "
							    "everything in it.", path);
		return tool_result_errorf("delete", "Cannot delete %s: %s", path,
					  strerror(errno));
	}

	struct delete_counts n = {0, 0};
	if (remove_tree(target, 0, &n) != 0)
		return tool_result_errorf("delete", "Stopped after deleting %ld file%s and "
						    "%ld director%s in %s: %s",
					  n.files, n.files == 1 ? "" : "s", n.dirs,
					  n.dirs == 1 ? "y" : "ies", path, strerror(errno));
	snprintf(msg, sizeof(msg), "Deleted %s (%ld file%s, %ld director%s)", path,
		 n.files, n.files == 1 ? "" : "s", n.dirs, n.dirs == 1 ? "y" : "ies");
	return tool_result_ok_copy("delete", msg);
}

struct tool_result tool_delete_fn(const char *arguments_json,
				  const struct agent_configuration *cfg,
				  struct memory_arena *scratch)
{
	(void)scratch;
	cJSON *root = json_parse(arguments_json ? arguments_json : "");
	const char *path = json_get_str(root, "path");
	struct tool_result r;
	if (!root)
		r = tool_result_errorf("delete", "Could not parse arguments as JSON");
	else if (!path || !path[0])
		r = tool_result_errorf("delete", "\"path\" is required");
	else
		r = do_delete(path, json_get_bool(root, "recursive", 0), cfg);
	cJSON_Delete(root);
	return r;
}

/* "build/ (recursive)" or "notes.txt". */
void tool_delete_format(const char *argument_json, char *buf, size_t size)
{
	cJSON *root = json_parse(argument_json);
	const char *path = json_get_str(root, "path");
	if (!path)
		snprintf(buf, size, "(invalid arguments)");
	else
		snprintf(buf, size, "%s%s", path,
			 json_get_bool(root, "recursive", 0) ? " (recursive)" : "");
	cJSON_Delete(root);
}

/* -------------------------------------------------------------------------
 * Definition
 * ---------------------------------------------------------------------- */

static const char SCHEMA_DELETE[] =
	"{"
	"\"type\":\"object\","
	"\"properties\":{"
	"\"path\":{\"type\":\"string\","
	"\"description\":\"File, symlink or directory to delete.\"},"
	"\"recursive\":{\"type\":\"boolean\","
	"\"default\":false,"
	"\"description\":\"Delete a directory and everything in it.\"}"
	"},"
	"\"required\":[\"path\"]"
	"}";

const struct tool_definition tool_delete_definition = {
	.name = "delete",
	.description = "Delete a file, symlink or directory within the sandbox. "
		       "Directories must be empty unless recursive is set. "
		       "This cannot be undone.",
	.parameters_schema = SCHEMA_DELETE,
	.fn = tool_delete_fn,
	.format_call = tool_delete_format,
	.flag = TOOL_DELETE,
	.confirm = TOOL_CONFIRM_WRITES,
};
