/*
 * markdown.c - Terminal markdown renderer.
 *
 * Parses a subset of markdown and emits ANSI-formatted output when the
 * terminal supports colour, or clean plain text otherwise.  The same code
 * path runs in both modes; only the escape sequences differ.
 *
 * Supported block elements:
 *   headings (H1-H6), fenced code blocks, blockquotes, unordered lists,
 *   ordered lists, horizontal rules, blank lines.
 *
 * Supported inline elements:
 *   **bold**, *italic*, `code spans`, ~~strikethrough~~, [link](url).
 *
 * Not supported: tables, HTML blocks, setext headings, nested block
 * elements.  These rarely appear in LLM chat responses and the complexity
 * cost is high.
 */
#define _POSIX_C_SOURCE 200809L

#include "markdown.h"
#include "terminal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * ANSI codes used only by this module.
 * General-purpose codes (TERM_BOLD, TERM_DIM, TERM_RESET …) come from
 * terminal.h.
 * ---------------------------------------------------------------------- */

#define MD_H1	   "\033[1;97m"	    /* bold bright-white - H1 and H2     */
#define MD_H3	   "\033[1m"	    /* bold               - H3 through H6 */
#define MD_CODE_FG "\033[38;5;180m" /* warm yellow  - inline code and blocks */
#define MD_CODE_BG "\033[48;5;236m" /* dark-grey bg - inline code            */
#define MD_QUOTE   "\033[2;36m"	    /* dim cyan - blockquote text            */
#define MD_FENCE   "\033[2m"	    /* dim - fence lines and rules           */

/* -------------------------------------------------------------------------
 * Inline state - tracks open formatting markers within one line.
 * ---------------------------------------------------------------------- */

struct istate {
	bool bold;
	bool italic;
	bool strike;
	bool code;
};

/*
 * Re-emit ANSI to match st from a clean baseline (after TERM_RESET).
 * Only called when color is true; code colour is managed separately.
 */
static void istate_apply(const struct istate *st, FILE *out)
{
	if (st->bold)
		fputs("\033[1m", out);
	if (st->italic)
		fputs("\033[3m", out);
	if (st->strike)
		fputs("\033[9m", out);
}

/* -------------------------------------------------------------------------
 * render_inline - character scanner for inline markdown.
 *
 * Processes one logical line, emitting formatted output.  ANSI codes are
 * omitted when color is false, but structural transformations (marker
 * stripping, link text extraction) always apply.
 * ---------------------------------------------------------------------- */

