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
 *   - The child gets an allowlisted environment, not Boris's own: secrets
 *     such as BORIS_API_KEY are never visible to the script
 *
 * The script itself still runs with the user's permissions; the sandbox
 * only constrains which script is started and its working directory.
 */
#define _POSIX_C_SOURCE 200809L

#include "tools.h"
#include "tool_builtins.h"
#include "arena.h"
#include "configuration.h"
#include "sandbox.h"
#include "json.h"
#include "logger.h"
#include "fs.h"
#include <stdbool.h>
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

extern char **environ;

#define RUN_MAX_OUTPUT	    65536
#define RUN_MAX_ARGV	    40	 /* interpreter parts + script + user args + NULL */
#define RUN_DEFAULT_TIMEOUT 30	 /* seconds */
#define RUN_MAX_TIMEOUT	    120	 /* hard cap regardless of config */
#define RUN_MAX_STDIN	    8192 /* max stdin bytes accepted */
#define USER_ARGS_MAX	    16
#define USER_ARG_LEN	    512
#define RUN_ENV_MAX	    64	 /* entries in the child's environment */

/* -------------------------------------------------------------------------
 * Child environment
 * ---------------------------------------------------------------------- */

/* Variables passed through to scripts. Everything else, including every
 * BORIS_* variable, is dropped. tools.run_env adds names to this list. */
static const char *const RUN_ENV_ALLOW[] = {
	"PATH", "HOME", "USER", "LOGNAME", "LANG", "LC_ALL", "LC_CTYPE",
	"TERM", "TMPDIR", "TZ",
};

#define RUN_DEFAULT_PATH "/usr/local/bin:/usr/bin:/bin"

static bool env_has(char *const *envp, size_t count, const char *name)
{
	size_t len = strlen(name);
	for (size_t i = 0; i < count; i++)
		if (strncmp(envp[i], name, len) == 0 && envp[i][len] == '=')
			return true;
	return false;
}

static void env_add(char **envp, size_t *count, const char *name,
		    const char *value, struct memory_arena *scratch)
{
	if (*count >= RUN_ENV_MAX - 1 || env_has(envp, *count, name))
		return;
	size_t len = strlen(name) + 1 + strlen(value) + 1;
	char *entry = arena_allocate(scratch, len);
	if (!entry)
		return;
	snprintf(entry, len, "%s=%s", name, value);
	envp[(*count)++] = entry;
}

static void env_pass(char **envp, size_t *count, const char *name,
		     struct memory_arena *scratch)
{
	const char *value = getenv(name);
	if (value)
		env_add(envp, count, name, value, scratch);
}

/*
 * Fill envp (RUN_ENV_MAX entries) with the child's environment, NULL
 * terminated. Built in the parent so the child does no allocation.
 */
static void build_child_env(const struct agent_configuration *cfg,
			    char **envp, struct memory_arena *scratch)
{
	size_t count = 0;
	for (size_t i = 0; i < sizeof(RUN_ENV_ALLOW) / sizeof(RUN_ENV_ALLOW[0]); i++)
		env_pass(envp, &count, RUN_ENV_ALLOW[i], scratch);

	if (cfg->run_env && cfg->run_env[0]) {
		char names[1024];
		snprintf(names, sizeof(names), "%s", cfg->run_env);
		char *save = NULL;
		for (char *tok = strtok_r(names, ", \t", &save); tok;
		     tok = strtok_r(NULL, ", \t", &save)) {
			if (strncmp(tok, "BORIS_", 6) == 0) {
				log_warning("run: tools.run_env: refusing to pass %s "
					    "to scripts", tok);
				continue;
			}
			env_pass(envp, &count, tok, scratch);
		}
	}

	if (!env_has(envp, count, "PATH"))
		env_add(envp, &count, "PATH", RUN_DEFAULT_PATH, scratch);
	/* Python block-buffers stdout when it is a pipe, so a script killed at
	 * the timeout would report no output at all, hiding where it hung. */
	env_add(envp, &count, "PYTHONUNBUFFERED", "1", scratch);
	envp[count] = NULL;
}

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
	char *stdin_data; /* scratch-arena copy, may be NULL */
	size_t stdin_len;
};

static int parse_run_args(const char *json_str, struct run_args *out,
			  struct memory_arena *scratch)
{
	memset(out, 0, sizeof(*out));
	out->timeout_seconds = RUN_DEFAULT_TIMEOUT;

	cJSON *root = json_parse(json_str);
	if (!root)
		return -1;

