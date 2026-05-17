/*
 * editor.c - Minimal line editor with history and tab completion.
 *
 * Uses POSIX termios for raw mode.  When stdin is not a TTY the public
 * API falls back to fgets so piped input still works.
 */
#define _POSIX_C_SOURCE 200809L

#include "editor.h"
#include "terminal.h"

#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#define BUF_INIT_CAP 256
#define HISTORY_MAX  500

/* -------------------------------------------------------------------------
 * Global editor state
 * ---------------------------------------------------------------------- */

static struct termios g_orig_termios;
static int g_raw_active = 0;
static int g_atexit_done = 0;

static char *g_history[HISTORY_MAX];
static int g_history_count = 0;

/* History navigation */
static int g_hist_nav = -1; /* -1 = not navigating */
static char *g_hist_saved = NULL;

/* Completion */
static editor_completion_fn g_comp_fn = NULL;
static void *g_comp_ctx = NULL;

/* Render tracking */
static int g_prev_cursor_line = 0;
static int g_prev_lines = 1;

/* -------------------------------------------------------------------------
 * Raw mode
 * ---------------------------------------------------------------------- */

static void disable_raw_mode(void)
{
	if (g_raw_active) {
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
		g_raw_active = 0;
	}
}

static void atexit_restore(void)
{
	disable_raw_mode();
}

static void enable_raw_mode(void)
{
	if (g_raw_active)
		return;
	struct termios raw;
	tcgetattr(STDIN_FILENO, &g_orig_termios);
	raw = g_orig_termios;
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
	g_raw_active = 1;
	if (!g_atexit_done) {
		atexit(atexit_restore);
		g_atexit_done = 1;
	}
}

/* -------------------------------------------------------------------------
 * Low-level I/O
 * ---------------------------------------------------------------------- */

static int read_byte(void)
{
	unsigned char c;
	ssize_t n = read(STDIN_FILENO, &c, 1);
	if (n <= 0)
		return -1;
	return c;
}

static int terminal_width(void)
{
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
		return (int)ws.ws_col;
	return 80;
}

/* -------------------------------------------------------------------------
 * UTF-8 helpers
 * ---------------------------------------------------------------------- */

static int utf8_byte_len(unsigned char c)
{
	if (c < 0x80)
		return 1;
	if ((c & 0xE0) == 0xC0)
		return 2;
	if ((c & 0xF0) == 0xE0)
		return 3;
	if ((c & 0xF8) == 0xF0)
		return 4;
	return 1;
}

static int utf8_display_width(const char *p, size_t avail)
{
	if (avail == 0)
		return 1;
	unsigned char c = (unsigned char)*p;
	if (c < 0x80)
		return 1;
	int bl = utf8_byte_len(c);
	if ((size_t)bl > avail)
		return 1;
	unsigned int cp = 0;
	if (bl == 2)
		cp = ((c & 0x1F) << 6) | ((unsigned char)p[1] & 0x3F);
	else if (bl == 3)
		cp = ((c & 0x0F) << 12) | (((unsigned char)p[1] & 0x3F) << 6) |
		     ((unsigned char)p[2] & 0x3F);
	else if (bl == 4)
		cp = ((c & 0x07) << 18) | (((unsigned char)p[1] & 0x3F) << 12) |
		     (((unsigned char)p[2] & 0x3F) << 6) | ((unsigned char)p[3] & 0x3F);
	if ((cp >= 0x1100 && cp <= 0x115F) ||
	    (cp >= 0x2E80 && cp <= 0x303F) ||
	    (cp >= 0x3130 && cp <= 0x318F) ||
	    (cp >= 0x4E00 && cp <= 0x9FFF) ||
	    (cp >= 0xAC00 && cp <= 0xD7AF) ||
	    (cp >= 0xF900 && cp <= 0xFAFF) ||
	    (cp >= 0xFE30 && cp <= 0xFE6F) ||
	    (cp >= 0xFF01 && cp <= 0xFF60) ||
	    (cp >= 0x1F300 && cp <= 0x1F9FF))
		return 2;
	return 1;
}

