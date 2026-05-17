/*
 * repl.c - Interactive read-eval-print loop.
 *
 * Uses the line editor for cursor movement, history, and tab completion.
 * Colours every prompt and response via the terminal abstraction layer.
 * Sessions are auto-loaded on start and saved on /quit or SIGINT.
 */
#define _POSIX_C_SOURCE 200809L

#include "repl.h"
#include "agent.h"
#include "markdown.h"
#include "tools.h"
#include "logger.h"
#include "terminal.h"
#include "editor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>

#define CONTEXT_WARN_PCT 75.0

/* -------------------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------------- */

struct repl_state {
	struct conversation_history *conv;
	const struct agent_configuration *cfg;
	int should_exit;
};

static volatile sig_atomic_t g_sigint = 0;

static void sigint_handler(int sig)
{
	(void)sig;
	g_sigint = 1;
}

/* -------------------------------------------------------------------------
 * Spinner - animated braille dots while the agent is thinking
 * ---------------------------------------------------------------------- */

static volatile sig_atomic_t g_spinner_active = 0;
static volatile sig_atomic_t g_spinner_frame = 0;
static struct sigaction g_old_sigalrm;

/* Braille spinner characters (3 bytes each in UTF-8) */
static const char g_spinner_chars[] =
	"\xe2\xa0\x8b"
	"\xe2\xa0\x99"
	"\xe2\xa0\xb9"
	"\xe2\xa0\xb8"
	"\xe2\xa0\xbc"
	"\xe2\xa0\xb4"
	"\xe2\xa0\xa6"
	"\xe2\xa0\xa7"
	"\xe2\xa0\x87"
	"\xe2\xa0\x8f";
#define SPIN_FRAMES 10
#define SPIN_BYTES  3

static void spinner_handler(int sig)
{
	(void)sig;
	if (!g_spinner_active)
		return;
	ssize_t r;
	r = write(STDOUT_FILENO, "\r\033[K  Boris is thinking ", 25);
	r = write(STDOUT_FILENO,
		  &g_spinner_chars[g_spinner_frame * SPIN_BYTES], SPIN_BYTES);
	(void)r;
	g_spinner_frame = (g_spinner_frame + 1) % SPIN_FRAMES;
}

static void spinner_start(void)
{
	if (!term_is_tty()) {
		printf("  Boris is thinking...");
		fflush(stdout);
		return;
	}
	g_spinner_active = 1;
	g_spinner_frame = 0;
	term_cursor_hide();
	fflush(stdout);
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = spinner_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	sigaction(SIGALRM, &sa, &g_old_sigalrm);
	struct itimerval it;
	it.it_interval.tv_sec = 0;
	it.it_interval.tv_usec = 80000;
	it.it_value.tv_sec = 0;
	it.it_value.tv_usec = 80000;
	setitimer(ITIMER_REAL, &it, NULL);
}

static void spinner_stop(void)
{
	if (!g_spinner_active)
		return;
	struct itimerval it;
	memset(&it, 0, sizeof(it));
	setitimer(ITIMER_REAL, &it, NULL);
	sigaction(SIGALRM, &g_old_sigalrm, NULL);
	g_spinner_active = 0;
	printf("\r\033[K");
	term_cursor_show();
	fflush(stdout);
}

/* -------------------------------------------------------------------------
 * Confirmation prompts
 * ---------------------------------------------------------------------- */

static bool confirm_action(const char *message)
{
	printf("  %s ", message);
	term_printf_color(TERM_FG_BRIGHT_GREEN, "[y");
	printf("/");
	term_printf_color(TERM_FG_BRIGHT_RED, "N");
	printf("] ");
	fflush(stdout);
	int c = getchar();
	bool ok = (c == 'y' || c == 'Y');
	while (c != '\n' && c != EOF)
		c = getchar();
	return ok;
}

/* -------------------------------------------------------------------------
 * Levenshtein distance - for "did you mean?" suggestions
 * ---------------------------------------------------------------------- */

