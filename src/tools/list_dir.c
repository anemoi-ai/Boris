/*
 * tools/list_dir.c - List directory contents within the sandbox.
 *
 * Shows files and directories with type, size, and modification date.
 * Supports recursive listing (depth limited) and hidden files.
 */

#define _POSIX_C_SOURCE 200809L

#include "tools.h"
#include "arena.h"
#include "configuration.h"
#include "sandbox.h"
#include "json.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#define LIST_DIR_MAX_ENTRIES 1000

struct list_dir_args {
	char path[4096];
	int show_hidden;
};

static int parse_list_dir_args(const char *json, struct list_dir_args *args)
{
	if (!json || !args)
		return -1;

	memset(args, 0, sizeof(*args));

	cJSON *root = json_parse(json);
	if (!root)
		return -1;

	const char *path = json_get_str(root, "path");
	if (!path) {
		cJSON_Delete(root);
		return -1;
	}

	if (strlen(path) >= sizeof(args->path)) {
		cJSON_Delete(root);
		return -1;
	}
	snprintf(args->path, sizeof(args->path), "%s", path);

	if (json_get_bool(root, "show_hidden", 0))
		args->show_hidden = 1;

	cJSON_Delete(root);
	return 0;
}

static const char *type_char(mode_t mode)
{
	if (S_ISDIR(mode))
		return "d";
	if (S_ISLNK(mode))
		return "l";
	return "f";
}

static void format_size(size_t size, char *buf, size_t buf_size)
{
	if (size >= 1048576)
		snprintf(buf, buf_size, "%.1f MB", (double)size / 1048576);
	else if (size >= 1024)
		snprintf(buf, buf_size, "%.1f KB", (double)size / 1024);
	else
		snprintf(buf, buf_size, "%zu B", size);
}

static void format_time(time_t t, char *buf, size_t buf_size)
{
	struct tm *tm = localtime(&t);
	strftime(buf, buf_size, "%Y-%m-%d", tm);
}

static int list_one_dir(const char *dir_path, int show_hidden,
			char **output, size_t *out_size, size_t *out_cap,
			int *count, int max_entries)
{
	DIR *dir = opendir(dir_path);
	if (!dir)
		return -1;

	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL && *count < max_entries) {
		if (!show_hidden && entry->d_name[0] == '.')
			continue;

		struct stat st;

		if (fstatat(dirfd(dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
			continue;

		char size_buf[32], time_buf[32];
		format_size((size_t)st.st_size, size_buf, sizeof(size_buf));
		format_time(st.st_mtime, time_buf, sizeof(time_buf));

		/* Grow output if needed */
		size_t line_len = strlen(entry->d_name) + 64;
		if (*out_size + line_len >= *out_cap) {
			*out_cap *= 2;
			char *tmp = realloc(*output, *out_cap);
			if (!tmp) {
				closedir(dir);
				return -1;
			}
			*output = tmp;
		}

		*out_size += (size_t)snprintf(*output + *out_size,
					      *out_cap - *out_size,
					      "    %s %s %s [%s]\n",
					      entry->d_name, size_buf,
					      time_buf, type_char(st.st_mode));
		(*count)++;
	}

	closedir(dir);
	return 0;
}

struct tool_result tool_list_dir_fn(const char *arguments_json,
				    const struct agent_configuration *cfg,
				    struct memory_arena *scratch)
{
	(void)scratch;
	struct list_dir_args args;

	if (!arguments_json) {
		return tool_result_errorf("list_dir", "Missing arguments");
	}

	if (parse_list_dir_args(arguments_json, &args) != 0) {
		return tool_result_errorf("list_dir", "Invalid arguments");
	}

	/* Normalize "/" or empty path to "." (sandbox root) */
	if (args.path[0] == '\0' ||
	    (args.path[0] == '/' && args.path[1] == '\0')) {
		args.path[0] = '.';
		args.path[1] = '\0';
	}

	char resolved[PATH_MAX];
	struct tool_result sandbox_err;
	if (tools_sandbox_resolve(cfg, args.path, resolved, sizeof(resolved),
				  &sandbox_err) != 0)
		return sandbox_err;

	struct stat st;
	if (stat(resolved, &st) != 0) {
		return tool_result_errorf("list_dir",
					  "Path not found: %s", args.path);
	}

	if (!S_ISDIR(st.st_mode)) {
		return tool_result_errorf("list_dir",
					  "Not a directory: %s", args.path);
	}

	size_t cap = 8192;
	char *output = malloc(cap);
	if (!output)
		return tool_result_errorf("list_dir", "Out of memory");

	size_t out_size = 0;
	int count = 0;

	out_size += (size_t)snprintf(output, cap, "%s/\n", resolved);

	list_one_dir(resolved, args.show_hidden,
		     &output, &out_size, &cap, &count, LIST_DIR_MAX_ENTRIES);

	if (count >= LIST_DIR_MAX_ENTRIES) {
		if (out_size + 50 >= cap) {
			cap *= 2;
			char *tmp = realloc(output, cap);
			if (!tmp) {
				free(output);
				return tool_result_errorf("list_dir", "Out of memory");
			}
			output = tmp;
		}
		out_size += (size_t)snprintf(output + out_size, cap - out_size,
					     "[Truncated at %d entries]\n",
					     LIST_DIR_MAX_ENTRIES);
	}

	return tool_result_ok("list_dir", output);
}
