/*
 * exec.h - Non-interactive mode: run one task and report the result.
 *
 * `boris exec "task"` builds a fresh conversation (no saved session),
 * runs the agent loop once, prints the answer (or a JSON report) and
 * exits with a status describing how the run ended.
 */

#ifndef EXEC_H
#define EXEC_H

#include <stdbool.h>
#include <stdio.h>
#include "boris_types.h"

struct conversation_history;

/* Exit statuses of `boris exec`. */
#define EXEC_EXIT_OK		 0   /* finished naturally */
#define EXEC_EXIT_ERROR		 1   /* model, network or configuration error */
#define EXEC_EXIT_MAX_ITERATIONS 2   /* stopped at agent.max_iterations */
#define EXEC_EXIT_USAGE		 64  /* bad command line (EX_USAGE) */
#define EXEC_EXIT_INTERRUPTED	 130 /* Ctrl+C */

struct exec_options {
	bool json;	 /* print a JSON report instead of the answer */
	bool show_tools; /* print one line per tool call to err */
	FILE *out;	 /* answer or JSON report (stdout) */
	FILE *err;	 /* errors and tool activity (stderr) */
};

/*
 * Build the conversation for one exec run and append task as the user's
 * message. With session_path naming an existing file, that conversation
 * is restored and its system prompt replaced by system_prompt (as the REPL
 * does on start); otherwise a new conversation starts with system_prompt.
 * Returns NULL (with a message on err) if a session file exists but cannot
 * be loaded, so a corrupt session is never silently overwritten.
 */
struct conversation_history *exec_conversation(const char *session_path,
					       const char *system_prompt,
					       const char *task, FILE *err);

/*
 * Run the agent on conv, which must end with the user's task, and report
 * the outcome on opts->out. Tools that need confirmation are refused,
 * since there is nobody to ask. Returns one of the EXEC_EXIT_* statuses.
 * Tools must already be registered.
 *
 * The JSON report has the keys: response, finish_reason ("natural",
 * "max_iterations", "error", "interrupted"), iterations, usage
 * {prompt_tokens, completion_tokens}, tool_calls [{name, summary,
 * is_error}], error (string or null).
 */
int exec_run(struct conversation_history *conv, const struct agent_configuration *cfg,
	     const struct exec_options *opts);

#endif /* EXEC_H */