static int levenshtein(const char *a, const char *b)
{
	int la = (int)strlen(a), lb = (int)strlen(b);
	if (la == 0)
		return lb;
	if (lb == 0)
		return la;
	int *dp = calloc((size_t)(la + 1) * (size_t)(lb + 1), sizeof(int));
	if (!dp)
		return 999;
#define D(i, j) dp[(i) * (lb + 1) + (j)]
	for (int i = 0; i <= la; i++)
		D(i, 0) = i;
	for (int j = 0; j <= lb; j++)
		D(0, j) = j;
	for (int i = 1; i <= la; i++)
		for (int j = 1; j <= lb; j++) {
			int cost = (a[i - 1] != b[j - 1]) ? 1 : 0;
			int v = D(i - 1, j) + 1;
			if (D(i, j - 1) + 1 < v)
				v = D(i, j - 1) + 1;
			if (D(i - 1, j - 1) + cost < v)
				v = D(i - 1, j - 1) + cost;
			D(i, j) = v;
		}
	int r = D(la, lb);
#undef D
	free(dp);
	return r;
}

/* -------------------------------------------------------------------------
 * Slash command table
 * ---------------------------------------------------------------------- */

struct slash_command {
	const char *name;
	const char *alias;
	const char *usage;
	const char *description;
	void (*handler)(struct repl_state *, const char *);
};

static void cmd_help(struct repl_state *st, const char *arg);
static void cmd_status(struct repl_state *st, const char *arg);
static void cmd_tokens(struct repl_state *st, const char *arg);
static void cmd_clear(struct repl_state *st, const char *arg);
static void cmd_reset(struct repl_state *st, const char *arg);
static void cmd_tools(struct repl_state *st, const char *arg);
static void cmd_save(struct repl_state *st, const char *arg);
static void cmd_load(struct repl_state *st, const char *arg);
static void cmd_quit(struct repl_state *st, const char *arg);

static const struct slash_command commands[] = {
	{"help", NULL, "", "Show available commands", cmd_help},
	{"status", NULL, "", "Show conversation stats", cmd_status},
	{"tokens", NULL, "", "Estimate token usage", cmd_tokens},
	{"clear", NULL, "", "Clear conversation", cmd_clear},
	{"reset", NULL, "", "Reset + reload system prompt", cmd_reset},
	{"save", NULL, "[file]", "Save conversation to file", cmd_save},
	{"load", NULL, "[file]", "Load conversation from file", cmd_load},
	{"tools", NULL, "", "List enabled tools", cmd_tools},
	{"quit", "exit", "", "Exit Boris", cmd_quit},
	{NULL, NULL, NULL, NULL, NULL}};

/* -------------------------------------------------------------------------
 * Command implementations
 * ---------------------------------------------------------------------- */

static void cmd_help(struct repl_state *st, const char *arg)
{
	(void)st;
	(void)arg;
	printf("\n");
	term_print_bold("  Available commands:\n");
	for (int i = 0; commands[i].name; i++) {
		term_printf_color(TERM_FG_BRIGHT_GREEN, "    /%-8s", commands[i].name);
		if (commands[i].usage && commands[i].usage[0])
			printf("%-10s", commands[i].usage);
		else
			printf("          ");
		term_printf_color(TERM_FG_BRIGHT_BLACK, "— %s\n", commands[i].description);
	}
	printf("\n");
	term_print_dim("  Type your message and press Enter to send.\n");
	term_print_dim("  End a line with \\ to continue on the next line.\n");
	printf("\n");
}

static void cmd_status(struct repl_state *st, const char *arg)
{
	(void)arg;
	const struct conversation_history *conv = st->conv;
	const struct agent_configuration *cfg = st->cfg;
	if (!conv) {
		printf("  No conversation active.\n");
		return;
	}

	size_t tokens = conversation_estimate_tokens(conv);
	double usage = cfg->context_window > 0
			       ? (double)tokens / cfg->context_window * 100.0
			       : 0.0;

	printf("\n");
	term_print_bold("  Conversation:\n");
	printf("    Messages      : %zu\n", conv->count);
	printf("    Characters    : %zu\n", conv->total_characters);
	printf("    Est. tokens   : %zu\n", tokens);
	printf("    Context window: %d tokens\n", cfg->context_window);
	printf("    Usage         : %.1f%%\n", usage);
	printf("\n");
	term_print_bold("  Configuration:\n");
	printf("    Model         : %s\n",
	       cfg->model_name ? cfg->model_name : "(not set)");
	printf("    Endpoint      : %s\n",
	       cfg->model_endpoint ? cfg->model_endpoint : "(not set)");
	printf("    Max iterations: %d\n", cfg->max_iterations);
	printf("    Sandbox       : %s\n",
	       cfg->sandbox_root ? cfg->sandbox_root : "(not set)");
	{
		char tools_buf[128];
		tools_mask_to_string(cfg->tools_enabled, tools_buf, sizeof(tools_buf));
		printf("    Tools         : %s\n", tools_buf);
	}
	printf("\n");
}