static size_t buf_display_width(const char *buf, size_t len)
{
	size_t w = 0, i = 0;
	while (i < len) {
		int bl = utf8_byte_len((unsigned char)buf[i]);
		if (i + (size_t)bl > len)
			bl = (int)(len - i);
		w += (size_t)utf8_display_width(buf + i, (size_t)bl);
		i += (size_t)bl;
	}
	return w;
}

/* -------------------------------------------------------------------------
 * Buffer manipulation
 * ---------------------------------------------------------------------- */

static void buf_ensure(struct editor_state *ed, size_t need)
{
	if (need < ed->cap)
		return;
	size_t nc = ed->cap;
	while (nc <= need)
		nc *= 2;
	char *tmp = realloc(ed->buf, nc);
	if (!tmp)
		return;
	ed->buf = tmp;
	ed->cap = nc;
}

static void buf_insert(struct editor_state *ed, const char *bytes, size_t blen)
{
	buf_ensure(ed, ed->len + blen + 1);
	memmove(ed->buf + ed->cursor + blen,
		ed->buf + ed->cursor,
		ed->len - ed->cursor);
	memcpy(ed->buf + ed->cursor, bytes, blen);
	ed->len += blen;
	ed->cursor += blen;
	ed->buf[ed->len] = '\0';
}

static void buf_delete(struct editor_state *ed, size_t pos, size_t blen)
{
	if (pos >= ed->len || blen == 0)
		return;
	if (pos + blen > ed->len)
		blen = ed->len - pos;
	memmove(ed->buf + pos, ed->buf + pos + blen, ed->len - pos - blen);
	ed->len -= blen;
	ed->buf[ed->len] = '\0';
}

static size_t prev_char(struct editor_state *ed, size_t pos)
{
	if (pos == 0)
		return 0;
	size_t p = pos - 1;
	while (p > 0 && ((unsigned char)ed->buf[p] & 0xC0) == 0x80)
		p--;
	return p;
}

static size_t next_char(struct editor_state *ed, size_t pos)
{
	if (pos >= ed->len)
		return ed->len;
	return pos + (size_t)utf8_byte_len((unsigned char)ed->buf[pos]);
}

static void set_buf_text(struct editor_state *ed, const char *text, size_t tlen)
{
	buf_ensure(ed, tlen + 1);
	memcpy(ed->buf, text, tlen);
	ed->buf[tlen] = '\0';
	ed->len = tlen;
	ed->cursor = tlen;
}

/* -------------------------------------------------------------------------
 * Rendering
 * ---------------------------------------------------------------------- */

static void render_line(struct editor_state *ed)
{
	if (!term_is_tty())
		return;

	int tw = terminal_width();
	if (tw <= 0)
		tw = 80;
	size_t pc = strlen(ed->prompt);
	size_t cc = buf_display_width(ed->buf, ed->len);
	size_t tc = pc + cc;
	int nl = tc > 0 ? (int)((tc + (size_t)tw - 1) / (size_t)tw) : 1;

	/* Move up to the prompt line */
	for (int i = 0; i < g_prev_cursor_line; i++)
		printf("\033[A");

	/* Clear all previously rendered lines */
	for (int i = 0; i < g_prev_lines; i++) {
		printf("\r\033[K");
		if (i < g_prev_lines - 1)
			printf("\033[B");
	}
	for (int i = 1; i < g_prev_lines; i++)
		printf("\033[A");

	/* Print prompt + content */
	printf("\r%s", ed->prompt);
	if (ed->len > 0)
		fwrite(ed->buf, 1, ed->len, stdout);

	g_prev_lines = nl;

	/* Position cursor */
	size_t cd = buf_display_width(ed->buf, ed->cursor);
	size_t cp = pc + cd;
	/* When cp lands exactly on a terminal-width boundary the terminal is in
	 * pending-wrap state: the cursor sits at the last column of the current
	 * row, not at column 0 of the next row.  Treat it as still on the
	 * previous row so the move-up count and column positioning are correct. */
	int cur_line, col;
	if (cp > 0 && cp % (size_t)tw == 0) {
		cur_line = (int)(cp / (size_t)tw) - 1;
		col      = tw - 1;  /* last column; col=tw would wrap on some terminals */
	} else {
		cur_line = (int)(cp / (size_t)tw);
		col      = (int)(cp % (size_t)tw);
	}
	int end_line = tc > 0 ? (int)((tc - 1) / (size_t)tw) : 0;
	for (int i = 0; i < end_line - cur_line; i++)
		printf("\033[A");
	printf("\r");
	if (col > 0)
		printf("\033[%dC", col);

	g_prev_cursor_line = cur_line;
	fflush(stdout);
}

