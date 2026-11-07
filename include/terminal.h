/*
 * terminal.h - ANSI escape sequence abstraction layer.
 *
 * Every visual feature degrades gracefully when stdout is not a TTY.
 * All term_print_* helpers check term_supports_color() internally,
 * so piped output (e.g. boris chat | less) contains no escape codes.
 *
 * Respects the NO_COLOR convention (https://no-color.org/).
 */

#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdbool.h>
#include <stddef.h>

/* 4-bit SGR foreground colours */
#define TERM_FG_BLACK	"\033[30m"
#define TERM_FG_RED	    "\033[31m"
#define TERM_FG_GREEN	"\033[32m"
#define TERM_FG_YELLOW	"\033[33m"
#define TERM_FG_BLUE	"\033[34m"
#define TERM_FG_MAGENTA "\033[35m"
#define TERM_FG_CYAN	"\033[36m"
#define TERM_FG_WHITE	"\033[37m"
#define TERM_FG_DEFAULT "\033[39m"

#define TERM_FG_BRIGHT_BLACK   "\033[90m"
#define TERM_FG_BRIGHT_RED     "\033[91m"
#define TERM_FG_BRIGHT_GREEN   "\033[92m"
#define TERM_FG_BRIGHT_YELLOW  "\033[93m"
#define TERM_FG_BRIGHT_BLUE    "\033[94m"
#define TERM_FG_BRIGHT_MAGENTA "\033[95m"
#define TERM_FG_BRIGHT_CYAN    "\033[96m"
#define TERM_FG_BRIGHT_WHITE   "\033[97m"

/* Styles */
#define TERM_BOLD      "\033[1m"
#define TERM_DIM       "\033[2m"
#define TERM_ITALIC    "\033[3m"
#define TERM_UNDERLINE "\033[4m"
#define TERM_RESET     "\033[0m"

/* Cached TTY / colour checks */
bool term_is_tty(void);
bool term_supports_color(void);

/* Cursor visibility */
void term_cursor_hide(void);
void term_cursor_show(void);

/* Clear the whole screen and home the cursor */
void term_erase_screen(void);

/* High-level print helpers (these check term_is_tty / term_supports_color) */
void term_print_color(const char *fg, const char *text);
void term_print_bold(const char *text);
void term_print_dim(const char *text);
void term_printf_color(const char *fg, const char *fmt, ...);
void term_print_separator(void);

/*
 * Copy untrusted text (file contents, tool output, model-written arguments)
 * into buf so it is safe to print on one terminal line: control characters
 * other than tab, including ESC, become spaces (no cursor moves, screen
 * clears or OSC sequences), and the result is cut to fit buf on a UTF-8
 * character boundary, ending in "..." when shortened. buf is always
 * NUL-terminated.
 */
void term_sanitize(const char *text, char *buf, size_t size);

#endif /* TERMINAL_H */