static void cmd_tokens(struct repl_state *st, const char *arg)
{
	(void)arg;
	const struct conversation_history *conv = st->conv;
	if (!conv) {
		printf("  No conversation active.\n\n");
		return;
	}
	size_t tokens = conversation_estimate_tokens(conv);
	double usage = st->cfg->context_window > 0
			       ? (double)tokens / st->cfg->context_window * 100.0
			       : 0.0;
	printf("  Estimated tokens: %zu / %d (%.1f%%)\n\n",
	       tokens, st->cfg->context_window, usage);
}

static void cmd_clear(struct repl_state *st, const char *arg)
{
	(void)arg;
	if (!st->conv)
		return;
	if (!confirm_action("Clear conversation?")) {
		term_print_dim("  Cancelled.\n\n");
		return;
	}
	conversation_clear(st->conv);
	term_print_color(TERM_FG_BRIGHT_GREEN, "  Conversation cleared. System prompt preserved.\n\n");
}

static void cmd_reset(struct repl_state *st, const char *arg)
{
	(void)arg;
	struct conversation_history *conv = st->conv;
	const struct agent_configuration *cfg = st->cfg;
	if (!conv)
		return;
	if (!confirm_action("Reset conversation?")) {
		term_print_dim("  Cancelled.\n\n");
		return;
	}
	conversation_clear(conv);
	const char *np = (cfg->system_prompt && cfg->system_prompt[0])
				 ? cfg->system_prompt
				 : "You are Boris, a helpful AI companion.";
	conversation_replace_system_prompt(conv, np);
	term_print_color(TERM_FG_BRIGHT_GREEN,
			 "  Conversation reset. System prompt reloaded from config.\n\n");
}

static void cmd_tools(struct repl_state *st, const char *arg)
{
	(void)arg;
	size_t num_tools = 0;
	const struct tool_definition *tools = tools_get_all(&num_tools);
	printf("\n");
	term_print_bold("  Registered tools:\n");
	for (size_t i = 0; i < num_tools; i++) {
		const char *status =
			(st->cfg->tools_enabled & tools[i].flag) ? "enabled" : "disabled";
		term_printf_color(TERM_FG_BRIGHT_GREEN, "    %-8s", status);
		printf(" %-12s %s\n", tools[i].name, tools[i].description);
	}
	printf("\n");
}

static void cmd_save(struct repl_state *st, const char *arg)
{
	char *path = (arg && arg[0]) ? strdup(arg) : conversation_session_path();
	if (!path) {
		printf("  Cannot determine save path. Set HOME or XDG_DATA_HOME.\n\n");
		return;
	}
	if (st->conv && conversation_save(st->conv, path) == BORIS_OK)
		printf("  Saved %zu messages to %s\n\n", st->conv->count, path);
	else
		printf("  Failed to save conversation.\n\n");
	free(path);
}

static void cmd_load(struct repl_state *st, const char *arg)
{
	char *path = (arg && arg[0]) ? strdup(arg) : conversation_session_path();
	if (!path) {
		printf("  Cannot determine load path. Set HOME or XDG_DATA_HOME.\n\n");
		return;
	}
	struct conversation_history *loaded = conversation_load(path);
	if (loaded) {
		if (st->conv) {
			if (conversation_clone(st->conv, loaded) != BORIS_OK) {
				fprintf(stderr, "  Could not allocate memory.\n");
				conversation_destroy(loaded);
				free(path);
				return;
			}
		}
		conversation_destroy(loaded);
		printf("  Loaded %zu messages from %s\n\n",
		       st->conv ? st->conv->count : 0, path);
	} else {
		printf("  No saved conversation found at %s\n\n", path);
	}
	free(path);
}

