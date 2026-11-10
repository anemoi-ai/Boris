/*
 * test_platform.c - The same answers on every system.
 *
 * On Linux and macOS regcomp(), fnmatch() and getopt_long() come from the C
 * library; on Windows they are Boris's own (src/platform_regex.c and
 * src/platform_win32.c). These tests state what Boris relies on and run
 * against whichever implementation the system has, so the two cannot drift
 * apart unnoticed.
 */

#define _GNU_SOURCE

#include "support.h"
#ifndef _WIN32
#include <fnmatch.h>
#include <getopt.h>
#include <regex.h>
#endif

static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(condition)                                                               \
	do {                                                                            \
		if (condition) {                                                        \
			tests_passed++;                                                 \
		} else {                                                                \
			tests_failed++;                                                 \
			printf("  FAIL: %s:%d - %s\n", __FILE__, __LINE__, #condition); \
		}                                                                       \
	} while (0)

/* -------------------------------------------------------------------------
 * Regular expressions
 * ---------------------------------------------------------------------- */

/* 1 match, 0 no match, -1 the pattern does not compile. */
static int regex_result(const char *pattern, const char *text, bool ignore_case)
{
	regex_t re;
	if (regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB | (ignore_case ? REG_ICASE : 0)) != 0)
		return -1;
	int rc = regexec(&re, text, 0, NULL, 0);
	regfree(&re);
	return rc == 0 ? 1 : 0;
}

static void test_regex(void)
{
	printf("  Test: extended regular expressions\n");

	static const struct {
		const char *pattern, *text;
		int expected;
	} cases[] = {
		/* literals and where a match may start */
		{"abc", "xxabcxx", 1},
		{"abc", "ab", 0},
		{"", "anything", 1},
		{"a", "", 0},
		/* anchors */
		{"^abc", "abcdef", 1},
		{"^abc", "xabc", 0},
		{"abc$", "xxabc", 1},
		{"abc$", "abcx", 0},
		{"^$", "", 1},
		{"^$", "x", 0},
		{"^a|b$", "ax", 1},
		{"^a|b$", "xb", 1},
		{"^a|b$", "xbx", 0},
		/* any character, repetition */
		{"a.c", "abc", 1},
		{"a.c", "ac", 0},
		{"ab*c", "ac", 1},
		{"ab*c", "abbbbc", 1},
		{"ab+c", "ac", 0},
		{"ab+c", "abc", 1},
		{"ab?c", "ac", 1},
		{"ab?c", "abbc", 0},
		{"^a{3}$", "aaa", 1},
		{"^a{3}$", "aa", 0},
		{"^a{3}$", "aaaa", 0},
		{"^a{2,}$", "a", 0},
		{"^a{2,}$", "aaaaa", 1},
		{"^a{2,4}$", "aaaa", 1},
		{"^a{2,4}$", "aaaaa", 0},
		{"^a{0}b$", "b", 1},
		{"^(ab){2}$", "abab", 1},
		{"^(ab){2}$", "ab", 0},
		/* alternation and grouping */
		{"TODO|FIXME", "a FIXME here", 1},
		{"TODO|FIXME", "nothing", 0},
		{"TODO.*(docs|validate)", "TODO: write the docs", 1},
		{"TODO.*(docs|validate)", "TODO: later", 0},
		{"^(a|b)*c$", "ababbac", 1},
		{"^(a|b)*c$", "abxc", 0},
		{"^(a|)b$", "b", 1},
		{"^((a*)*)*b$", "aaab", 1},
		{"x(a+)+y", "xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaz", 0},
		/* bracket expressions */
		{"[abc]", "xbx", 1},
		{"[abc]", "xyz", 0},
		{"[^abc]", "abc", 0},
		{"[^abc]", "abcd", 1},
		{"[a-f0-9]+$", "x7f", 1},
		{"^[a-c]+$", "abcd", 0},
		{"[]]", "a]b", 1},
		{"[^]]", "]]", 0},
		{"[a-]", "x-x", 1},
		{"[[:digit:]]+", "abc123", 1},
		{"[[:digit:]]+", "abc", 0},
		{"^[[:alpha:]_][[:alnum:]_]*$", "valid_name1", 1},
		{"^[[:alpha:]_][[:alnum:]_]*$", "1invalid", 0},
		{"[[:space:]]", "a b", 1},
		{"[[:upper:]]", "abc", 0},
		{"[\\]", "a\\b", 1}, /* a backslash is ordinary inside brackets */
		/* escapes */
		{"a\\.c", "a.c", 1},
		{"a\\.c", "abc", 0},
		{"f\\(x\\)", "call f(x)", 1},
		{"f\\(x\\)", "call fx", 0},
		{"a\\|b", "a|b", 1},
		{"a\\|b", "a", 0},
		{"\\$[0-9]+", "costs $15", 1},
		{"\\w+@\\w+", "mail bob@example", 1},
		{"^\\w+$", "two words", 0},
		{"a\\sb", "a b", 1},
		{"a\\Sb", "a b", 0},
		{"\\bcat\\b", "a cat sat", 1},
		{"\\bcat\\b", "concatenate", 0},
		{"\\<cat", "a catalog", 1},
		{"cat\\>", "a catalog", 0},
		/* the search tool feeds it source code */
		{"^[ \t]*#include *<[a-z/]+\\.h>", "#include <stdio.h>", 1},
		{"def +[a-z_]+\\(", "def add(a, b):", 1},
		{"[0-9]{1,3}(\\.[0-9]{1,3}){3}", "addr 192.168.1.1", 1},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		int got = regex_result(cases[i].pattern, cases[i].text, false);
		if (got != cases[i].expected)
			printf("    /%s/ on \"%s\": expected %d, got %d\n", cases[i].pattern,
			       cases[i].text, cases[i].expected, got);
		ASSERT(got == cases[i].expected);
	}

	/* REG_ICASE */
	ASSERT(regex_result("todo", "A TODO", true) == 1);
	ASSERT(regex_result("todo", "A TODO", false) == 0);
	ASSERT(regex_result("[a-c]+$", "xABC", true) == 1);
	ASSERT(regex_result("^[^a-c]+$", "ABC", true) == 0);
	ASSERT(regex_result("STRASSE", "strasse", true) == 1);

	/* Patterns that must be rejected, not silently misread */
	static const char *const invalid[] = {"(", "(a|b", "[abc", "[[:nosuch:]]", "a{2,1}",
					      "*a",  "a\\",  "[z-a]"};
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		int got = regex_result(invalid[i], "x", false);
		if (got != -1)
			printf("    /%s/ compiled\n", invalid[i]);
		ASSERT(got == -1);
	}

	/* regerror gives a message for whatever regcomp returned */
	regex_t re;
	char msg[128] = "";
	int rc = regcomp(&re, "(", REG_EXTENDED | REG_NOSUB);
	ASSERT(rc != 0);
	regerror(rc, &re, msg, sizeof(msg));
	ASSERT(msg[0] != '\0');
}

