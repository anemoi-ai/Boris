/*
 * tools/run.c - Execute a script file within the sandbox.
 *
 * Resolves the script path inside the sandbox, detects the interpreter via
 * shebang or file extension, then fork/exec's the child with combined
 * stdout+stderr captured and returned to the model.
 *
 * Security properties:
 *   - Script path is validated by sandbox_resolve() before any exec
 *   - Execution uses fork/execvp - never system() or popen()
 *   - Shebang interpreter must be an absolute path
 *   - Extension-based fallback uses a fixed allowlist; no arbitrary commands
 *   - Combined output is capped at RUN_MAX_OUTPUT bytes
 *   - Hard timeout enforced with SIGKILL
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
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <time.h>

#define RUN_MAX_OUTPUT	    65536
#define RUN_MAX_ARGV	    40	 /* interpreter parts + script + user args + NULL */
#define RUN_DEFAULT_TIMEOUT 30	 /* seconds */
#define RUN_MAX_TIMEOUT	    120	 /* hard cap regardless of config */
#define RUN_MAX_STDIN	    8192 /* max stdin bytes accepted */
#define USER_ARGS_MAX	    16
#define USER_ARG_LEN	    512

/* -------------------------------------------------------------------------
 * Interpreter resolution
 * ---------------------------------------------------------------------- */

static const struct {
	const char *ext;
	const char *interpreter;
} ext_map[] = {
	{".py", "python3"},
	{".js", "node"},
	{".sh", "sh"},
	{".rb", "ruby"},
	{".lua", "lua"},
	{".pl", "perl"},
	{".php", "php"},
};

static const char *interp_from_extension(const char *path)
{
	const char *dot = strrchr(path, '.');
	if (!dot)
		return NULL;
	for (size_t i = 0; i < sizeof(ext_map) / sizeof(ext_map[0]); i++) {
		if (strcmp(dot, ext_map[i].ext) == 0)
			return ext_map[i].interpreter;
	}
	return NULL;
}

/*
 * build_argv_shebang - read the first line of the script, parse the #!
 * interpreter, and populate argv_out ready for execvp.
 *
 * token_buf must be at least 512 bytes; tokens point into it.
 * Returns the number of argv entries (excluding NULL terminator), or -1 if
 * no valid shebang is present.
 */
static int build_argv_shebang(const char *script_path,
			      char *const *user_args, int user_argc,
			      char **argv_out, int argv_cap,
			      char *token_buf, size_t token_buf_size)
{
	FILE *f = fopen(script_path, "r");
	if (!f)
		return -1;

	char line[512];
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return -1;
	}
	fclose(f);

	if (line[0] != '#' || line[1] != '!')
		return -1;

	size_t len = strlen(line);
	while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
		line[--len] = '\0';

	snprintf(token_buf, token_buf_size, "%s", line + 2);

	int argc = 0;
	char *tok = strtok(token_buf, " \t");
	while (tok && argc < argv_cap - user_argc - 2) {
		if (tok[0] != '\0')
			argv_out[argc++] = tok;
		tok = strtok(NULL, " \t");
	}

	if (argc == 0)
		return -1;

	/* Shebang interpreter must be an absolute path for safety */
	if (argv_out[0][0] != '/')
		return -1;

	argv_out[argc++] = (char *)script_path;

	for (int i = 0; i < user_argc && argc < argv_cap - 1; i++)
		argv_out[argc++] = user_args[i];

	argv_out[argc] = NULL;
	return argc;
}

/* -------------------------------------------------------------------------
 * Argument parsing
 * ---------------------------------------------------------------------- */

struct run_args {
	char path[4096];
	char user_args[USER_ARGS_MAX][USER_ARG_LEN];
	int user_argc;
	int timeout_seconds;
	char *stdin_data; /* heap-allocated, may be NULL */
	size_t stdin_len;
};

static int parse_run_args(const char *json_str, struct run_args *out)
{
	if (!json_str || !out)
		return -1;

	memset(out, 0, sizeof(*out));
	out->timeout_seconds = RUN_DEFAULT_TIMEOUT;

	cJSON *root = json_parse(json_str);
	if (!root)
		return -1;

	const char *path = json_get_str(root, "path");
	if (!path || strlen(path) >= sizeof(out->path)) {
		cJSON_Delete(root);
		return -1;
	}
	snprintf(out->path, sizeof(out->path), "%s", path);

	cJSON *args_node = cJSON_GetObjectItemCaseSensitive(root, "args");
	if (args_node && cJSON_IsArray(args_node)) {
		int n = cJSON_GetArraySize(args_node);
		if (n > USER_ARGS_MAX)
			n = USER_ARGS_MAX;
		for (int i = 0; i < n; i++) {
			cJSON *item = cJSON_GetArrayItem(args_node, i);
			if (!item || !cJSON_IsString(item))
				continue;
			snprintf(out->user_args[out->user_argc],
				 USER_ARG_LEN, "%s", item->valuestring);
			out->user_argc++;
		}
	}

	cJSON *timeout_node = cJSON_GetObjectItemCaseSensitive(root,
							       "timeout_seconds");
	if (timeout_node && cJSON_IsNumber(timeout_node)) {
		int t = (int)timeout_node->valuedouble;
		if (t < 1)
			t = 1;
		if (t > RUN_MAX_TIMEOUT)
			t = RUN_MAX_TIMEOUT;
		out->timeout_seconds = t;
	}

	const char *stdin_str = json_get_str(root, "stdin");
	if (stdin_str && stdin_str[0]) {
		size_t slen = strlen(stdin_str);
		if (slen > RUN_MAX_STDIN)
			slen = RUN_MAX_STDIN;
		out->stdin_data = malloc(slen + 1);
		if (out->stdin_data) {
			memcpy(out->stdin_data, stdin_str, slen);
			out->stdin_data[slen] = '\0';
			out->stdin_len = slen;
		}
	}

	cJSON_Delete(root);
	return 0;
}