static void cmd_quit(struct repl_state *st, const char *arg)
{
	(void)arg;
	if (st->conv && st->conv->count > 1) {
		char *session = conversation_session_path();
		if (session) {
			if (conversation_save(st->conv, session) == BORIS_OK)
				printf("  Conversation saved to %s\n", session);
			free(session);
		}
	}
	printf("\n");
	term_print_color(TERM_FG_BRIGHT_GREEN, "  Goodbye for now.\n");
	printf("\n");
	st->should_exit = 1;
}

/* -------------------------------------------------------------------------
 * Slash command dispatcher (table-driven)
 * ---------------------------------------------------------------------- */

static int handle_command(const char *line, struct repl_state *st)
{
	const char *cmd = (line[0] == '/') ? line + 1 : line;
	char name[64];
	const char *arg = "";
	const char *space = strchr(cmd, ' ');
	if (space) {
		size_t nlen = (size_t)(space - cmd);
		if (nlen >= sizeof(name))
			nlen = sizeof(name) - 1;
		memcpy(name, cmd, nlen);
		name[nlen] = '\0';
		arg = space + 1;
		while (*arg == ' ')
			arg++;
	} else {
		snprintf(name, sizeof(name), "%s", cmd);
	}

	for (int i = 0; commands[i].name; i++) {
		if (strcmp(name, commands[i].name) == 0 ||
		    (commands[i].alias && strcmp(name, commands[i].alias) == 0)) {
			commands[i].handler(st, arg);
			return st->should_exit;
		}
	}

	term_printf_color(TERM_FG_BRIGHT_RED, "  Unknown command: /%s\n", name);
	int best = 999;
	const char *suggest = NULL;
	for (int i = 0; commands[i].name; i++) {
		int d = levenshtein(name, commands[i].name);
		if (d < best) {
			best = d;
			suggest = commands[i].name;
		}
		if (commands[i].alias) {
			int da = levenshtein(name, commands[i].alias);
			if (da < best) {
				best = da;
				suggest = commands[i].alias;
			}
		}
	}
	if (best <= 2 && suggest)
		term_printf_color(TERM_FG_BRIGHT_YELLOW,
				  "  Did you mean: /%s\n", suggest);
	printf("\n");
	return 0;
}

/* -------------------------------------------------------------------------
 * Tab completion callback (slash commands)
 * ---------------------------------------------------------------------- */

static int slash_completion(const char *input, char *completion,
			    size_t comp_size, void *ctx)
{
	(void)ctx;
	if (!input || input[0] != '/')
		return 0;
	const char *partial = input + 1;
	size_t plen = strlen(partial);
	int count = 0;
	const char *last_match = NULL;
	for (int i = 0; commands[i].name; i++) {
		if (strncmp(commands[i].name, partial, plen) == 0) {
			count++;
			last_match = commands[i].name;
		}
	}
	if (count == 1) {
		snprintf(completion, comp_size, "/%s ", last_match);
		return 1;
	}
	if (count > 1) {
		printf("\n");
		for (int i = 0; commands[i].name; i++) {
			if (strncmp(commands[i].name, partial, plen) == 0) {
				term_printf_color(TERM_FG_BRIGHT_GREEN,
						  "  /%s", commands[i].name);
				if (commands[i].usage && commands[i].usage[0])
					printf(" %s", commands[i].usage);
				term_printf_color(TERM_FG_BRIGHT_BLACK,
						  " — %s\n", commands[i].description);
			}
		}
		return count;
	}
	return 0;
}

/* -------------------------------------------------------------------------
 * Context window warning
 * ---------------------------------------------------------------------- */

static void check_context_warning(struct repl_state *st)
{
	if (!st->conv || st->cfg->context_window <= 0)
		return;
	size_t tokens = conversation_estimate_tokens(st->conv);
	double pct = (double)tokens / st->cfg->context_window * 100.0;
	if (pct >= CONTEXT_WARN_PCT)
		term_printf_color(TERM_FG_BRIGHT_YELLOW,
				  "  ⚠ Context: %.0f%% used (%zu/%d tokens). "
				  "/clear or /reset to free space.\n",
				  pct, tokens, st->cfg->context_window);
}

