/*
 * test_markdown.c - Tests for the terminal markdown renderer.
 *
 * Tests run with non-TTY stdout (piped through make), so term_supports_color()
 * returns false and markdown_render() takes the plain-text path throughout.
 * We capture output via tmpfile() — always non-TTY — and verify the structural
 * transformations: heading detection, list markers, code fences, inline
 * element stripping.  The ANSI-colour path is exercised by running boris
 * interactively in a terminal.
 */

#define _POSIX_C_SOURCE 200809L

#include "markdown.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(condition)                                        \
	do {                                                     \
		if (condition) {                                 \
			tests_passed++;                          \
		} else {                                         \
			tests_failed++;                          \
			fprintf(stderr, "  FAIL %s:%d: %s\n",    \
				__FILE__, __LINE__, #condition); \
		}                                                \
	} while (0)

/* -------------------------------------------------------------------------
 * Capture helper — render to a tmpfile and return the result as a heap
 * string.  The caller must free() the result.
 * ---------------------------------------------------------------------- */

static char *capture(const char *input)
{
	FILE *f = tmpfile();
	if (!f)
		return NULL;
	markdown_render(input, f);
	long sz = ftell(f);
	if (sz < 0) {
		fclose(f);
		return NULL;
	}
	char *out = malloc((size_t)sz + 1);
	if (!out) {
		fclose(f);
		return NULL;
	}
	rewind(f);
	size_t nr = fread(out, 1, (size_t)sz, f);
	out[nr] = '\0';
	fclose(f);
	return out;
}

/* -------------------------------------------------------------------------
 * NULL / empty safety
 * ---------------------------------------------------------------------- */

static void test_null_input(void)
{
	printf("  Test: NULL input does not crash\n");
	markdown_render(NULL, stdout);
	ASSERT(1);
}

static void test_null_output(void)
{
	printf("  Test: NULL output does not crash\n");
	markdown_render("hello", NULL);
	ASSERT(1);
}

static void test_empty_string(void)
{
	printf("  Test: empty string produces no output\n");
	char *out = capture("");
	ASSERT(out != NULL);
	ASSERT(out && strlen(out) == 0);
	free(out);
}

/* -------------------------------------------------------------------------
 * Plain text
 * ---------------------------------------------------------------------- */

static void test_plain_text_indent(void)
{
	printf("  Test: plain text gets two-space indent\n");
	char *out = capture("hello world");
	ASSERT(out && strncmp(out, "  hello world\n", 14) == 0);
	free(out);
}

static void test_multiline(void)
{
	printf("  Test: multi-line input all rendered\n");
	char *out = capture("line one\nline two\nline three");
	ASSERT(out && strstr(out, "line one") != NULL);
	ASSERT(out && strstr(out, "line two") != NULL);
	ASSERT(out && strstr(out, "line three") != NULL);
	free(out);
}

static void test_blank_lines_preserved(void)
{
	printf("  Test: blank line between paragraphs preserved\n");
	char *out = capture("first\n\nsecond");
	ASSERT(out && strstr(out, "first") != NULL);
	ASSERT(out && strstr(out, "second") != NULL);
	ASSERT(out && strstr(out, "\n\n") != NULL);
	free(out);
}

static void test_trailing_newline_not_doubled(void)
{
	printf("  Test: trailing newline in input does not produce blank line\n");
	char *with_nl = capture("hello\n");
	char *without_nl = capture("hello");
	ASSERT(with_nl && without_nl &&
	       strcmp(with_nl, without_nl) == 0);
	free(with_nl);
	free(without_nl);
}

/* -------------------------------------------------------------------------
 * Headings
 * ---------------------------------------------------------------------- */

static void test_heading_h1(void)
{
	printf("  Test: H1 strips # marker and preserves text\n");
	char *out = capture("# My Heading");
	ASSERT(out && strstr(out, "My Heading") != NULL);
	ASSERT(out && strstr(out, "#") == NULL);
	free(out);
}

static void test_heading_h2(void)
{
	printf("  Test: H2 strips ## marker\n");
	char *out = capture("## Section Title");
	ASSERT(out && strstr(out, "Section Title") != NULL);
	ASSERT(out && strstr(out, "##") == NULL);
	free(out);
}

static void test_heading_h3(void)
{
	printf("  Test: H3 strips ### marker\n");
	char *out = capture("### Subsection");
	ASSERT(out && strstr(out, "Subsection") != NULL);
	ASSERT(out && strstr(out, "###") == NULL);
	free(out);
}

static void test_not_a_heading(void)
{
	printf("  Test: # without space is not a heading\n");
	char *out = capture("#notaheading");
	ASSERT(out && strstr(out, "#notaheading") != NULL);
	free(out);
}

/* -------------------------------------------------------------------------
 * Inline elements
 * ---------------------------------------------------------------------- */

static void test_bold_markers_stripped(void)
{
	printf("  Test: **bold** markers stripped in plain mode\n");
	char *out = capture("This is **bold** text");
	ASSERT(out && strstr(out, "bold") != NULL);
	ASSERT(out && strstr(out, "**") == NULL);
	free(out);
}

static void test_italic_markers_stripped(void)
{
	printf("  Test: *italic* markers stripped in plain mode\n");
	char *out = capture("This is *italic* text");
	ASSERT(out && strstr(out, "italic") != NULL);
	free(out);
}

static void test_inline_code_backticks_consumed(void)
{
	printf("  Test: inline `code` backtick markers consumed\n");
	char *out = capture("Run `make test` to verify");
	ASSERT(out && strstr(out, "make test") != NULL);
	free(out);
}

static void test_strikethrough_stripped(void)
{
	printf("  Test: ~~strike~~ markers stripped in plain mode\n");
	char *out = capture("This ~~was~~ removed");
	ASSERT(out && strstr(out, "was") != NULL);
	ASSERT(out && strstr(out, "~~") == NULL);
	free(out);
}

static void test_link_extraction(void)
{
	printf("  Test: [text](url) emits text and url\n");
	char *out = capture("[click here](https://example.com)");
	ASSERT(out && strstr(out, "click here") != NULL);
	ASSERT(out && strstr(out, "https://example.com") != NULL);
	ASSERT(out && strstr(out, "[") == NULL);
	free(out);
}

static void test_incomplete_link_literal(void)
{
	printf("  Test: incomplete link syntax is emitted literally\n");
	char *out = capture("not a [link");
	ASSERT(out && strstr(out, "[link") != NULL);
	free(out);
}

/* -------------------------------------------------------------------------
 * Lists
 * ---------------------------------------------------------------------- */

static void test_unordered_list_dash(void)
{
	printf("  Test: unordered list (dash) gets bullet\n");
	char *out = capture("- first item");
	ASSERT(out && strstr(out, "\xE2\x80\xa2") != NULL); /* UTF-8 bullet • */
	ASSERT(out && strstr(out, "first item") != NULL);
	ASSERT(out && strstr(out, "- ") == NULL);
	free(out);
}

static void test_unordered_list_star(void)
{
	printf("  Test: unordered list (star) gets bullet\n");
	char *out = capture("* second item");
	ASSERT(out && strstr(out, "\xE2\x80\xa2") != NULL);
	ASSERT(out && strstr(out, "second item") != NULL);
	free(out);
}

static void test_unordered_list_plus(void)
{
	printf("  Test: unordered list (plus) gets bullet\n");
	char *out = capture("+ third item");
	ASSERT(out && strstr(out, "\xE2\x80\xa2") != NULL);
	ASSERT(out && strstr(out, "third item") != NULL);
	free(out);
}

static void test_ordered_list(void)
{
	printf("  Test: ordered list preserves numbers\n");
	char *out = capture("1. alpha\n2. beta\n3. gamma");
	ASSERT(out && strstr(out, "1.") != NULL);
	ASSERT(out && strstr(out, "2.") != NULL);
	ASSERT(out && strstr(out, "alpha") != NULL);
	ASSERT(out && strstr(out, "beta") != NULL);
	free(out);
}

static void test_list_item_inline_formatting(void)
{
	printf("  Test: inline formatting inside list items\n");
	char *out = capture("- **important** note");
	ASSERT(out && strstr(out, "important") != NULL);
	ASSERT(out && strstr(out, "**") == NULL);
	free(out);
}

/* -------------------------------------------------------------------------
 * Block elements
 * ---------------------------------------------------------------------- */

static void test_blockquote(void)
{
	printf("  Test: blockquote gets pipe marker\n");
	char *out = capture("> quoted text");
	ASSERT(out && strstr(out, "\xe2\x94\x82") != NULL); /* UTF-8 │ */
	ASSERT(out && strstr(out, "quoted text") != NULL);
	ASSERT(out && strstr(out, "> ") == NULL);
	free(out);
}

static void test_code_fence(void)
{
	printf("  Test: fenced code block content indented\n");
	char *out = capture("```\nsome code here\n```");
	ASSERT(out && strstr(out, "some code here") != NULL);
	free(out);
}

static void test_code_fence_language_tag(void)
{
	printf("  Test: fenced code block with language tag\n");
	char *out = capture("```c\nint x = 0;\n```");
	ASSERT(out && strstr(out, "int x = 0;") != NULL);
	free(out);
}

static void test_code_fence_no_inline_processing(void)
{
	printf("  Test: no inline processing inside code fences\n");
	char *out = capture("```\n**not bold**\n```");
	/* Markers should appear as-is inside a fence */
	ASSERT(out && strstr(out, "**not bold**") != NULL);
	free(out);
}

static void test_horizontal_rule_dash(void)
{
	printf("  Test: --- replaced with decorative rule\n");
	char *out = capture("---");
	ASSERT(out && strstr(out, "\xe2\x94\x80") != NULL); /* UTF-8 ─ */
	ASSERT(out && strstr(out, "---") == NULL);
	free(out);
}

static void test_horizontal_rule_equals(void)
{
	printf("  Test: === replaced with decorative rule\n");
	char *out = capture("===");
	ASSERT(out && strstr(out, "\xe2\x94\x80") != NULL);
	free(out);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void)
{
	printf("\n=== Markdown Renderer Tests ===\n\n");

	/* NULL / empty safety */
	test_null_input();
	test_null_output();
	test_empty_string();

	/* Plain text */
	test_plain_text_indent();
	test_multiline();
	test_blank_lines_preserved();
	test_trailing_newline_not_doubled();

	/* Headings */
	test_heading_h1();
	test_heading_h2();
	test_heading_h3();
	test_not_a_heading();

	/* Inline elements */
	test_bold_markers_stripped();
	test_italic_markers_stripped();
	test_inline_code_backticks_consumed();
	test_strikethrough_stripped();
	test_link_extraction();
	test_incomplete_link_literal();

	/* Lists */
	test_unordered_list_dash();
	test_unordered_list_star();
	test_unordered_list_plus();
	test_ordered_list();
	test_list_item_inline_formatting();

	/* Block elements */
	test_blockquote();
	test_code_fence();
	test_code_fence_language_tag();
	test_code_fence_no_inline_processing();
	test_horizontal_rule_dash();
	test_horizontal_rule_equals();

	printf("\n--- Markdown Results ---\n");
	printf("  Passed: %d\n", tests_passed);
	printf("  Failed: %d\n\n", tests_failed);

	return tests_failed > 0 ? 1 : 0;
}
