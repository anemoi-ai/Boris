/*
 * tools/run_internal.h - Shared between run.c and its Windows launcher.
 *
 * run.c holds the tool itself and the POSIX fork/exec path. On Windows,
 * choosing an interpreter and starting a process are different enough to
 * live in their own file, run_win32.c.
 */

#ifndef RUN_INTERNAL_H
#define RUN_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#define RUN_MAX_OUTPUT 65536

/* Combined stdout+stderr of the child. */
struct child_output {
	char *buf; /* RUN_MAX_OUTPUT + 1 bytes */
	size_t len;
	bool truncated;
};

/*
 * Build the text returned to the model: a header line (exit code, timeout
 * or cancellation), the captured output and a truncation note. finished is
 * false when the child had to be killed. Returns a heap string or NULL.
 */
char *run_format_result(const struct child_output *out, bool finished, bool cancelled,
			int exit_code, int timeout_seconds);

#ifdef _WIN32
/*
 * Run script (an absolute path inside the sandbox) with the interpreter its
 * shebang line or extension calls for, in directory cwd, with environment
 * envp ("NAME=value" strings, NULL terminated).
 *
 * Returns the formatted result (see run_format_result), or NULL with a
 * message for the model in problem.
 */
char *run_win32_script(const char *script, char *const *user_args, int user_argc,
		       char *const *envp, const char *cwd, const char *stdin_data,
		       size_t stdin_len, int timeout_seconds, char *problem,
		       size_t problem_size);
#endif

#endif /* RUN_INTERNAL_H */