/* -------------------------------------------------------------------------
 * History navigation helpers
 * ---------------------------------------------------------------------- */

static void reset_nav(void)
{
	if (g_hist_nav >= 0) {
		free(g_hist_saved);
		g_hist_saved = NULL;
		g_hist_nav = -1;
	}
}

static void history_apply(struct editor_state *ed, int idx)
{
	const char *entry = (idx >= 0 && idx < g_history_count)
				    ? g_history[idx]
				    : "";
	set_buf_text(ed, entry, strlen(entry));
	render_line(ed);
}

/* -------------------------------------------------------------------------
 * Tab completion
 * ---------------------------------------------------------------------- */

static void handle_tab(struct editor_state *ed)
{
	if (!g_comp_fn || ed->len == 0 || ed->buf[0] != '/')
		return;

	char completion[256];
	int count = g_comp_fn(ed->buf, completion, sizeof(completion),
			      g_comp_ctx);

	if (count == 1) {
		size_t clen = strlen(completion);
		set_buf_text(ed, completion, clen);
		render_line(ed);
	} else if (count > 1) {
		/* Multiple matches already printed by callback; re-render */
		render_line(ed);
	}
}

/* -------------------------------------------------------------------------
 * Character processing (raw mode)
 * ---------------------------------------------------------------------- */

static enum editor_action process_raw(struct editor_state *ed)
{
	int c = read_byte();
	if (c < 0)
		return ed->len == 0 ? EDITOR_EOF : EDITOR_DONE;

	/* Enter */
	if (c == '\n' || c == '\r') {
		if (term_is_tty())
			printf("\n");
		g_prev_cursor_line = 0;
		g_prev_lines = 1;
		return EDITOR_DONE;
	}

	/* Ctrl+C */
	if (c == 3) {
		if (term_is_tty())
			printf("^C\n");
		ed->len = 0;
		ed->cursor = 0;
		ed->buf[0] = '\0';
		reset_nav();
		g_prev_cursor_line = 0;
		g_prev_lines = 1;
		return EDITOR_INTERRUPT;
	}

	/* Ctrl+D */
	if (c == 4) {
		if (ed->len == 0)
			return EDITOR_EOF;
		if (ed->cursor < ed->len) {
			size_t nx = next_char(ed, ed->cursor);
			buf_delete(ed, ed->cursor, nx - ed->cursor);
			render_line(ed);
		}
		return EDITOR_MORE;
	}

	/* Backspace */
	if (c == 0x7F) {
		if (ed->cursor > 0) {
			size_t pv = prev_char(ed, ed->cursor);
			buf_delete(ed, pv, ed->cursor - pv);
			ed->cursor = pv;
			reset_nav();
			render_line(ed);
		}
		return EDITOR_MORE;
	}