/* -------------------------------------------------------------------------
 * Print agent response (coloured)
 * ---------------------------------------------------------------------- */

static void print_agent_response(struct repl_state *st,
				 struct agent_result *result)
{
	(void)st;
	if (result->finish_reason == FINISHED_INTERRUPTED) {
		term_printf_color(TERM_FG_BRIGHT_YELLOW,
				  "  Interrupted.\n\n");
		return;
	}
	if (result->finish_reason == FINISHED_ERROR) {
		term_printf_color(TERM_FG_BRIGHT_RED, "  Boris stopped: %s\n\n",
				  result->final_response
					  ? result->final_response
					  : "an error occurred");
	} else if (result->finish_reason == FINISHED_MAX_ITERATIONS) {
		if (result->final_response) {
			printf("  Boris:\n");
			markdown_render(result->final_response, stdout);
		}
		term_printf_color(TERM_FG_BRIGHT_YELLOW,
				  "  (Reached max %d iterations)\n\n",
				  st->cfg->max_iterations);
	} else if (result->final_response) {
		printf("  Boris:\n");
		markdown_render(result->final_response, stdout);
		if (result->turns_used > 1) {
			if (result->total_prompt_tokens > 0 ||
			    result->total_completion_tokens > 0) {
				term_printf_color(TERM_FG_BRIGHT_BLACK,
						  "  (%d iteration%s, %d+%d tokens)\n",
						  result->turns_used,
						  result->turns_used > 1 ? "s" : "",
						  result->total_prompt_tokens,
						  result->total_completion_tokens);
			} else {
				term_printf_color(TERM_FG_BRIGHT_BLACK,
						  "  (%d iteration%s)\n",
						  result->turns_used,
						  result->turns_used > 1 ? "s" : "");
			}
		}
		printf("\n");
	} else {
		printf("  Boris stopped unexpectedly.\n\n");
	}
}

/* -------------------------------------------------------------------------
 * Session context header (on restore)
 * ---------------------------------------------------------------------- */

static void print_session_header(struct conversation_history *conv,
				 const char *saved_at)
{
	term_print_color(TERM_FG_BRIGHT_BLACK,
			 "  ┌ Session restored");
	printf(" (%zu messages", conv->count);
	if (saved_at) {
		printf(", saved %s", saved_at);
	}
	printf(")\n");

	for (size_t mi = conv->count; mi > 0; mi--) {
		struct conversation_message *m = &conv->messages[mi - 1];
		if (m->role == MESSAGE_ROLE_USER && m->content) {
			term_print_color(TERM_FG_BRIGHT_BLACK, "  │ ");
			printf("Last: \"%.60s%s\"\n",
			       m->content,
			       strlen(m->content) > 60 ? "..." : "");
			break;
		}
	}
	term_print_color(TERM_FG_BRIGHT_BLACK,
			 "  └─────────────────────────────────────────\n");
	printf("\n");
}

/* -------------------------------------------------------------------------
 * History path helper
 * ---------------------------------------------------------------------- */

static char *history_path(void)
{
	const char *home = getenv("HOME");
	if (!home)
		return NULL;
	char *p = malloc(strlen(home) + 32);
	if (!p)
		return NULL;
	snprintf(p, strlen(home) + 32, "%s/.boris/history.txt", home);
	return p;
}

/* -------------------------------------------------------------------------
 * repl_run - the main interactive loop
 * ---------------------------------------------------------------------- */

