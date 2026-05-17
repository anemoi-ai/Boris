/*
 * tools/write.c - Write file contents within the sandbox.
 *
 * Supports overwrite, append, and create_new modes.
 * Optionally creates parent directories.
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "tools.h"
#include "arena.h"
#include "configuration.h"
#include "sandbox.h"
#include "json.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define fsync(fd) _commit(fd)
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#else
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#endif

#define WRITE_MAX_CONTENT 131072

struct write_args {
	char path[4096];
	char *content;
	char mode[32];
	int create_dirs;
};

static int parse_write_args(const char *json, struct write_args *args,
			    struct memory_arena *scratch)
{
	if (!json || !args)
		return -1;

	memset(args, 0, sizeof(*args));
	snprintf(args->mode, sizeof(args->mode), "%s", "overwrite");

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

	const char *content = json_get_str(root, "content");
	if (!content) {
		cJSON_Delete(root);
		return -1;
	}

	if (strlen(content) > WRITE_MAX_CONTENT) {
		cJSON_Delete(root);
		return -1;
	}

	/* Arena-allocated — no individual free needed; scratch owns it */
	args->content = arena_duplicate_string(scratch, content);
	if (!args->content) {
		cJSON_Delete(root);
		return -1;
	}

	const char *mode = json_get_str(root, "mode");
	if (mode)
		snprintf(args->mode, sizeof(args->mode), "%s", mode);

	if (json_get_bool(root, "create_dirs", 0))
		args->create_dirs = 1;

	cJSON_Delete(root);
	return 0;
}

struct tool_result tool_write_fn(const char *arguments_json,
				 const struct agent_configuration *cfg,
				 struct memory_arena *scratch)
{
	struct write_args args;

	if (!arguments_json) {
		return tool_result_errorf("write", "Missing arguments");
	}

	if (parse_write_args(arguments_json, &args, scratch) != 0) {
		return tool_result_errorf("write",
					  "Could not parse arguments as JSON. If the content "
					  "contains double quotes (e.g. HTML attributes), every "
					  "quote inside the content string must be escaped as \\\" "
					  "in the JSON. Consider writing the file in smaller chunks "
					  "to reduce encoding complexity.");
	}

	char resolved[PATH_MAX];
	struct tool_result sandbox_err;
	if (tools_sandbox_resolve(cfg, args.path, resolved, sizeof(resolved),
				  &sandbox_err) != 0)
		return sandbox_err;

	/* Create parent directories if requested */
	if (args.create_dirs) {
		char parent[PATH_MAX];
		snprintf(parent, sizeof(parent), "%s", resolved);
		char *last_slash = strrchr(parent, '/');
		if (last_slash && last_slash != parent) {
			*last_slash = '\0';
			if (sandbox_mkdirp(parent, 0755) != 0) {
				return tool_result_errorf("write",
							  "Failed to create parent directories");
			}
		}
	}

	size_t content_len = strlen(args.content);

	/* Append mode: write directly — cannot be made atomic */
	if (strcmp(args.mode, "append") == 0) {
		int fd = open(resolved, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW,
			      0644);
		if (fd < 0) {
			return tool_result_errorf("write",
						  "Cannot open file for append: %s", args.path);
		}
		size_t total_written = 0;
		while (total_written < content_len) {
			ssize_t n = write(fd, args.content + total_written,
					  content_len - total_written);
			if (n <= 0) {
				close(fd);
				return tool_result_errorf("write", "Write error");
			}
			total_written += (size_t)n;
		}
		close(fd);

		char *msg = malloc(64 + PATH_MAX);
		if (!msg)
			return tool_result_errorf("write", "Out of memory");
		snprintf(msg, 64 + PATH_MAX, "Appended %zu bytes to %s",
			 total_written, resolved);
		return tool_result_ok("write", msg);
	}

	/* Overwrite / create_new: write to a temp file then rename atomically */
	if (strlen(resolved) + 11 >= PATH_MAX) {
		return tool_result_errorf("write",
					  "Path too long for atomic write: %s", args.path);
	}

	/* For create_new, reject if the target already exists */
	if (strcmp(args.mode, "create_new") == 0) {
		struct stat exist_st;
		if (stat(resolved, &exist_st) == 0) {
			return tool_result_errorf("write",
						  "File already exists: %s", args.path);
		}
	}

	char tmp_path[PATH_MAX + 16];
	snprintf(tmp_path, sizeof(tmp_path), "%s.boris_tmp", resolved);

	int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
	if (fd < 0) {
		return tool_result_errorf("write",
					  "Cannot create file: %s", args.path);
	}

	size_t total_written = 0;
	while (total_written < content_len) {
		ssize_t n = write(fd, args.content + total_written,
				  content_len - total_written);
		if (n <= 0) {
			close(fd);
			unlink(tmp_path);
			return tool_result_errorf("write", "Write error");
		}
		total_written += (size_t)n;
	}
	fsync(fd);
	close(fd);

#ifdef _WIN32
	if (!MoveFileExA(tmp_path, resolved, MOVEFILE_REPLACE_EXISTING)) {
		DeleteFileA(tmp_path);
		return tool_result_errorf("write", "Cannot rename to: %s", resolved);
	}
#else
	if (rename(tmp_path, resolved) != 0) {
		unlink(tmp_path);
		return tool_result_errorf("write", "Cannot rename to: %s", resolved);
	}
#endif

	char *msg = malloc(64 + PATH_MAX);
	if (!msg)
		return tool_result_errorf("write", "Out of memory");

	snprintf(msg, 64 + PATH_MAX, "Written %zu bytes to %s",
		 total_written, resolved);
	return tool_result_ok("write", msg);
}
