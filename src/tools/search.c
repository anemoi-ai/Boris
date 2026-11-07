/*
 * tools/search.c - Search file contents (or names) under a sandbox path.
 *
 * Walks a directory tree in sorted order and prints "path:line: text" for
 * each matching line, with paths relative to the sandbox root. With no
 * pattern, it lists the files whose names match glob instead.
 *
 * The walk never follows symlinks (so it cannot leave the sandbox or loop),
 * skips hidden entries such as .git, skips binary and very large files, and
 * stops at a result limit, a file limit and on Ctrl+C. Output is
 * deterministic: entries are visited in byte order.
 */

#define _POSIX_C_SOURCE 200809L

#include "tools.h"
#include "tool_builtins.h"
#include "arena.h"
#include "json.h"
#include "logger.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define SEARCH_DEFAULT_RESULTS 100
#define SEARCH_MAX_RESULTS     500
#define SEARCH_MAX_FILES       20000		/* files examined per call */
#define SEARCH_MAX_FILE_BYTES  (4 * 1024 * 1024) /* larger files are skipped */
#define SEARCH_MAX_DEPTH       32
#define SEARCH_LINE_MAX	       200 /* bytes of each matching line shown */
#define SEARCH_BINARY_PROBE    8192

/* -------------------------------------------------------------------------
 * Output buffer
 * ---------------------------------------------------------------------- */

struct out_buf {
	char *data;
	size_t len;
	size_t cap;
	bool failed;
};

static void out_append(struct out_buf *o, const char *s, size_t n)
{
	if (o->failed)
		return;
	if (o->len + n + 1 > o->cap) {
		size_t cap = o->cap ? o->cap * 2 : 4096;
		while (cap < o->len + n + 1)
			cap *= 2;
		char *p = realloc(o->data, cap);
		if (!p) {
			o->failed = true;
			return;
		}
		o->data = p;
		o->cap = cap;
	}
	memcpy(o->data + o->len, s, n);
	o->len += n;
	o->data[o->len] = '\0';
}

static void out_str(struct out_buf *o, const char *s)
{
	out_append(o, s, strlen(s));
}

/* -------------------------------------------------------------------------
 * Arguments and walk state
 * ---------------------------------------------------------------------- */

struct search_args {
	char path[4096];
	const char *pattern; /* NULL: list files matching glob */
	const char *glob;    /* NULL: every file */
	bool literal;
	bool ignore_case;
	int max_results;
};

struct search_state {
	const struct search_args *args;
	regex_t regex;
	bool have_regex;
	char root[PATH_MAX]; /* resolved sandbox root, for relative paths */
	size_t root_len;
	char start[PATH_MAX]; /* resolved search path, for path globs */
	size_t start_len;
	struct out_buf out;
	int results;
	int files_matched;
	int files_examined;
	int files_skipped; /* binary, too large or unreadable */
	bool stopped;	   /* hit a limit or Ctrl+C */
};

static const char *parse_search_args(const cJSON *root, struct search_args *args)
{
	memset(args, 0, sizeof(*args));
	if (!root)
		return "Could not parse arguments as JSON";

	const char *path = json_get_str(root, "path");
	snprintf(args->path, sizeof(args->path), "%s", path && path[0] ? path : ".");
	if (path && strlen(path) >= sizeof(args->path))
		return "\"path\" is too long";

	args->pattern = json_get_str(root, "pattern");
	if (args->pattern && !args->pattern[0])
		args->pattern = NULL;
	args->glob = json_get_str(root, "glob");
	if (args->glob && !args->glob[0])
		args->glob = NULL;
	args->literal = json_get_bool(root, "literal", 0);
	args->ignore_case = json_get_bool(root, "ignore_case", 0);

	int max = json_get_int(root, "max_results", SEARCH_DEFAULT_RESULTS);
	args->max_results = max < 1 ? 1 : max > SEARCH_MAX_RESULTS ? SEARCH_MAX_RESULTS : max;

	if (!args->pattern && !args->glob)
		return "Give \"pattern\" to search file contents, \"glob\" to find "
		       "files by name, or both";
	return NULL;
}