void repl_run(struct conversation_history *conv,
	      const struct agent_configuration *config)
{
	struct repl_state state = {
		.conv = conv,
		.cfg = config,
		.should_exit = 0,
	};

	/* Auto-load previous session */
	if (conv && conv->count <= 1) {
		char *session = conversation_session_path();
		if (session) {
			struct conversation_history *loaded =
				conversation_load(session);
			if (loaded && loaded->count > 0) {
				if (conversation_clone(conv, loaded) == BORIS_OK) {
					if (config->system_prompt &&
					    config->system_prompt[0])
						conversation_replace_system_prompt(
							conv,
							config->system_prompt);
					char *saved_at =
						conversation_load_saved_at(session);
					print_session_header(conv, saved_at);
					free(saved_at);
				}
			}
			conversation_destroy(loaded);
			free(session);
		}
	}

	/* Install SIGINT handler */
#ifndef _WIN32
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigint_handler;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
#else
	signal(SIGINT, sigint_handler);
#endif

	/* Load history */
	char *hist_file = history_path();
	if (hist_file) {
		editor_history_load(hist_file);
		free(hist_file);
	}
	editor_set_completion(slash_completion, NULL);

	/* Hints */
	term_print_dim("  Type your message and press Enter to send, /help for commands.\n");
	term_print_dim("  Press Ctrl+D to exit.\n\n");

	/* ----- Main loop ----- */
	while (!state.should_exit && !g_sigint) {
		check_context_warning(&state);

		char *user_input = NULL;
		size_t user_len = 0;
		size_t user_cap = 0;
		int first_line = 1;

		while (1) {
			struct editor_state ed;
			editor_init(&ed);
			const char *prompt = first_line ? "  You: " : "  ... ";
			editor_begin(&ed, prompt);

			enum editor_action action;
			while ((action = editor_process_char(&ed)) == EDITOR_MORE)
				;
			editor_end(&ed);

			if (action == EDITOR_EOF) {
				editor_free(&ed);
				if (user_len > 0)
					break;
				free(user_input);
				printf("\n");
				goto repl_done;
			}

			if (action == EDITOR_INTERRUPT) {
				editor_free(&ed);
				free(user_input);
				user_input = NULL;
				goto next_turn;
			}

			char *text = editor_get_text(&ed);

			/* Slash command on first line */
			if (first_line && text[0] == '/') {
				free(user_input);
				handle_command(text, &state);
				free(text);
				editor_free(&ed);
				goto next_turn;
			}

			/* Empty line - skip or submit */
			if (text[0] == '\0') {
				free(text);
				editor_free(&ed);
				if (user_len > 0)
					break;
				first_line = 0;
				continue;
			}

			/* Backslash continuation? */
			size_t tlen = strlen(text);
			int continuation = (tlen > 0 && text[tlen - 1] == '\\');
			if (continuation)
				text[--tlen] = '\0';

			/* Accumulate into dynamic buffer */
			size_t need = user_len + (user_len > 0 ? 1 : 0) + tlen + 1;
			if (need > user_cap) {
				user_cap = need < 8192 ? 8192 : need * 2;
				char *tmp = realloc(user_input, user_cap);
				if (!tmp) {
					free(user_input);
					free(text);
					editor_free(&ed);
					fprintf(stderr, "  Out of memory reading input.\n");
					goto repl_done;
				}
				user_input = tmp;
			}
			if (user_len > 0)
				user_input[user_len++] = '\n';
			memcpy(user_input + user_len, text, tlen);
			user_len += tlen;
			user_input[user_len] = '\0';
			free(text);
			editor_free(&ed);
			first_line = 0;

			if (!continuation)
				break;
		}

		/* Send to agent */
		printf("\n");
		spinner_start();
		conversation_add_user(conv, user_input);
		editor_history_add(user_input);
		free(user_input);
		user_input = NULL;

		struct agent_result result = agent_run_conv(conv, config);

		spinner_stop();

		if (g_sigint) {
			agent_result_free(&result);
			break;
		}

		print_agent_response(&state, &result);
		agent_result_free(&result);

		term_print_separator();
	next_turn:;
	}

repl_done:
	/* Save history */
	{
		char *hf = history_path();
		if (hf) {
			editor_history_save(hf);
			free(hf);
		}
	}

	/* Ctrl+C path */
	if (g_sigint) {
		char *session = conversation_session_path();
		if (session) {
			if (conversation_save(conv, session) == BORIS_OK)
				printf("\n  Interrupted. Session saved to %s\n\n", session);
			else
				printf("\n  Interrupted. (Session save failed.)\n\n");
			free(session);
		} else {
			printf("\n  Interrupted.\n\n");
		}
	}
}
