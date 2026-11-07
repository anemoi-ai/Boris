/*
 * tools/read.c - Read file contents within the sandbox.
 *
 * Reads a file with optional offset and length parameters.
 * Detects binary content and refuses to return it.
 */

#define _POSIX_C_SOURCE 200809L

#include "tools.h"
#include "tool_builtins.h"
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

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

#define READ_DEFAULT_LENGTH	 65536
#define READ_MAX_LENGTH		 131072
#define READ_TRUNCATION_NOTE_MAX 256

struct read_args {
	char path[4096];
	long offset;
	long length;
};

static int parse_read_args(const char *json, struct read_args *args)
{
	if (!json || !args)
		return -1;

	memset(args, 0, sizeof(*args));
	args->offset = 0;
	args->length = READ_DEFAULT_LENGTH;

	cJSON *root = json_parse(json);
	if (!root)
		return -1;

	if (tools_arg_path(root, args->path, sizeof(args->path)) != 0) {
		cJSON_Delete(root);
		return -1;
	}

	cJSON *offset_node = cJSON_GetObjectItemCaseSensitive(root, "offset");
	if (offset_node && cJSON_IsNumber(offset_node)) {
		args->offset = (long)offset_node->valuedouble;
		if (args->offset < 0)
			args->offset = 0;
	}

	cJSON *length_node = cJSON_GetObjectItemCaseSensitive(root, "length");
	if (length_node && cJSON_IsNumber(length_node)) {
		args->length = (long)length_node->valuedouble;
		if (args->length <= 0)
			args->length = READ_DEFAULT_LENGTH;
		else if (args->length > READ_MAX_LENGTH)
			args->length = READ_MAX_LENGTH;
	}

	cJSON_Delete(root);
	return 0;
}

static int is_binary_content(const char *data, size_t len)
{
	if (len == 0)
		return 0;

	size_t check = len < 512 ? len : 512;
	size_t non_printable = 0;

	for (size_t i = 0; i < check; i++) {
		unsigned char c = (unsigned char)data[i];
		if (c < 32 && c != '\t' && c != '\n' && c != '\r')
			non_printable++;
	}

	return ((non_printable * 100) / check) > 30;
}

struct tool_result tool_read_fn(const char *arguments_json,
				const struct agent_configuration *cfg,
				struct memory_arena *scratch)
{
	(void)scratch; /* buffer exceeds arena block size; heap allocation retained */
	struct read_args args;

	if (!arguments_json) {
		return tool_result_errorf("read", "Missing arguments");
	}

	if (parse_read_args(arguments_json, &args) != 0) {
		return tool_result_errorf("read", "Invalid arguments");
	}

	char resolved[PATH_MAX];
	struct tool_result sandbox_err;
	if (tools_sandbox_resolve(cfg, args.path, resolved, sizeof(resolved),
				  &sandbox_err) != 0)
		return sandbox_err;

	struct stat st;
	if (stat(resolved, &st) != 0) {
		return tool_result_errorf("read", "File not found: %s", args.path);
	}

	if (S_ISDIR(st.st_mode)) {
		return tool_result_errorf("read",
					  "Path is a directory: %s", args.path);
	}

	/* FIFOs and devices would block or never end */
	if (!S_ISREG(st.st_mode)) {
		return tool_result_errorf("read",
					  "Not a regular file: %s", args.path);
	}

	size_t total_size = (size_t)st.st_size;
	if (args.offset > (long)total_size) {
		return tool_result_errorf("read", "Offset exceeds file size");
	}

	size_t read_size = (size_t)args.length;
	if ((size_t)args.offset + read_size > total_size)
		read_size = total_size - (size_t)args.offset;

	int fd = open(resolved, O_RDONLY | O_NOFOLLOW);
	if (fd < 0) {
		return tool_result_errorf("read", "Cannot open file");
	}

	if (lseek(fd, (off_t)args.offset, SEEK_SET) < 0) {
		close(fd);
		return tool_result_errorf("read", "Cannot seek in file");
	}

	/* Pre-size to hold read data plus optional truncation note in one alloc */
	char *buf = malloc(read_size + READ_TRUNCATION_NOTE_MAX + 1);
	if (!buf) {
		close(fd);
		return tool_result_errorf("read", "Out of memory");
	}

	ssize_t n = read(fd, buf, read_size);
	close(fd);

	if (n < 0) {
		free(buf);
		return tool_result_errorf("read", "Read error");
	}

	buf[n] = '\0';

	if (is_binary_content(buf, (size_t)n)) {
		free(buf);
		return tool_result_errorf("read", "Binary content detected");
	}

	/* Append truncation note in-place — no realloc needed */
	size_t next = (size_t)args.offset + (size_t)n;
	if (next < total_size) {
		int note_len = snprintf(buf + n, READ_TRUNCATION_NOTE_MAX,
					"\n[File truncated: showing bytes %ld-%zu of %zu. "
					"Use offset=%zu to read further.]",
					args.offset, next, total_size, next);
		if (note_len > 0)
			n += note_len < READ_TRUNCATION_NOTE_MAX ? note_len
								 : READ_TRUNCATION_NOTE_MAX - 1;
		buf[n] = '\0';
	}

	return tool_result_ok("read", buf);
}

/* "notes.md" or "notes.md (from byte 4096)". Display only; never fails. */
void tool_read_format(const char *argument_json, char *buf, size_t size)
{
	cJSON *root = json_parse(argument_json);
	const char *path = json_get_str(root, "path");
	int offset = json_get_int(root, "offset", 0);
	if (!path)
		snprintf(buf, size, "(invalid arguments)");
	else if (offset > 0)
		snprintf(buf, size, "%s (from byte %d)", path, offset);
	else
		snprintf(buf, size, "%s", path);
	cJSON_Delete(root);
}

/* -------------------------------------------------------------------------
 * Definition
 * ---------------------------------------------------------------------- */

static const char SCHEMA_READ[] =
	"{"
	"\"type\":\"object\","
	"\"properties\":{"
	"\"path\":{\"type\":\"string\","
	"\"description\":\"File path to read. Relative paths are resolved within the sandbox root.\"},"
	"\"offset\":{\"type\":\"integer\","
	"\"description\":\"Byte offset to start reading from.\","
	"\"default\":0},"
	"\"length\":{\"type\":\"integer\","
	"\"description\":\"Maximum bytes to read (capped at 131072).\","
	"\"default\":65536}"
	"},"
	"\"required\":[\"path\"]"
	"}";

const struct tool_definition tool_read_definition = {
	.name = "read",
	.description = "Read the contents of a file within the sandbox. "
		       "Use offset and length to page through large files.",
	.parameters_schema = SCHEMA_READ,
	.fn = tool_read_fn,
	.format_call = tool_read_format,
	.flag = TOOL_READ,
	.confirm = TOOL_CONFIRM_NEVER,
};