/* Path of abs relative to the sandbox root ("." for the root itself). */
static const char *relative_path(const struct search_state *s, const char *abs)
{
	if (strncmp(abs, s->root, s->root_len) == 0) {
		if (abs[s->root_len] == '\0')
			return ".";
		if (abs[s->root_len] == '/')
			return abs + s->root_len + 1;
	}
	return abs;
}

/* -------------------------------------------------------------------------
 * Matching
 * ---------------------------------------------------------------------- */

/*
 * Does a file pass the glob filter? A glob without '/' matches the file
 * name ("*.py"). One with '/' matches the path relative to the sandbox
 * root or to the search path; '*' also crosses directories there, so the
 * path globs models write ("src/" then "*.c", or a leading "**" + "/")
 * behave as expected. A leading "**" + "/" also matches at the top level.
 */
static bool glob_matches(const struct search_state *s, const char *abs, const char *name)
{
	const char *g = s->args->glob;
	if (!g)
		return true;
	while (g[0] == '.' && g[1] == '/')
		g += 2;
	if (!strchr(g, '/'))
		return fnmatch(g, name, 0) == 0;

	const char *from_root = relative_path(s, abs);
	const char *from_start = abs;
	if (strncmp(abs, s->start, s->start_len) == 0 && abs[s->start_len] == '/')
		from_start = abs + s->start_len + 1;

	if (fnmatch(g, from_root, 0) == 0 || fnmatch(g, from_start, 0) == 0)
		return true;
	if (strncmp(g, "**/", 3) == 0) {
		const char *rest = g + 3;
		return fnmatch(rest, from_root, 0) == 0 || fnmatch(rest, from_start, 0) == 0 ||
		       (!strchr(rest, '/') && fnmatch(rest, name, 0) == 0);
	}
	return false;
}

static bool contains_ci(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	for (; *hay; hay++)
		if (strncasecmp(hay, needle, n) == 0)
			return true;
	return false;
}

static bool line_matches(const struct search_state *s, const char *line)
{
	if (s->have_regex)
		return regexec(&s->regex, line, 0, NULL, 0) == 0;
	return s->args->ignore_case ? contains_ci(line, s->args->pattern)
				    : strstr(line, s->args->pattern) != NULL;
}

/* Append "rel:lineno: text", text cut at a UTF-8 boundary. */
static void emit_match(struct search_state *s, const char *rel, size_t lineno,
		       const char *line)
{
	size_t len = strlen(line);
	bool cut = len > SEARCH_LINE_MAX;
	if (cut) {
		len = SEARCH_LINE_MAX;
		while (len > 0 && ((unsigned char)line[len] & 0xC0) == 0x80)
			len--;
	}
	char head[PATH_MAX + 32];
	snprintf(head, sizeof(head), "%s:%zu: ", rel, lineno);
	out_str(&s->out, head);
	out_append(&s->out, line, len);
	out_str(&s->out, cut ? " [...]\n" : "\n");
}

static bool limit_reached(struct search_state *s)
{
	if (s->results >= s->args->max_results ||
	    s->files_examined >= SEARCH_MAX_FILES || tools_cancelled())
		s->stopped = true;
	return s->stopped;
}

static void search_file(struct search_state *s, const char *abs, off_t size)
{
	const char *rel = relative_path(s, abs);
	s->files_examined++;

	if (!s->args->pattern) { /* name search only */
		out_str(&s->out, rel);
		out_str(&s->out, "\n");
		s->results++;
		s->files_matched++;
		return;
	}
	if (size > SEARCH_MAX_FILE_BYTES) {
		s->files_skipped++;
		return;
	}

	size_t len = 0;
	char *text = file_read_all(abs, SEARCH_MAX_FILE_BYTES, &len);
	if (!text) {
		s->files_skipped++;
		return;
	}
	if (memchr(text, '\0', len < SEARCH_BINARY_PROBE ? len : SEARCH_BINARY_PROBE)) {
		s->files_skipped++;
		free(text);
		return;
	}

	bool matched = false;
	size_t lineno = 0;
	char *line = text;
	const char *end = text + len;
	while (line && line < end && !limit_reached(s)) {
		lineno++;
		char *nl = strchr(line, '\n');
		if (nl)
			*nl = '\0';
		size_t l = strlen(line);
		if (l > 0 && line[l - 1] == '\r')
			line[l - 1] = '\0';
		if (line_matches(s, line)) {
			emit_match(s, rel, lineno, line);
			s->results++;
			matched = true;
		}
		line = nl ? nl + 1 : NULL;
	}
	if (matched)
		s->files_matched++;
	free(text);
}