	if (tools_arg_path(root, out->path, sizeof(out->path)) != 0) {
		cJSON_Delete(root);
		return -1;
	}

	cJSON *args_node = cJSON_GetObjectItemCaseSensitive(root, "args");
	cJSON *args_arr = cJSON_IsArray(args_node) ? args_node : NULL;
	cJSON *item = NULL;
	cJSON_ArrayForEach(item, args_arr)
	{
		if (out->user_argc >= USER_ARGS_MAX)
			break;
		if (!cJSON_IsString(item))
			continue;
		snprintf(out->user_args[out->user_argc], USER_ARG_LEN, "%s",
			 item->valuestring);
		out->user_argc++;
	}

	cJSON *timeout_node = cJSON_GetObjectItemCaseSensitive(root,
							       "timeout_seconds");
	if (cJSON_IsNumber(timeout_node)) {
		double t = timeout_node->valuedouble;
		out->timeout_seconds = t < 1 ? 1 : t > RUN_MAX_TIMEOUT ? RUN_MAX_TIMEOUT : (int)t;
	}

	const char *stdin_str = json_get_str(root, "stdin");
	if (stdin_str && stdin_str[0]) {
		size_t slen = strlen(stdin_str);
		if (slen > RUN_MAX_STDIN)
			slen = RUN_MAX_STDIN;
		out->stdin_data = arena_duplicate_string_length(scratch, stdin_str, slen);
		out->stdin_len = out->stdin_data ? slen : 0;
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

/* -------------------------------------------------------------------------
 * Child process
 * ---------------------------------------------------------------------- */

/* Longest a single poll() waits, so timeouts and Ctrl+C are noticed promptly. */
#define RUN_POLL_SLICE_MS 200

static _Noreturn void exec_in_child(char *const argv[], char **envp,
				    const char *cwd, int out_fd, int in_fd)
{
	/* Own process group, so a timeout can kill everything the script
	 * started, not just the script itself. */
	setpgid(0, 0);
	signal(SIGPIPE, SIG_DFL);

	dup2(out_fd, STDOUT_FILENO);
	dup2(out_fd, STDERR_FILENO);
	close(out_fd);

	if (in_fd < 0)
		in_fd = open("/dev/null", O_RDONLY);
	if (in_fd >= 0) {
		dup2(in_fd, STDIN_FILENO);
		close(in_fd);
	}

	if (cwd && chdir(cwd) != 0) {
		fprintf(stderr, "run: cannot chdir to '%s': %s\n", cwd, strerror(errno));
		_exit(127);
	}

	/* execvp searches the PATH of the new environment. */
	environ = envp;
	execvp(argv[0], argv);
	fprintf(stderr, "run: cannot execute '%s': %s\n", argv[0], strerror(errno));
	_exit(127);
}

/* Write stdin data to the child, ignoring SIGPIPE: a script that exits
 * without reading its input must not kill Boris. */
static void feed_stdin(int fd, const char *data, size_t len)
{
	struct sigaction ign, old;
	memset(&ign, 0, sizeof(ign));
	ign.sa_handler = SIG_IGN;
	sigemptyset(&ign.sa_mask);
	sigaction(SIGPIPE, &ign, &old);
	fs_write_all(fd, data, len); /* EPIPE just means the child stopped reading */
	sigaction(SIGPIPE, &old, NULL);
	close(fd);
}

struct child_output {
	char *buf; /* RUN_MAX_OUTPUT + 1 bytes */
	size_t len;
	bool truncated;
};

/* Read combined output until EOF, the deadline, or cancellation.
 * Returns false if the child must be killed (timeout or Ctrl+C). */
static bool collect_output(int fd, struct child_output *out,
			   const struct timespec *start, int timeout_seconds)
{
	for (;;) {
		long remaining = (long)timeout_seconds * 1000 - elapsed_ms(start);
		if (remaining <= 0 || tools_cancelled())
			return false;

		struct pollfd pfd = {.fd = fd, .events = POLLIN};
		int r = poll(&pfd, 1, remaining < RUN_POLL_SLICE_MS ? (int)remaining
								     : RUN_POLL_SLICE_MS);
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0)
			return true; /* treat as EOF; waitpid below still applies */
		if (r == 0)
			continue;

		char drain[4096];
		size_t space = RUN_MAX_OUTPUT - out->len;
		char *dst = space > 0 ? out->buf + out->len : drain;
		ssize_t n = read(fd, dst, space > 0 ? space : sizeof(drain));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return true; /* EOF: child closed its output */
		if (space > 0)
			out->len += (size_t)n;
		else
			out->truncated = true;
	}
}

/* Wait for the child to exit, still honouring the deadline: a script can
 * close its output and keep running. The child is left unreaped (WNOWAIT)
 * so its process-group ID cannot be reused before we kill the group.
 * Returns false on timeout/cancel. */
static bool wait_for_exit(pid_t pid, const struct timespec *start,
			  int timeout_seconds)
{
	for (;;) {
		siginfo_t info;
		memset(&info, 0, sizeof(info));
		int r = waitid(P_PID, (id_t)pid, &info, WEXITED | WNOHANG | WNOWAIT);
		if ((r == 0 && info.si_pid == pid) || (r < 0 && errno != EINTR))
			return true;
		if (elapsed_ms(start) >= (long)timeout_seconds * 1000 || tools_cancelled())
			return false;
		struct timespec ts = {0, 20 * 1000000L};
		nanosleep(&ts, NULL);
	}
}

/*
 * fork/exec argv, capture combined stdout+stderr with a timeout.
 *
 * cwd is the working directory for the child (may be NULL to inherit).
 * Returns a heap-allocated result string, or NULL on internal failure.
 */
static char *exec_child(char *const argv[], char **envp, const char *cwd,
			const char *stdin_data, size_t stdin_len,
			int timeout_seconds)
{
	int out_pipe[2]; /* parent reads [0], child writes [1] */
	int in_pipe[2] = {-1, -1}; /* parent writes [1], child reads [0] */
	bool has_stdin = stdin_data && stdin_len > 0;

	struct child_output out = {.buf = malloc(RUN_MAX_OUTPUT + 1)};
	if (!out.buf)
		return NULL;

	if (pipe(out_pipe) != 0) {
		free(out.buf);
		return NULL;
	}
	if (has_stdin && pipe(in_pipe) != 0) {
		close(out_pipe[0]);
		close(out_pipe[1]);
		free(out.buf);
		return NULL;
	}

	pid_t pid = fork();
	if (pid == 0) {
		close(out_pipe[0]);
		if (has_stdin)
			close(in_pipe[1]);
		exec_in_child(argv, envp, cwd, out_pipe[1], in_pipe[0]);
	}

	close(out_pipe[1]);
	if (has_stdin)
		close(in_pipe[0]);
	if (pid < 0) {
		close(out_pipe[0]);
		if (has_stdin)
			close(in_pipe[1]);
		free(out.buf);
		return NULL;
	}
	setpgid(pid, pid); /* also in the parent, to win the race with exec */

	if (has_stdin)
		feed_stdin(in_pipe[1], stdin_data, stdin_len);

	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);

