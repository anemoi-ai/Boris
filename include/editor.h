/*
 * editor.h - Minimal line editor with history and tab completion.
 *
 * Inspired by antirez's linenoise, but trimmed to ~400 lines so students
 * can read every file.  Uses POSIX termios for raw mode; falls back to
 * fgets when stdin is not a TTY.
 */

#ifndef EDITOR_H
#define EDITOR_H

#include <stdbool.h>
#include <stddef.h>

enum editor_action {
	EDITOR_DONE,	  /* Enter pressed — line is ready           */
	EDITOR_MORE,	  /* Need more input (character processed)   */
	EDITOR_EOF,	  /* Ctrl+D on empty line                    */
	EDITOR_INTERRUPT, /* Ctrl+C — input cancelled                */
};

struct editor_state {
	char *buf;	    /* heap-allocated edit buffer            */
	size_t len;	    /* current content length (bytes)        */
	size_t cap;	    /* allocated capacity                    */
	size_t cursor;	    /* cursor position (byte offset into buf)*/
	const char *prompt; /* prompt string (e.g. "  You: ")       */
};

/*
 * Completion callback.
 *
 * input        — the current editor buffer
 * completion   — filled with the single-match completion (if count == 1)
 * comp_size    — size of the completion buffer
 * ctx          — opaque pointer set via editor_set_completion()
 *
 * Returns 0 (no match), 1 (single match — completion filled), or >1
 * (multiple matches — callback should print them to stdout itself).
 */
typedef int (*editor_completion_fn)(const char *input,
				    char *completion,
				    size_t comp_size,
				    void *ctx);

void editor_init(struct editor_state *ed);
void editor_free(struct editor_state *ed);

/* Enter raw mode and display prompt. */
void editor_begin(struct editor_state *ed, const char *prompt);

/* Read/process one character.  Call in a loop until != EDITOR_MORE. */
enum editor_action editor_process_char(struct editor_state *ed);

/* Restore terminal settings. */
void editor_end(struct editor_state *ed);

/* Return a strdup'd copy of the buffer (caller must free). */
char *editor_get_text(struct editor_state *ed);

/* Read-only pointer into the live buffer (don't free). */
const char *editor_get_buffer(struct editor_state *ed);

/* Set the tab-completion callback (optional). */
void editor_set_completion(editor_completion_fn fn, void *ctx);

/* History API */
void editor_history_add(const char *line);
void editor_history_load(const char *path);
void editor_history_save(const char *path);

#endif /* EDITOR_H */