static int compare_names(const struct dirent **a, const struct dirent **b)
{
	return strcmp((*a)->d_name, (*b)->d_name);
}

static int skip_hidden(const struct dirent *e)
{
	return e->d_name[0] != '.';
}

static void walk(struct search_state *s, const char *dir, int depth)
{
	struct dirent **names = NULL;
	int n = scandir(dir, &names, skip_hidden, compare_names);
	if (n < 0)
		return;

	for (int i = 0; i < n; i++) {
		if (!limit_reached(s)) {
			char child[PATH_MAX];
			int w = snprintf(child, sizeof(child), "%s/%s", dir, names[i]->d_name);
			struct stat st;
			if (w > 0 && (size_t)w < sizeof(child) && lstat(child, &st) == 0) {
				if (S_ISDIR(st.st_mode) && depth < SEARCH_MAX_DEPTH)
					walk(s, child, depth + 1);
				else if (S_ISREG(st.st_mode) &&
					 glob_matches(s, child, names[i]->d_name))
					search_file(s, child, st.st_size);
				/* symlinks, devices, FIFOs: never followed or read */
			}
		}
		free(names[i]);
	}
	free(names);
}

/* -------------------------------------------------------------------------
 * Tool entry point
 * ---------------------------------------------------------------------- */

static void append_summary(struct search_state *s)
{
	char line[512];
	if (s->results == 0 && s->args->glob && s->files_examined == 0 && !s->stopped) {
		snprintf(line, sizeof(line),
			 "No files under %.200s match glob '%.200s'. A glob without / "
			 "matches file names (*.py); with / it matches paths (src/*.py).",
			 s->args->path, s->args->glob);
		out_str(&s->out, line);
		return;
	}
	if (s->results == 0)
		snprintf(line, sizeof(line), "No %s found (%d file%s examined",
			 s->args->pattern ? "matches" : "files", s->files_examined,
			 s->files_examined == 1 ? "" : "s");
	else if (s->args->pattern)
		snprintf(line, sizeof(line), "\n%d match%s in %d file%s (%d examined",
			 s->results, s->results == 1 ? "" : "es", s->files_matched,
			 s->files_matched == 1 ? "" : "s", s->files_examined);
	else
		snprintf(line, sizeof(line), "\n%d file%s (%d examined", s->results,
			 s->results == 1 ? "" : "s", s->files_examined);
	out_str(&s->out, line);

	if (s->files_skipped) {
		snprintf(line, sizeof(line), ", %d skipped: binary or over 4 MB",
			 s->files_skipped);
		out_str(&s->out, line);
	}
	out_str(&s->out, ")");
	if (s->stopped) {
		if (tools_cancelled())
			out_str(&s->out, "\n[Cancelled by the user.]");
		else
			out_str(&s->out, "\n[Stopped at the result or file limit; "
					 "narrow the pattern, glob or path for the rest.]");
	}
}