/* -------------------------------------------------------------------------
 * fnmatch
 * ---------------------------------------------------------------------- */

static void test_fnmatch(void)
{
	printf("  Test: fnmatch (flags 0, as the search tool calls it)\n");

	static const struct {
		const char *pattern, *text;
		bool matches;
	} cases[] = {
		{"*.py", "main.py", true},
		{"*.py", "main.pyc", false},
		{"*.py", ".py", true},
		{"*", "", true},
		{"*", "anything", true},
		{"?", "", false},
		{"a?c", "abc", true},
		{"a?c", "ac", false},
		{"src/*.c", "src/main.c", true},
		{"src/*.c", "src/tools/run.c", true}, /* '*' crosses '/' with flags 0 */
		{"src/*.c", "lib/main.c", false},
		{"*test*", "my_test_file", true},
		{"*a*b*c", "xxaxxbxxc", true},
		{"*a*b*c", "xxaxxcxxb", false},
		{"a*a*a*a*b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", false},
		{"[abc].txt", "b.txt", true},
		{"[abc].txt", "d.txt", false},
		{"[!abc].txt", "d.txt", true},
		{"[a-c]?", "bz", true},
		{"file[0-9][0-9]", "file07", true},
		{"file[0-9][0-9]", "file7", false},
		{"\\*.txt", "*.txt", true},
		{"\\*.txt", "a.txt", false},
		{"Makefile", "makefile", false},
		{"exact", "exact", true},
		{"exact", "exactly", false},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		bool got = fnmatch(cases[i].pattern, cases[i].text, 0) == 0;
		if (got != cases[i].matches)
			printf("    '%s' on '%s': expected %d, got %d\n", cases[i].pattern,
			       cases[i].text, cases[i].matches, got);
		ASSERT(got == cases[i].matches);
	}
}

/* -------------------------------------------------------------------------
 * getopt_long
 * ---------------------------------------------------------------------- */

#define MAX_SEEN 16

struct parsed {
	int count;
	int opts[MAX_SEEN];
	const char *args[MAX_SEEN];
	int first_operand; /* optind when parsing stopped */
};

static struct parsed parse(int argc, char **argv, const char *optstring)
{
	static const struct option long_opts[] = {
		{"config", required_argument, 0, 'c'},
		{"max-iterations", required_argument, 0, 'i'},
		{"max-retries", required_argument, 0, 'r'},
		{"json", no_argument, 0, 'j'},
		{"quiet", no_argument, 0, 'q'},
		{0, 0, 0, 0}};
	struct parsed p;
	memset(&p, 0, sizeof(p));
	optind = 0; /* start a new scan */
	opterr = 0; /* the error cases below should stay quiet */
	int opt;
	while (p.count < MAX_SEEN &&
	       (opt = getopt_long(argc, argv, optstring, long_opts, NULL)) != -1) {
		p.opts[p.count] = opt;
		p.args[p.count] = optarg;
		p.count++;
	}
	p.first_operand = optind;
	return p;
}