static long elapsed_ms(const struct timespec *start)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long)((now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000);
}

/*
 * exec_child - fork/exec argv, capture combined stdout+stderr with a timeout.
 *
 * cwd is the working directory for the child (may be NULL to inherit).
 * Returns a heap-allocated result string, or NULL on internal failure.
 * *timed_out_out is set to 1 if the child was killed for exceeding the limit.
 */
static char *exec_child(char *const argv[], const char *cwd,
			const char *stdin_data, size_t stdin_len,
			int timeout_seconds, int *timed_out_out)
{
	int out_pipe[2]; /* parent reads [0], child writes [1] */
	int in_pipe[2];	 /* parent writes [1], child reads [0] */
	*timed_out_out = 0;

	if (pipe(out_pipe) != 0)
		return NULL;

	int has_stdin = (stdin_data && stdin_len > 0);
	if (has_stdin && pipe(in_pipe) != 0) {
		close(out_pipe[0]);
		close(out_pipe[1]);
		return NULL;
	}

	pid_t pid = fork();
	if (pid < 0) {
		close(out_pipe[0]);
		close(out_pipe[1]);
		if (has_stdin) {
			close(in_pipe[0]);
			close(in_pipe[1]);
		}
		return NULL;
	}

	if (pid == 0) {
		/* ---- child ---- */
		dup2(out_pipe[1], STDOUT_FILENO);
		dup2(out_pipe[1], STDERR_FILENO);
		close(out_pipe[0]);
		close(out_pipe[1]);

		if (has_stdin) {
			dup2(in_pipe[0], STDIN_FILENO);
			close(in_pipe[0]);
			close(in_pipe[1]);
		} else {
			int null_fd = open("/dev/null", O_RDONLY);
			if (null_fd >= 0) {
				dup2(null_fd, STDIN_FILENO);
				close(null_fd);
			}
		}

		if (cwd) {
			if (chdir(cwd) != 0) {
				fprintf(stderr, "run: cannot chdir to '%s': %s\n",
					cwd, strerror(errno));
				_exit(127);
			}
		}

		execvp(argv[0], argv);
		fprintf(stderr, "run: cannot execute '%s': %s\n",
			argv[0], strerror(errno));
		_exit(127);
	}

	/* ---- parent ---- */
	close(out_pipe[1]);

	if (has_stdin) {
		close(in_pipe[0]);
		size_t written = 0;
		while (written < stdin_len) {
			ssize_t n = write(in_pipe[1],
					  stdin_data + written,
					  stdin_len - written);
			if (n <= 0)
				break;
			written += (size_t)n;
		}
		close(in_pipe[1]);
	}

	/* Capture output with deadline */
	char *outbuf = malloc(RUN_MAX_OUTPUT + 1);
	if (!outbuf) {
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		close(out_pipe[0]);
		return NULL;
	}
	size_t outlen = 0;
	int timed_out = 0;
	int output_truncated = 0;

	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);

	for (;;) {
		long remaining = (long)timeout_seconds * 1000 - elapsed_ms(&start);
		if (remaining <= 0) {
			timed_out = 1;
			break;
		}

		int poll_ms = (int)(remaining > 5000 ? 5000 : remaining);
		struct pollfd pfd = {.fd = out_pipe[0], .events = POLLIN};
		int r = poll(&pfd, 1, poll_ms);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (r == 0)
			continue; /* re-check deadline */

		size_t space = RUN_MAX_OUTPUT - outlen;
		char drain[4096];
		char *readbuf = space > 0 ? (outbuf + outlen) : drain;
		size_t readlen = space > 0 ? space : sizeof(drain);

		ssize_t n = read(out_pipe[0], readbuf, readlen);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			break; /* EOF — child closed its write end */

		if (space > 0)
			outlen += (size_t)n;
		else
			output_truncated = 1;
	}
	outbuf[outlen] = '\0';
	close(out_pipe[0]);

	if (timed_out)
		kill(pid, SIGKILL);

	int status = 0;
	waitpid(pid, &status, 0);

	int exit_code = 0;
	if (WIFEXITED(status))
		exit_code = WEXITSTATUS(status);
	else if (WIFSIGNALED(status))
		exit_code = 128 + WTERMSIG(status);

	/* Format result: header + captured output */
	size_t result_cap = outlen + 256;
	char *result = malloc(result_cap);
	if (!result) {
		free(outbuf);
		return NULL;
	}

	int hlen;
	if (timed_out) {
		hlen = snprintf(result, result_cap,
				"Timed out after %d seconds (process killed)\n"
				"--- output (partial) ---\n",
				timeout_seconds);
	} else {
		hlen = snprintf(result, result_cap,
				"Exit code: %d\n--- output ---\n",
				exit_code);
	}

	if (hlen > 0 && (size_t)hlen < result_cap) {
		size_t space = result_cap - (size_t)hlen - 1;
		size_t copy = outlen < space ? outlen : space;
		memcpy(result + hlen, outbuf, copy);
		result[hlen + copy] = '\0';
	}

	free(outbuf);

	if (output_truncated) {
		/* Append truncation notice — reallocate since result_cap was tight */
		const char *note = "\n[Output truncated at 65536 bytes]";
		size_t cur = strlen(result);
		size_t note_len = strlen(note);
		char *extended = realloc(result, cur + note_len + 1);
		if (extended) {
			memcpy(extended + cur, note, note_len + 1);
			result = extended;
		}
	}

	*timed_out_out = timed_out;
	return result;
}