static void render_inline(const char *s, size_t len, FILE *out, bool color)
{
	struct istate st = {false, false, false, false};
	size_t i = 0;

	while (i < len) {
		/* Inside a code span: emit raw until the closing backtick. */
		if (st.code) {
			if (s[i] == '`') {
				if (color) {
					fputs(TERM_RESET, out);
					istate_apply(&st, out);
				}
				st.code = false;
				i++;
				continue;
			}
			fputc(s[i++], out);
			continue;
		}

		/* ~~strikethrough~~ */
		if (i + 1 < len && s[i] == '~' && s[i + 1] == '~') {
			if (color) {
				if (st.strike) {
					fputs(TERM_RESET, out);
					st.strike = false;
					istate_apply(&st, out);
				} else {
					fputs("\033[9m", out);
					st.strike = true;
				}
			} else {
				st.strike = !st.strike;
			}
			i += 2;
			continue;
		}

		/* **bold** - must be checked before single-star italic */
		if (i + 1 < len && s[i] == '*' && s[i + 1] == '*') {
			if (color) {
				if (st.bold) {
					fputs("\033[22m", out);
					st.bold = false;
				} else {
					fputs("\033[1m", out);
					st.bold = true;
				}
			} else {
				st.bold = !st.bold;
			}
			i += 2;
			continue;
		}

		/* *italic* */
		if (s[i] == '*') {
			if (color) {
				if (st.italic) {
					fputs("\033[23m", out);
					st.italic = false;
				} else {
					fputs("\033[3m", out);
					st.italic = true;
				}
			} else {
				st.italic = !st.italic;
			}
			i++;
			continue;
		}

		/* `code span` */
		if (s[i] == '`') {
			if (color)
				fputs(MD_CODE_FG MD_CODE_BG, out);
			st.code = true;
			i++;
			continue;
		}

		/* [link text](url) */
		if (s[i] == '[') {
			const char *cb =
				memchr(s + i + 1, ']', len - i - 1);
			if (cb) {
				size_t cb_pos = (size_t)(cb - s);
				if (cb_pos + 1 < len &&
				    s[cb_pos + 1] == '(') {
					const char *ue =
						memchr(s + cb_pos + 2, ')',
						       len - cb_pos - 2);
					if (ue) {
						size_t tlen =
							cb_pos - i - 1;
						size_t ulen =
							(size_t)(ue - s) -
							cb_pos - 2;
						render_inline(s + i + 1,
							      tlen, out,
							      color);
						if (color) {
							fputs(TERM_RESET,
							      out);
							istate_apply(&st,
								     out);
							fputs(" " TERM_DIM
							      "(",
							      out);
							fwrite(s + cb_pos + 2,
							       1, ulen, out);
							fputs(")" TERM_RESET,
							      out);
							istate_apply(&st,
								     out);
						} else {
							fputs(" (", out);
							fwrite(s + cb_pos + 2,
							       1, ulen, out);
							fputc(')', out);
						}
						i = (size_t)(ue - s) + 1;
						continue;
					}
				}
			}
			/* No complete link pattern - '[' is a literal. */
		}

		fputc(s[i++], out);
	}

	/* Reset any open inline state at end of line. */
	if (color && (st.bold || st.italic || st.code || st.strike))
		fputs(TERM_RESET, out);
}

/* -------------------------------------------------------------------------
 * Block-level helpers
 * ---------------------------------------------------------------------- */

/*
 * Count and strip leading '#' from a heading line.
 * Returns the heading level (1-6), or 0 if the line is not a heading.
 * On success, *content points to the first character of the heading text.
 */
static int parse_heading(const char *line, const char **content)
{
	int level = 0;
	while (line[level] == '#' && level < 6)
		level++;
	if (level == 0 || line[level] != ' ')
		return 0;
	*content = line + level + 1;
	return level;
}

/*
 * Return true if line is a horizontal rule: three or more identical
 * characters from the set { - = _ } and nothing else.
 */
static bool is_hrule(const char *line, size_t len)
{
	if (len < 3)
		return false;
	char c = line[0];
	if (c != '-' && c != '=' && c != '_')
		return false;
	for (size_t k = 0; k < len; k++)
		if (line[k] != c)
			return false;
	return true;
}

/* -------------------------------------------------------------------------
 * markdown_render - main entry point
 * ---------------------------------------------------------------------- */