static struct tool_result run_search(const struct search_args *args,
				     const struct agent_configuration *cfg)
{
	struct search_state s;
	memset(&s, 0, sizeof(s));
	s.args = args;

	struct tool_result err;
	if (tools_sandbox_resolve(cfg, ".", s.root, sizeof(s.root), &err) != 0)
		return err;
	s.root_len = strlen(s.root);

	if (tools_sandbox_resolve(cfg, args->path, s.start, sizeof(s.start), &err) != 0)
		return err;
	s.start_len = strlen(s.start);
	const char *start = s.start;
	struct stat st;
	if (stat(start, &st) != 0)
		return tool_result_errorf("search", "Path not found: %s", args->path);

	if (args->pattern && !args->literal) {
		int flags = REG_EXTENDED | REG_NOSUB | (args->ignore_case ? REG_ICASE : 0);
		int rc = regcomp(&s.regex, args->pattern, flags);
		if (rc != 0) {
			char msg[256];
			regerror(rc, &s.regex, msg, sizeof(msg));
			return tool_result_errorf("search",
						  "Invalid regular expression: %s. Patterns "
						  "are POSIX extended regex (use [0-9] rather "
						  "than \\d), or set literal=true.", msg);
		}
		s.have_regex = true;
	}

	if (S_ISDIR(st.st_mode))
		walk(&s, start, 0);
	else if (S_ISREG(st.st_mode))
		search_file(&s, start, st.st_size);
	else
		return tool_result_errorf("search", "Not a file or directory: %s", args->path);

	if (s.have_regex)
		regfree(&s.regex);

	append_summary(&s);
	if (s.out.failed) {
		free(s.out.data);
		return tool_result_errorf("search", "Out of memory");
	}
	return tool_result_ok("search", s.out.data);
}

struct tool_result tool_search_fn(const char *arguments_json,
				  const struct agent_configuration *cfg,
				  struct memory_arena *scratch)
{
	(void)scratch;
	cJSON *root = json_parse(arguments_json ? arguments_json : "");
	struct search_args args;
	const char *parse_err = parse_search_args(root, &args);
	struct tool_result r = parse_err ? tool_result_errorf("search", "%s", parse_err)
					 : run_search(&args, cfg);
	cJSON_Delete(root);
	return r;
}

/* "/TODO|FIXME/ in src (*.c)" or "files *.md in docs". */
void tool_search_format(const char *argument_json, char *buf, size_t size)
{
	cJSON *root = json_parse(argument_json);
	const char *pattern = json_get_str(root, "pattern");
	const char *path = json_get_str(root, "path");
	const char *glob = json_get_str(root, "glob");
	if (!path || !path[0])
		path = ".";
	if (pattern && pattern[0])
		snprintf(buf, size, "/%s/ in %s%s%s%s", pattern, path,
			 glob ? " (" : "", glob ? glob : "", glob ? ")" : "");
	else if (glob)
		snprintf(buf, size, "files %s in %s", glob, path);
	else
		snprintf(buf, size, "(invalid arguments)");
	cJSON_Delete(root);
}

/* -------------------------------------------------------------------------
 * Definition
 * ---------------------------------------------------------------------- */

static const char SCHEMA_SEARCH[] =
	"{"
	"\"type\":\"object\","
	"\"properties\":{"
	"\"pattern\":{\"type\":\"string\","
	"\"description\":\"POSIX extended regular expression to find in file "
	"contents (use [0-9] rather than \\\\d). Omit to list files by glob only.\"},"
	"\"path\":{\"type\":\"string\","
	"\"default\":\".\","
	"\"description\":\"Directory (searched recursively) or file to search.\"},"
	"\"glob\":{\"type\":\"string\","
	"\"description\":\"Only matching files: a name pattern (*.py) or a path "
	"pattern (src/*.py).\"},"
	"\"literal\":{\"type\":\"boolean\","
	"\"default\":false,"
	"\"description\":\"Treat pattern as plain text, not a regex.\"},"
	"\"ignore_case\":{\"type\":\"boolean\","
	"\"default\":false},"
	"\"max_results\":{\"type\":\"integer\","
	"\"default\":100,"
	"\"description\":\"Stop after this many matches (1-500).\"}"
	"}"
	"}";

const struct tool_definition tool_search_definition = {
	.name = "search",
	.description = "Search the sandbox recursively. With pattern: matching lines "
		       "as path:line: text. With only glob: matching file paths. "
		       "Hidden directories, symlinks and binary files are skipped. "
		       "Use this instead of reading files one by one to find where "
		       "something is.",
	.parameters_schema = SCHEMA_SEARCH,
	.fn = tool_search_fn,
	.format_call = tool_search_format,
	.flag = TOOL_SEARCH,
	.confirm = TOOL_CONFIRM_NEVER,
};