#define ARGC(v) ((int)(sizeof(v) / sizeof((v)[0])) - 1)

static void test_getopt(void)
{
	printf("  Test: getopt_long\n");
	const char *all = "c:i:r:jq";

	/* Options after operands are still options; the operands end up last,
	 * in their original order (boris exec "task" --json) */
	char *mixed[] = {"boris", "do", "--json", "this", "-i", "5", "now", NULL};
	struct parsed p = parse(ARGC(mixed), mixed, all);
	ASSERT(p.count == 2 && p.opts[0] == 'j' && p.opts[1] == 'i');
	ASSERT(p.count == 2 && p.args[1] && strcmp(p.args[1], "5") == 0);
	ASSERT(p.first_operand == 4);
	ASSERT(strcmp(mixed[4], "do") == 0 && strcmp(mixed[5], "this") == 0 &&
	       strcmp(mixed[6], "now") == 0);

	/* A leading '+' stops at the first operand (boris chat) */
	char *ordered[] = {"boris", "-q", "task", "--json", NULL};
	p = parse(ARGC(ordered), ordered, "+c:i:r:jq");
	ASSERT(p.count == 1 && p.opts[0] == 'q');
	ASSERT(p.first_operand == 2 && strcmp(ordered[2], "task") == 0);

	/* "--" ends the options; what follows is an operand even if it has a dash */
	char *dashes[] = {"boris", "-j", "--", "-q", "x", NULL};
	p = parse(ARGC(dashes), dashes, all);
	ASSERT(p.count == 1 && p.opts[0] == 'j');
	ASSERT(p.first_operand == 3 && strcmp(dashes[3], "-q") == 0);

	/* The forms an argument can take */
	char *forms[] = {"boris", "--config=a.ini", "--max-iterations", "7", "-r3", "-qj",
			 "-c", "b.ini", NULL};
	p = parse(ARGC(forms), forms, all);
	ASSERT(p.count == 6);
	ASSERT(p.opts[0] == 'c' && p.args[0] && strcmp(p.args[0], "a.ini") == 0);
	ASSERT(p.opts[1] == 'i' && p.args[1] && strcmp(p.args[1], "7") == 0);
	ASSERT(p.opts[2] == 'r' && p.args[2] && strcmp(p.args[2], "3") == 0);
	ASSERT(p.opts[3] == 'q' && p.opts[4] == 'j');
	ASSERT(p.opts[5] == 'c' && p.args[5] && strcmp(p.args[5], "b.ini") == 0);
	ASSERT(p.first_operand == 8);

	/* An unambiguous prefix of a long option is enough; an ambiguous one is not */
	char *prefix[] = {"boris", "--js", "--max-i", "9", NULL};
	p = parse(ARGC(prefix), prefix, all);
	ASSERT(p.count == 2 && p.opts[0] == 'j' && p.opts[1] == 'i');
	char *ambiguous[] = {"boris", "--max", "9", NULL};
	p = parse(ARGC(ambiguous), ambiguous, all);
	ASSERT(p.count >= 1 && p.opts[0] == '?');

	/* Errors are reported as '?' */
	char *unknown[] = {"boris", "-z", NULL};
	p = parse(ARGC(unknown), unknown, all);
	ASSERT(p.count == 1 && p.opts[0] == '?');
	char *unknown_long[] = {"boris", "--nope", NULL};
	p = parse(ARGC(unknown_long), unknown_long, all);
	ASSERT(p.count == 1 && p.opts[0] == '?');
	char *missing[] = {"boris", "-c", NULL};
	p = parse(ARGC(missing), missing, all);
	ASSERT(p.count == 1 && p.opts[0] == '?');
	char *missing_long[] = {"boris", "--config", NULL};
	p = parse(ARGC(missing_long), missing_long, all);
	ASSERT(p.count == 1 && p.opts[0] == '?');
	char *unwanted[] = {"boris", "--json=yes", NULL};
	p = parse(ARGC(unwanted), unwanted, all);
	ASSERT(p.count == 1 && p.opts[0] == '?');

	/* A lone "-" is an operand (boris exec - reads the task from stdin) */
	char *lone[] = {"boris", "-j", "-", NULL};
	p = parse(ARGC(lone), lone, all);
	ASSERT(p.count == 1 && p.first_operand == 2 && strcmp(lone[2], "-") == 0);

	/* Nothing to parse */
	char *none[] = {"boris", NULL};
	p = parse(ARGC(none), none, all);
	ASSERT(p.count == 0 && p.first_operand == 1);
}

int main(void)
{
	printf("\n=== Platform Tests ===\n\n");

	test_regex();
	test_fnmatch();
	test_getopt();

	printf("\n--- Platform Results ---\n");
	printf("  Passed: %d\n", tests_passed);
	printf("  Failed: %d\n", tests_failed);
	printf("\n");
	return tests_failed > 0 ? 1 : 0;
}