void markdown_render(const char *text, FILE *out)
{
	if (!text || !out)
		return;

	bool color =
		term_supports_color() && (isatty(fileno(out)) == 1);

	/* Strip trailing newlines so the caller controls post-response spacing. */
	const char *end = text + strlen(text);
	while (end > text && (end[-1] == '\n' || end[-1] == '\r'))
		end--;
	size_t trimmed = (size_t)(end - text);

	if (trimmed == 0)
		return;

	char *buf = malloc(trimmed + 1);
	if (!buf)
		return;
	memcpy(buf, text, trimmed);
	buf[trimmed] = '\0';

	bool in_fence = false;

	char *line = buf;
	while (line) {
		char *nl = strchr(line, '\n');
		if (nl)
			*nl = '\0';

		/* Strip trailing CR. */
		size_t llen = strlen(line);
		if (llen > 0 && line[llen - 1] == '\r')
			line[--llen] = '\0';

		/* ---- Fenced code block ---- */
		bool fence_line =
			llen >= 3 &&
			((line[0] == '`' && line[1] == '`' &&
			  line[2] == '`') ||
			 (line[0] == '~' && line[1] == '~' &&
			  line[2] == '~'));

		if (fence_line) {
			in_fence = !in_fence;
			if (color)
				fprintf(out, "%s  %s%s\n", MD_FENCE,
					line, TERM_RESET);
			else
				fprintf(out, "  %s\n", line);
			line = nl ? nl + 1 : NULL;
			continue;
		}

		if (in_fence) {
			if (color)
				fprintf(out, "%s  %s%s\n", MD_CODE_FG,
					line, TERM_RESET);
			else
				fprintf(out, "  %s\n", line);
			line = nl ? nl + 1 : NULL;
			continue;
		}

		/* ---- Blank line ---- */
		if (llen == 0) {
			fputc('\n', out);
			line = nl ? nl + 1 : NULL;
			continue;
		}

		/* ---- Heading ---- */
		const char *htxt = NULL;
		int hlevel = parse_heading(line, &htxt);
		if (hlevel > 0) {
			const char *hfmt =
				(hlevel <= 2) ? MD_H1 : MD_H3;
			if (color) {
				fprintf(out, "\n%s  ", hfmt);
				render_inline(htxt, strlen(htxt), out,
					      true);
				fprintf(out, "%s\n", TERM_RESET);
			} else {
				fputs("\n  ", out);
				render_inline(htxt, strlen(htxt), out,
					      false);
				fputc('\n', out);
			}
			line = nl ? nl + 1 : NULL;
			continue;
		}

		/* ---- Horizontal rule ---- */
		if (is_hrule(line, llen)) {
			if (color)
				fprintf(out,
					"\n%s  ──────────────────────────────────────────%s\n\n",
					MD_FENCE, TERM_RESET);
			else
				fputs("\n  ──────────────────────────────────────────\n\n",
				      out);
			line = nl ? nl + 1 : NULL;
			continue;
		}

		/* ---- Blockquote ---- */
		if (line[0] == '>' &&
		    (llen == 1 || line[1] == ' ')) {
			const char *content =
				(llen > 2) ? line + 2 : "";
			if (color) {
				fprintf(out, "%s  │ ", MD_QUOTE);
				render_inline(content, strlen(content),
					      out, true);
				fprintf(out, "%s\n", TERM_RESET);
			} else {
				fputs("  │ ", out);
				render_inline(content, strlen(content),
					      out, false);
				fputc('\n', out);
			}
			line = nl ? nl + 1 : NULL;
			continue;
		}

		/* ---- Unordered list item ---- */
		if ((line[0] == '-' || line[0] == '*' ||
		     line[0] == '+') &&
		    llen > 1 && line[1] == ' ') {
			const char *content = line + 2;
			if (color) {
				fprintf(out, "  %s•%s ", TERM_BOLD,
					TERM_RESET);
				render_inline(content, strlen(content),
					      out, true);
			} else {
				fputs("  • ", out);
				render_inline(content, strlen(content),
					      out, false);
			}
			fputc('\n', out);
			line = nl ? nl + 1 : NULL;
			continue;
		}

		/* ---- Ordered list item ---- */
		{
			size_t d = 0;
			while (d < llen &&
			       isdigit((unsigned char)line[d]))
				d++;
			if (d > 0 && d + 1 < llen &&
			    line[d] == '.' && line[d + 1] == ' ') {
				fprintf(out, "  %.*s. ", (int)d, line);
				render_inline(line + d + 2, llen - d - 2,
					      out, color);
				fputc('\n', out);
				line = nl ? nl + 1 : NULL;
				continue;
			}
		}

		/* ---- Normal paragraph line ---- */
		fputs("  ", out);
		render_inline(line, llen, out, color);
		fputc('\n', out);

		line = nl ? nl + 1 : NULL;
	}

	free(buf);
}