	/* Escape sequences */
	if (c == 0x1B) {
		int s1 = read_byte();
		if (s1 < 0)
			return EDITOR_MORE;
		if (s1 != '[')
			return EDITOR_MORE;
		int s2 = read_byte();
		if (s2 < 0)
			return EDITOR_MORE;

		switch (s2) {
		case 'A': /* Up — history prev */
			if (g_history_count == 0)
				return EDITOR_MORE;
			if (g_hist_nav < 0) {
				g_hist_saved = ed->buf ? strdup(ed->buf) : strdup("");
				g_hist_nav = g_history_count - 1;
			} else if (g_hist_nav > 0) {
				g_hist_nav--;
			}
			history_apply(ed, g_hist_nav);
			return EDITOR_MORE;
		case 'B': /* Down — history next */
			if (g_hist_nav < 0)
				return EDITOR_MORE;
			g_hist_nav++;
			if (g_hist_nav >= g_history_count) {
				size_t sl = g_hist_saved ? strlen(g_hist_saved) : 0;
				set_buf_text(ed, g_hist_saved ? g_hist_saved : "", sl);
				reset_nav();
			} else {
				history_apply(ed, g_hist_nav);
			}
			render_line(ed);
			return EDITOR_MORE;
		case 'C': /* Right */
			if (ed->cursor < ed->len) {
				ed->cursor = next_char(ed, ed->cursor);
				render_line(ed);
			}
			return EDITOR_MORE;
		case 'D': /* Left */
			if (ed->cursor > 0) {
				ed->cursor = prev_char(ed, ed->cursor);
				render_line(ed);
			}
			return EDITOR_MORE;
		case 'H': /* Home */
			ed->cursor = 0;
			render_line(ed);
			return EDITOR_MORE;
		case 'F': /* End */
			ed->cursor = ed->len;
			render_line(ed);
			return EDITOR_MORE;
		case '3': /* Delete (expect ~) */
		{
			int s3 = read_byte();
			if (s3 == '~' && ed->cursor < ed->len) {
				size_t nx = next_char(ed, ed->cursor);
				buf_delete(ed, ed->cursor, nx - ed->cursor);
				render_line(ed);
			}
			return EDITOR_MORE;
		}
		default:
			return EDITOR_MORE;
		}
	}

	/* Ctrl+A — Home */
	if (c == 1) {
		ed->cursor = 0;
		render_line(ed);
		return EDITOR_MORE;
	}
	/* Ctrl+E — End */
	if (c == 5) {
		ed->cursor = ed->len;
		render_line(ed);
		return EDITOR_MORE;
	}
	/* Ctrl+U — Kill line */
	if (c == 0x15) {
		buf_delete(ed, 0, ed->cursor);
		ed->cursor = 0;
		render_line(ed);
		return EDITOR_MORE;
	}
	/* Ctrl+K — Kill to end */
	if (c == 0x0B) {
		buf_delete(ed, ed->cursor, ed->len - ed->cursor);
		render_line(ed);
		return EDITOR_MORE;
	}
	/* Ctrl+W — Delete word backwards */
	if (c == 0x17) {
		if (ed->cursor > 0) {
			size_t p = ed->cursor;
			while (p > 0 && ed->buf[p - 1] == ' ')
				p--;
			while (p > 0 && ed->buf[p - 1] != ' ')
				p--;
			buf_delete(ed, p, ed->cursor - p);
			ed->cursor = p;
			render_line(ed);
		}
		return EDITOR_MORE;
	}
	/* Ctrl+L — Clear screen */
	if (c == 0x0C) {
		if (term_is_tty()) {
			printf("\033[2J\033[H");
			g_prev_lines = 1;
			g_prev_cursor_line = 0;
			render_line(ed);
		}
		return EDITOR_MORE;
	}

	/* Tab — autocomplete */
	if (c == '\t') {
		handle_tab(ed);
		return EDITOR_MORE;
	}

	/* Printable character (including UTF-8 lead bytes) */
	if (c >= 0x20 || (unsigned char)c >= 0xC0) {
		char bytes[4];
		int bl = 1;
		bytes[0] = (char)c;
		if ((unsigned char)c >= 0xC0) {
			int expect = utf8_byte_len((unsigned char)c);
			for (int i = 1; i < expect && i < 4; i++) {
				int nb = read_byte();
				if (nb < 0 || (nb & 0xC0) != 0x80)
					break;
				bytes[bl++] = (char)nb;
			}
		}
		reset_nav();
		buf_insert(ed, bytes, (size_t)bl);
		render_line(ed);
		return EDITOR_MORE;
	}

	return EDITOR_MORE;
}

/* -------------------------------------------------------------------------
 * Fallback: fgets for pipe / non-TTY input
 * ---------------------------------------------------------------------- */

static enum editor_action process_fgets(struct editor_state *ed)
{
	printf("%s", ed->prompt);
	fflush(stdout);