	int status = 0;
	bool finished = collect_output(out_pipe[0], &out, &start, timeout_seconds) &&
			wait_for_exit(pid, &start, timeout_seconds);
	close(out_pipe[0]);
	out.buf[out.len] = '\0';

	/* Kill the whole group: on timeout/cancel, and also after a normal exit
	 * so background processes the script started do not outlive it. */
	kill(-pid, SIGKILL);
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;

	bool cancelled = !finished && tools_cancelled();
	int exit_code = WIFEXITED(status)   ? WEXITSTATUS(status)
			: WIFSIGNALED(status) ? 128 + WTERMSIG(status)
					      : 0;

	/* Format result: header + captured output + optional note */
	size_t result_cap = out.len + 256;
	char *result = malloc(result_cap);
	if (!result) {
		free(out.buf);
		return NULL;
	}

	int hlen;
	if (cancelled)
		hlen = snprintf(result, result_cap,
				"Cancelled by the user (process killed)\n"
				"--- output (partial) ---\n");
	else if (!finished)
		hlen = snprintf(result, result_cap,
				"Timed out after %d seconds (process killed)\n"
				"--- output (partial) ---\n",
				timeout_seconds);
	else
		hlen = snprintf(result, result_cap,
				"Exit code: %d\n--- output ---\n", exit_code);

	memcpy(result + hlen, out.buf, out.len);
	size_t pos = (size_t)hlen + out.len;
	if (out.truncated)
		snprintf(result + pos, result_cap - pos,
			 "\n[Output truncated at %d bytes]", RUN_MAX_OUTPUT);
	else
		result[pos] = '\0';

	free(out.buf);
	return result;
}

/* -------------------------------------------------------------------------
 * Public tool function
 * ---------------------------------------------------------------------- */

struct tool_result tool_run_fn(const char *arguments_json,
			       const struct agent_configuration *cfg,
			       struct memory_arena *scratch)
{
	struct run_args args;