/* -------------------------------------------------------------------------
 * Public tool function
 * ---------------------------------------------------------------------- */

struct tool_result tool_run_fn(const char *arguments_json,
			       const struct agent_configuration *cfg,
			       struct memory_arena *scratch)
{
	(void)scratch;
	struct run_args args;

	if (!arguments_json)
		return tool_result_errorf("run", "Missing arguments");

	if (parse_run_args(arguments_json, &args) != 0)
		return tool_result_errorf("run", "Invalid arguments");

	char resolved[PATH_MAX];
	struct tool_result sandbox_err;
	if (tools_sandbox_resolve(cfg, args.path, resolved, sizeof(resolved),
				  &sandbox_err) != 0) {
		free(args.stdin_data);
		return sandbox_err;
	}

	struct stat st;
	if (stat(resolved, &st) != 0) {
		free(args.stdin_data);
		return tool_result_errorf("run", "File not found: %s", args.path);
	}
	if (S_ISDIR(st.st_mode)) {
		free(args.stdin_data);
		return tool_result_errorf("run",
					  "Path is a directory: %s", args.path);
	}

	/* Build argv - shebang takes priority over extension */
	char *user_argv[USER_ARGS_MAX + 1];
	for (int i = 0; i < args.user_argc; i++)
		user_argv[i] = args.user_args[i];

	char *exec_argv[RUN_MAX_ARGV];
	char shebang_buf[512];
	int argc;

	argc = build_argv_shebang(resolved,
				  (char *const *)user_argv, args.user_argc,
				  exec_argv, RUN_MAX_ARGV,
				  shebang_buf, sizeof(shebang_buf));

	if (argc < 0) {
		const char *interp = interp_from_extension(resolved);
		if (!interp) {
			free(args.stdin_data);
			return tool_result_errorf("run",
						  "Cannot determine interpreter for '%s'. "
						  "Add a shebang (e.g. #!/usr/bin/env python3) "
						  "or use a recognised extension: "
						  ".py .js .sh .rb .lua .pl .php",
						  args.path);
		}
		argc = 0;
		exec_argv[argc++] = (char *)interp;
		exec_argv[argc++] = resolved;
		for (int i = 0; i < args.user_argc && argc < RUN_MAX_ARGV - 1; i++)
			exec_argv[argc++] = user_argv[i];
		exec_argv[argc] = NULL;
	}

	/* Clamp timeout: per-call request cannot exceed the config ceiling */
	int timeout = args.timeout_seconds;
	if (cfg->run_timeout_seconds > 0 && timeout > cfg->run_timeout_seconds)
		timeout = cfg->run_timeout_seconds;

	/* Working directory: the directory containing the script */
	char dir_buf[PATH_MAX];
	snprintf(dir_buf, sizeof(dir_buf), "%s", resolved);
	char *last_slash = strrchr(dir_buf, '/');
	if (last_slash && last_slash != dir_buf)
		*last_slash = '\0';
	else
		snprintf(dir_buf, sizeof(dir_buf), "%s", cfg->sandbox_root);

	log_info("run: %s (interpreter=%s, timeout=%ds)",
		 resolved, exec_argv[0], timeout);

	int timed_out = 0;
	char *output = exec_child((char *const *)exec_argv, dir_buf,
				  args.stdin_data, args.stdin_len,
				  timeout, &timed_out);
	free(args.stdin_data);

	if (!output)
		return tool_result_errorf("run", "Failed to launch process");

	return tool_result_ok("run", output);
}
