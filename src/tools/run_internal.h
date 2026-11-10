/*
 * tools/run_internal.h - Shared between run.c and its Windows launcher.
 *
 * run.c holds the tool itself and the POSIX fork/exec path. On Windows,
 * choosing an interpreter and starting a process are different enough to
 * live in their own file, run_win32.c.
 */

#ifndef RUN_INTERNAL_H
#define RUN_INTERNAL_H

#include "boris_types.h"
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

#ifndef _WIN32
/*
 * Confinement of the script by the operating system (run_confine.c): the
 * script can read the system and its interpreter, and read and write only
 * the sandbox and a temporary directory of its own.
 */
struct run_confinement {
	bool active;	    /* false: unavailable or switched off; run unconfined */
	int ruleset_fd;	    /* Landlock rules for the child to adopt */
	char tmp_dir[4096]; /* the script's temporary directory (its TMPDIR) */
};

/* In the parent, before fork(). interpreter is argv[0]; path_var the PATH
 * the script will be given, used to find it. */
void run_confine_prepare(const struct agent_configuration *cfg, const char *sandbox_root,
			 const char *interpreter, const char *path_var,
			 struct run_confinement *out);

/* In the child, just before exec. Returns 0, or -1 with errno set, in which
 * case the script must not be started. */
int run_confine_child(const struct run_confinement *c);

/* In the parent, after fork(). */
void run_confine_release(struct run_confinement *c);
#endif

#ifdef _WIN32
/*
 * Prepare to run scripts at low integrity, the Windows mechanism that stops
 * a process writing anywhere not explicitly opened to it: marks the sandbox
 * root (once) as writable by low-integrity processes and makes a temporary
 * directory that is too. Returns false, having logged why, if scripts will
 * have to run unconfined.
 */
bool run_win32_confine_prepare(const char *sandbox_root, char *tmp_dir, size_t tmp_size);

/*
 * Run script (an absolute path inside the sandbox) with the interpreter its
 * shebang line or extension calls for, in directory cwd, with environment
 * envp ("NAME=value" strings, NULL terminated). With low_integrity the
 * process cannot write outside what run_win32_confine_prepare() opened up.
 *
 * Returns the formatted result (see run_format_result), or NULL with a
 * message for the model in problem.
 */
char *run_win32_script(const char *script, char *const *user_args, int user_argc,
		       char *const *envp, const char *cwd, const char *stdin_data,
		       size_t stdin_len, int timeout_seconds, bool low_integrity,
		       char *problem, size_t problem_size);
#endif

#endif /* RUN_INTERNAL_H */