	char line[4096];
	if (!fgets(line, sizeof(line), stdin))
		return ed->len == 0 ? EDITOR_EOF : EDITOR_DONE;

	size_t len = strlen(line);
	while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
		line[--len] = '\0';

	if (len == 0)
		return ed->len > 0 ? EDITOR_DONE : EDITOR_MORE;

	buf_ensure(ed, ed->len + len + 2);
	if (ed->len > 0)
		ed->buf[ed->len++] = '\n';
	memcpy(ed->buf + ed->len, line, len);
	ed->len += len;
	ed->buf[ed->len] = '\0';
	ed->cursor = ed->len;
	return EDITOR_DONE;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void editor_init(struct editor_state *ed)
{
	ed->buf = calloc(1, BUF_INIT_CAP);
	ed->len = 0;
	ed->cap = BUF_INIT_CAP;
	ed->cursor = 0;
	ed->prompt = "";
}

void editor_free(struct editor_state *ed)
{
	free(ed->buf);
	ed->buf = NULL;
	ed->len = 0;
	ed->cap = 0;
	ed->cursor = 0;
}

void editor_begin(struct editor_state *ed, const char *prompt)
{
	ed->prompt = prompt;
	ed->len = 0;
	ed->cursor = 0;
	ed->buf[0] = '\0';
	reset_nav();
	g_prev_lines = 1;
	g_prev_cursor_line = 0;

	if (isatty(STDIN_FILENO) && term_is_tty()) {
		enable_raw_mode();
		printf("%s", prompt);
		fflush(stdout);
	}
}

enum editor_action editor_process_char(struct editor_state *ed)
{
	if (g_raw_active)
		return process_raw(ed);
	return process_fgets(ed);
}

void editor_end(struct editor_state *ed)
{
	(void)ed;
	disable_raw_mode();
}

char *editor_get_text(struct editor_state *ed)
{
	return ed->buf ? strdup(ed->buf) : strdup("");
}

const char *editor_get_buffer(struct editor_state *ed)
{
	return ed->buf ? ed->buf : "";
}

void editor_set_completion(editor_completion_fn fn, void *ctx)
{
	g_comp_fn = fn;
	g_comp_ctx = ctx;
}

/* -------------------------------------------------------------------------
 * History management
 * ---------------------------------------------------------------------- */

void editor_history_add(const char *line)
{
	if (!line || !line[0])
		return;
	if (g_history_count > 0 &&
	    strcmp(g_history[g_history_count - 1], line) == 0)
		return;
	if (g_history_count >= HISTORY_MAX) {
		free(g_history[0]);
		memmove(g_history, g_history + 1,
			(HISTORY_MAX - 1) * sizeof(char *));
		g_history_count--;
	}
	g_history[g_history_count++] = strdup(line);
}

void editor_history_load(const char *path)
{
	if (!path)
		return;
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	char line[4096];
	while (fgets(line, sizeof(line), f)) {
		size_t len = strlen(line);
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';
		char *unesc = malloc(len * 2 + 1);
		if (!unesc)
			break;
		size_t j = 0;
		for (size_t i = 0; i < len; i++) {
			if (line[i] == '\\' && i + 1 < len) {
				if (line[i + 1] == 'n') {
					unesc[j++] = '\n';
					i++;
				} else if (line[i + 1] == '\\') {
					unesc[j++] = '\\';
					i++;
				} else
					unesc[j++] = line[i];
			} else {
				unesc[j++] = line[i];
			}
		}
		unesc[j] = '\0';
		if (g_history_count < HISTORY_MAX)
			g_history[g_history_count++] = unesc;
		else
			free(unesc);
	}
	fclose(f);
}

void editor_history_save(const char *path)
{
	if (!path || g_history_count == 0)
		return;
	FILE *f = fopen(path, "w");
	if (!f)
		return;
	for (int i = 0; i < g_history_count; i++) {
		const char *s = g_history[i];
		for (size_t j = 0; s[j]; j++) {
			if (s[j] == '\n')
				fputs("\\n", f);
			else if (s[j] == '\\')
				fputs("\\\\", f);
			else
				fputc(s[j], f);
		}
		fputc('\n', f);
	}
	fclose(f);
}
