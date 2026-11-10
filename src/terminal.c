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
#include "platform.h"

/* -------------------------------------------------------------------------
 * Cached detection
 * ---------------------------------------------------------------------- */

static int g_tty_cached = -1; /* -1 = not yet checked */
static int g_color_cached = -1;

bool term_is_tty(void)
{
	if (g_tty_cached < 0) {
#ifdef _WIN32
		/* A console that cannot be switched to ANSI escape sequences
		 * (before Windows 10) is treated like a pipe: plain text. */
		g_tty_cached = platform_console_vt() ? 1 : 0;
#else
		g_tty_cached = isatty(STDOUT_FILENO) ? 1 : 0;
#endif
	}
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

#ifdef _WIN32
	/* Every console that passed term_is_tty() does colour; TERM is unset. */
	g_color_cached = 1;
	return true;
#else
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
#endif
}

/* -------------------------------------------------------------------------
 * Cursor visibility
 * ---------------------------------------------------------------------- */

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
 * Screen
 * ---------------------------------------------------------------------- */

void term_erase_screen(void)
{
	if (term_is_tty())
		printf("\033[2J\033[H");
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

/* Length of the valid UTF-8 sequence starting at s (1-4), or 0 if invalid. */
static size_t utf8_seq_len(const unsigned char *s)
{
	size_t len = s[0] < 0x80	   ? 1
		     : (s[0] & 0xE0) == 0xC0 ? 2
		     : (s[0] & 0xF0) == 0xE0 ? 3
		     : (s[0] & 0xF8) == 0xF0 ? 4
					     : 0;
	for (size_t i = 1; i < len; i++)
		if ((s[i] & 0xC0) != 0x80)
			return 0;
	return len;
}

void term_sanitize(const char *text, char *buf, size_t size)
{
	if (!buf || size == 0)
		return;
	buf[0] = '\0';
	if (!text)
		return;

	static const char ellipsis[] = "...";
	size_t out = 0;
	const unsigned char *p = (const unsigned char *)text;

	while (*p) {
		size_t len = utf8_seq_len(p);
		size_t emit = len ? len : 1;
		/* Room for this character, and for "..." if more text follows */
		size_t reserve = p[emit] ? sizeof(ellipsis) : 1;
		if (out + emit + reserve > size) {
			/* Every accepted character left room for "..." whenever more
			 * text followed, so this always fits (given size >= 4). */
			if (size >= sizeof(ellipsis)) {
				memcpy(buf + out, ellipsis, sizeof(ellipsis));
				return;
			}
			break;
		}

		if (len == 1) {
			/* C0 controls (incl. ESC) and DEL become spaces; tab stays */
			buf[out++] = (*p < 0x20 && *p != '\t') || *p == 0x7F ? ' ' : (char)*p;
		} else if (len == 0) {
			buf[out++] = '?'; /* invalid byte, e.g. a raw 0x9B (8-bit CSI) */
		} else if (len == 2 && p[0] == 0xC2 && p[1] < 0xA0) {
			buf[out++] = ' '; /* C1 control U+0080..U+009F */
			buf[out++] = ' ';
		} else {
			memcpy(buf + out, p, len);
			out += len;
		}
		p += emit;
	}
	buf[out] = '\0';
}
