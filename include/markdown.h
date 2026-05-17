/*
 * markdown.h - Terminal markdown renderer.
 *
 * markdown_render() renders a markdown string to ANSI-formatted terminal
 * output.  Falls back to clean plain text when the output is not a TTY or
 * when NO_COLOR is set.  In plain-text mode the structural transformations
 * still apply (bullets, heading indentation, marker stripping) so that
 * piped output is readable without terminal support.
 */

#ifndef MARKDOWN_H
#define MARKDOWN_H

#include <stdio.h>

/*
 * Render markdown text to out.
 * Applies ANSI formatting when term_supports_color() is true and out is a
 * TTY; otherwise emits clean plain text with markdown markers stripped.
 * Trailing newlines in text are consumed; the caller decides spacing after.
 */
void markdown_render(const char *text, FILE *out);

#endif /* MARKDOWN_H */