	if (!arguments_json)
		return tool_result_errorf("run", "Missing arguments");

	if (parse_run_args(arguments_json, &args, scratch) != 0)
		return tool_result_errorf("run", "Invalid arguments");

	char resolved[PATH_MAX];
	struct tool_result sandbox_err;
	if (tools_sandbox_resolve(cfg, args.path, resolved, sizeof(resolved),
				  &sandbox_err) != 0)
		return sandbox_err;

	struct stat st;
	if (stat(resolved, &st) != 0)
		return tool_result_errorf("run", "File not found: %s", args.path);
	if (!S_ISREG(st.st_mode))
		return tool_result_errorf("run", "Not a regular file: %s", args.path);

	/* Build argv - shebang takes priority over extension */
	char *user_argv[USER_ARGS_MAX + 1];
	for (int i = 0; i < args.user_argc; i++)
		user_argv[i] = args.user_args[i];

	char *exec_argv[RUN_MAX_ARGV];
	char shebang_buf[512];
	int argc = build_argv_shebang(resolved,
				      (char *const *)user_argv, args.user_argc,
				      exec_argv, RUN_MAX_ARGV,
				      shebang_buf, sizeof(shebang_buf));

	if (argc < 0) {
		const char *interp = interp_from_extension(resolved);
		if (!interp)
			return tool_result_errorf("run",
						  "Cannot determine interpreter for '%s'. "
						  "Add a shebang (e.g. #!/usr/bin/env python3) "
						  "or use a recognised extension: "
						  ".py .js .sh .rb .lua .pl .php",
						  args.path);
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

	char *envp[RUN_ENV_MAX];
	build_child_env(cfg, envp, scratch);

	char *output = exec_child((char *const *)exec_argv, envp, dir_buf,
				  args.stdin_data, args.stdin_len, timeout);
	if (!output)
		return tool_result_errorf("run", "Failed to launch process");

	return tool_result_ok("run", output);
}

/* "build.sh --fast (stdin: 120 bytes)". */
void tool_run_format(const char *argument_json, char *buf, size_t size)
{
	cJSON *root = json_parse(argument_json);
	const char *path = json_get_str(root, "path");
	if (!path) {
		snprintf(buf, size, "(invalid arguments)");
		cJSON_Delete(root);
		return;
	}

	size_t pos = (size_t)snprintf(buf, size, "%s", path);
	cJSON *args = cJSON_GetObjectItemCaseSensitive(root, "args");
	cJSON *arr = cJSON_IsArray(args) ? args : NULL;
	cJSON *item = NULL;
	cJSON_ArrayForEach(item, arr)
	{
		if (pos >= size)
			break;
		if (cJSON_IsString(item))
			pos += (size_t)snprintf(buf + pos, size - pos, " %s", item->valuestring);
	}
	const char *stdin_str = json_get_str(root, "stdin");
	if (stdin_str && stdin_str[0] && pos < size)
		snprintf(buf + pos, size - pos, " (stdin: %zu bytes)", strlen(stdin_str));
	cJSON_Delete(root);
}

/* -------------------------------------------------------------------------
 * Definition
 * ---------------------------------------------------------------------- */

static const char SCHEMA_RUN[] =
	"{"
	"\"type\":\"object\","
	"\"properties\":{"
	"\"path\":{\"type\":\"string\","
	"\"description\":\"Path to the script file to execute. "
	"Relative paths are resolved within the sandbox root.\"},"
	"\"args\":{\"type\":\"array\","
	"\"items\":{\"type\":\"string\"},"
	"\"description\":\"Command-line arguments to pass to the script.\","
	"\"default\":[]},"
	"\"stdin\":{\"type\":\"string\","
	"\"description\":\"Data to send to the process on stdin.\","
	"\"default\":\"\"},"
	"\"timeout_seconds\":{\"type\":\"integer\","
	"\"description\":\"Maximum execution time in seconds (1-120).\","
	"\"default\":30}"
	"},"
	"\"required\":[\"path\"]"
	"}";

const struct tool_definition tool_run_definition = {
	.name = "run",
	.description = "Execute a script file within the sandbox and return its output. "
		       "The interpreter is detected from the shebang line or file extension "
		       "(.py, .js, .sh, .rb, .lua, .pl, .php). "
		       "Returns the exit code and combined stdout/stderr.",
	.parameters_schema = SCHEMA_RUN,
	.fn = tool_run_fn,
	.format_call = tool_run_format,
	.flag = TOOL_RUN,
	.confirm = TOOL_CONFIRM_RUN,
};
