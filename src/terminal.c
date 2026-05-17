/*
 * terminal.c - ANSI escape sequence abstraction layer.
 *
 * All functions cache their TTY / colour checks.  When stdout is piped
 * or NO_COLOR is set, the helpers emit plain text with no escape codes.
 */
#define _POSIX_C_SOURCE 200809L

#include "terminal.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Cached detection
 * ---------------------------------------------------------------------- */

static int g_tty_cached = -1; /* -1 = not yet checked */
static int g_color_cached = -1;

bool term_is_tty(void)
{
	if (g_tty_cached < 0)
		g_tty_cached = isatty(STDOUT_FILENO) ? 1 : 0;
	return g_tty_cached == 1;
}

bool term_supports_color(void)
{
	if (g_color_cached >= 0)
		return g_color_cached == 1;

	g_color_cached = 0;

	if (!term_is_tty())
		return false;

	const char *no_color = getenv("NO_COLOR");
	if (no_color && no_color[0])
		return false;

	const char *ct = getenv("COLORTERM");
	if (ct && (strstr(ct, "truecolor") || strstr(ct, "24bit"))) {
		g_color_cached = 1;
		return true;
	}

	const char *term = getenv("TERM");
	if (!term)
		return false;
	if (strstr(term, "256color") || strstr(term, "xterm") ||
	    strstr(term, "screen") || strstr(term, "tmux") ||
	    strstr(term, "ansi") || strstr(term, "color")) {
		g_color_cached = 1;
		return true;
	}

	return false;
}

/* -------------------------------------------------------------------------
 * Cursor movement
 * ---------------------------------------------------------------------- */

void term_cursor_up(int n)
{
	if (n > 0 && term_is_tty())
		printf("\033[%dA", n);
}

void term_cursor_down(int n)
{
	if (n > 0 && term_is_tty())
		printf("\033[%dB", n);
}

void term_cursor_left(int n)
{
	if (n > 0 && term_is_tty())
		printf("\033[%dD", n);
}

void term_cursor_right(int n)
{
	if (n > 0 && term_is_tty())
		printf("\033[%dC", n);
}

void term_cursor_move(int row, int col)
{
	if (term_is_tty())
		printf("\033[%d;%dH", row, col);
}

void term_cursor_save(void)
{
	if (term_is_tty())
		printf("\033[s");
}

void term_cursor_restore(void)
{
	if (term_is_tty())
		printf("\033[u");
}

void term_cursor_hide(void)
{
	if (term_is_tty())
		printf("\033[?25l");
}

void term_cursor_show(void)
{
	if (term_is_tty())
		printf("\033[?25h");
}

/* -------------------------------------------------------------------------
 * Erasing
 * ---------------------------------------------------------------------- */

void term_erase_line(void)
{
	if (term_is_tty())
		printf("\033[K");
}

void term_erase_screen(void)
{
	if (term_is_tty())
		printf("\033[2J\033[H");
}

void term_erase_lines_above(int n)
{
	if (!term_is_tty() || n <= 0)
		return;
	for (int i = 0; i < n; i++)
		printf("\033[A\033[K");
}

/* -------------------------------------------------------------------------
 * High-level print helpers
 * ---------------------------------------------------------------------- */

void term_print_color(const char *fg, const char *text)
{
	if (term_supports_color())
		printf("%s%s%s", fg, text, TERM_RESET);
	else
		printf("%s", text);
}

void term_print_bold(const char *text)
{
	if (term_supports_color())
		printf("%s%s%s", TERM_BOLD, text, TERM_RESET);
	else
		printf("%s", text);
}

void term_print_dim(const char *text)
{
	if (term_supports_color())
		printf("%s%s%s", TERM_DIM, text, TERM_RESET);
	else
		printf("%s", text);
}

void term_printf_color(const char *fg, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (term_supports_color()) {
		printf("%s", fg);
		vprintf(fmt, ap);
		printf("%s", TERM_RESET);
	} else {
		vprintf(fmt, ap);
	}
	va_end(ap);
}

void term_print_separator(void)
{
	if (term_supports_color())
		printf("%s  ──────────────────────────────────────────────%s\n",
		       TERM_DIM, TERM_RESET);
	else
		printf("\n");
}
