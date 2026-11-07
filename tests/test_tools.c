/*
 * test_tools.c - Unit tests for the built-in tool implementations.
 *
 * Tests each tool function (read, write, list_dir, memory) directly,
 * plus tools_mask_to_string(). Uses a real temporary sandbox directory
 * and points HOME at a temporary directory so the memory tool never touches
 * the real ~/.boris.
 */

#define _POSIX_C_SOURCE 200809L

#include "tools.h"
#include "tool_builtins.h"
#include "cJSON.h"
#include "arena.h"
#include "configuration.h"
#include "boris_types.h"
#include "terminal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <time.h>

static int tests_passed = 0;
static int tests_failed = 0;

/* Shared per-suite scratch arena - matches what tools_dispatch provides */
static struct memory_arena *g_scratch;

#define ASSERT(condition)                                                               \
	do {                                                                            \
		if (condition) {                                                        \
			tests_passed++;                                                 \
		} else {                                                                \
			tests_failed++;                                                 \
			printf("  FAIL: %s:%d - %s\n", __FILE__, __LINE__, #condition); \
		}                                                                       \
	} while (0)

static char g_sandbox[256];
static char g_home[256];
static char g_saved_home[1024];

static void setup(void)
{
	snprintf(g_sandbox, sizeof(g_sandbox),
		 "/tmp/boris_tools_test_%d", (int)getpid());
	mkdir(g_sandbox, 0755);

	/* The memory tool stores under $HOME/.boris: use a throwaway HOME */
	const char *home = getenv("HOME");
	snprintf(g_saved_home, sizeof(g_saved_home), "%s", home ? home : "");
	snprintf(g_home, sizeof(g_home), "/tmp/boris_tools_home_%d", (int)getpid());
	mkdir(g_home, 0755);
	setenv("HOME", g_home, 1);

	g_scratch = arena_create(32768);
}

static void teardown(void)
{
	char cmd[PATH_MAX * 2 + 16];
	snprintf(cmd, sizeof(cmd), "rm -rf %s %s", g_sandbox, g_home);
	system(cmd);
	if (g_saved_home[0])
		setenv("HOME", g_saved_home, 1);
	arena_destroy(g_scratch);
	g_scratch = NULL;
}

static struct agent_configuration make_cfg(void)
{
	struct agent_configuration cfg;
	configuration_set_defaults(&cfg);
	cfg.sandbox_root = strdup(g_sandbox);
	return cfg;
}

/* -------------------------------------------------------------------------
 * Read tool
 * ---------------------------------------------------------------------- */

static void test_read_basic(void)
{
	printf("  Test: read - basic file read\n");

	char path[512];
	snprintf(path, sizeof(path), "%s/test.txt", g_sandbox);
	FILE *f = fopen(path, "w");
	ASSERT(f != NULL);
	if (f) {
		fprintf(f, "Hello, Boris!");
		fclose(f);
	}

	struct agent_configuration cfg = make_cfg();
	struct tool_result r = tool_read_fn("{\"path\":\"test.txt\"}", &cfg, g_scratch);

	ASSERT(r.is_error == false);
	ASSERT(r.content != NULL);
	if (r.content)
		ASSERT(strstr(r.content, "Hello, Boris!") != NULL);

	tool_result_free(&r);
	configuration_destroy(&cfg);
	unlink(path);
}

static void test_read_not_found(void)
{
	printf("  Test: read - file not found returns error\n");

	struct agent_configuration cfg = make_cfg();
	struct tool_result r = tool_read_fn(
		"{\"path\":\"no_such_file.txt\"}", &cfg, g_scratch);

	ASSERT(r.is_error == true);

	tool_result_free(&r);
	configuration_destroy(&cfg);
}

static void test_read_is_directory(void)
{
	printf("  Test: read - reading a directory returns error\n");

	char subdir[PATH_MAX];
	snprintf(subdir, sizeof(subdir), "%s/adir", g_sandbox);
	mkdir(subdir, 0755);

	struct agent_configuration cfg = make_cfg();
	struct tool_result r = tool_read_fn("{\"path\":\"adir\"}", &cfg, g_scratch);

	ASSERT(r.is_error == true);

	tool_result_free(&r);
	configuration_destroy(&cfg);
}

static void test_read_traversal_blocked(void)
{
	printf("  Test: read - path traversal is blocked\n");

	struct agent_configuration cfg = make_cfg();
	struct tool_result r = tool_read_fn(
		"{\"path\":\"../../etc/passwd\"}", &cfg, g_scratch);

	ASSERT(r.is_error == true);

	tool_result_free(&r);
	configuration_destroy(&cfg);
}

static void test_read_no_sandbox(void)
{
	printf("  Test: read - no sandbox root returns error\n");

	struct agent_configuration cfg;
	configuration_set_defaults(&cfg);
	/* sandbox_root is NULL */

	struct tool_result r = tool_read_fn("{\"path\":\"test.txt\"}", &cfg, g_scratch);
	ASSERT(r.is_error == true);

	tool_result_free(&r);
	configuration_destroy(&cfg);
}

static void test_read_offset_length(void)
{
	printf("  Test: read - offset and length slice the content\n");

	char path[512];
	snprintf(path, sizeof(path), "%s/slice.txt", g_sandbox);
	FILE *f = fopen(path, "w");
	ASSERT(f != NULL);
	if (f) {
		fprintf(f, "ABCDEFGHIJ");
		fclose(f);
	}

	struct agent_configuration cfg = make_cfg();
	struct tool_result r = tool_read_fn(
		"{\"path\":\"slice.txt\",\"offset\":3,\"length\":4}", &cfg, g_scratch);

	ASSERT(r.is_error == false);
	ASSERT(r.content != NULL);
	if (r.content)
		ASSERT(strncmp(r.content, "DEFG", 4) == 0);

	tool_result_free(&r);
	configuration_destroy(&cfg);
	unlink(path);
}

static void test_read_binary_blocked(void)
{
	printf("  Test: read - binary content is blocked\n");

	char path[512];
	snprintf(path, sizeof(path), "%s/binary.bin", g_sandbox);
	FILE *f = fopen(path, "wb");
	ASSERT(f != NULL);
	if (f) {
		/* >30% non-printable bytes triggers binary detection */
		for (int i = 0; i < 100; i++)
			fputc(i < 40 ? '\x02' : 'A', f);
		fclose(f);
	}

	struct agent_configuration cfg = make_cfg();
	struct tool_result r = tool_read_fn("{\"path\":\"binary.bin\"}", &cfg, g_scratch);

	ASSERT(r.is_error == true);
	if (r.content)
		ASSERT(strstr(r.content, "Binary") != NULL);

	tool_result_free(&r);
	configuration_destroy(&cfg);
	unlink(path);
}

static void test_read_truncation_note(void)
{
	printf("  Test: read - truncation note added for large files\n");

	char path[512];
	snprintf(path, sizeof(path), "%s/large.txt", g_sandbox);
	FILE *f = fopen(path, "w");
	ASSERT(f != NULL);
	if (f) {
		/* Write 200 bytes; read only 10 via length param */
		for (int i = 0; i < 200; i++)
			fputc('X', f);
		fclose(f);
	}

	struct agent_configuration cfg = make_cfg();
	struct tool_result r = tool_read_fn(
		"{\"path\":\"large.txt\",\"length\":10}", &cfg, g_scratch);

	ASSERT(r.is_error == false);
	ASSERT(r.content != NULL);
	if (r.content)
		ASSERT(strstr(r.content, "truncated") != NULL ||
		       strstr(r.content, "Truncated") != NULL);

	tool_result_free(&r);
	configuration_destroy(&cfg);
	unlink(path);
}

/* -------------------------------------------------------------------------
 * Write tool
 * ---------------------------------------------------------------------- */

static void test_write_overwrite(void)
{
	printf("  Test: write - overwrite mode creates/replaces file\n");

	struct agent_configuration cfg = make_cfg();
	struct tool_result r = tool_write_fn(
		"{\"path\":\"out.txt\",\"content\":\"written\",\"mode\":\"overwrite\"}",
		&cfg, g_scratch);

	ASSERT(r.is_error == false);

	char path[512];
	snprintf(path, sizeof(path), "%s/out.txt", g_sandbox);
	FILE *f = fopen(path, "r");
	ASSERT(f != NULL);
	if (f) {
		char buf[64] = {0};
		fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		ASSERT(strcmp(buf, "written") == 0);
	}

	tool_result_free(&r);
	configuration_destroy(&cfg);
	unlink(path);
}

static void test_write_append(void)
{
	printf("  Test: write - append mode appends to existing file\n");

	char path[512];
	snprintf(path, sizeof(path), "%s/append.txt", g_sandbox);
	FILE *f = fopen(path, "w");
	ASSERT(f != NULL);
	if (f) {
		fprintf(f, "first");
		fclose(f);
	}

	struct agent_configuration cfg = make_cfg();
	struct tool_result r = tool_write_fn(
		"{\"path\":\"append.txt\",\"content\":\",second\",\"mode\":\"append\"}",
		&cfg, g_scratch);

	ASSERT(r.is_error == false);

	f = fopen(path, "r");
	ASSERT(f != NULL);
	if (f) {
		char buf[64] = {0};
		fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		ASSERT(strcmp(buf, "first,second") == 0);
	}

	tool_result_free(&r);
	configuration_destroy(&cfg);
	unlink(path);
}

static void test_write_create_new_success(void)
{
	printf("  Test: write - create_new succeeds on new file\n");

	char path[512];
	snprintf(path, sizeof(path), "%s/brand_new.txt", g_sandbox);
	unlink(path);

	struct agent_configuration cfg = make_cfg();
	struct tool_result r = tool_write_fn(
		"{\"path\":\"brand_new.txt\",\"content\":\"new\",\"mode\":\"create_new\"}",
		&cfg, g_scratch);

	ASSERT(r.is_error == false);

	tool_result_free(&r);
	configuration_destroy(&cfg);
	unlink(path);
}

static void test_write_create_new_fails_if_exists(void)
{
	printf("  Test: write - create_new fails if file already exists\n");

	char path[512];
	snprintf(path, sizeof(path), "%s/exists.txt", g_sandbox);
	FILE *f = fopen(path, "w");
	if (f) {
		fprintf(f, "x");
		fclose(f);
	}

	struct agent_configuration cfg = make_cfg();
	struct tool_result r = tool_write_fn(
		"{\"path\":\"exists.txt\",\"content\":\"y\",\"mode\":\"create_new\"}",
		&cfg, g_scratch);

	ASSERT(r.is_error == true);

	tool_result_free(&r);
	configuration_destroy(&cfg);
	unlink(path);
}

static void test_write_create_dirs(void)
{
	printf("  Test: write - create_dirs creates parent directories\n");

	struct agent_configuration cfg = make_cfg();
	struct tool_result r = tool_write_fn(
		"{\"path\":\"deep/nested/file.txt\",\"content\":\"deep\","
		"\"create_dirs\":true}",
		&cfg, g_scratch);

	ASSERT(r.is_error == false);

	char path[512];
	snprintf(path, sizeof(path), "%s/deep/nested/file.txt", g_sandbox);
	struct stat st;
	ASSERT(stat(path, &st) == 0);
	ASSERT(S_ISREG(st.st_mode));

	tool_result_free(&r);
	configuration_destroy(&cfg);
}

static void test_write_traversal_blocked(void)
{
	printf("  Test: write - path traversal is blocked\n");

	struct agent_configuration cfg = make_cfg();
	struct tool_result r = tool_write_fn(
		"{\"path\":\"../../tmp/evil.txt\",\"content\":\"evil\"}",
		&cfg, g_scratch);

	ASSERT(r.is_error == true);

	tool_result_free(&r);
	configuration_destroy(&cfg);
}

static void test_write_large_content_via_dispatch(void)
{
	printf("  Test: write - content larger than the dispatch arena block\n");

	struct agent_configuration cfg = make_cfg();
	cfg.tools_enabled = TOOL_ALL;
	tools_register_builtins();
	size_t n = 100000;
	char *json = malloc(n + 64);
	ASSERT(json != NULL);
	if (!json)
		return;
	int p = snprintf(json, 64, "{\"path\":\"big.txt\",\"content\":\"");
	memset(json + p, 'A', n);
	snprintf(json + p + n, 8, "\"}");

	struct tool_call tc;
	memset(&tc, 0, sizeof(tc));
	snprintf(tc.id, sizeof(tc.id), "c1");
	snprintf(tc.name, sizeof(tc.name), "write");
	tc.argument_json = json;
	struct tool_result r = tools_dispatch(&tc, &cfg);

	char path[512];
	snprintf(path, sizeof(path), "%s/big.txt", g_sandbox);
	struct stat st;
	ASSERT(r.is_error == false);
	ASSERT(stat(path, &st) == 0 && (size_t)st.st_size == n);

	unlink(path);
	free(json);
	tool_result_free(&r);
	configuration_destroy(&cfg);
}

static void test_write_create_dirs_escape_blocked(void)
{
	printf("  Test: write - create_dirs cannot escape via ..\n");

	struct agent_configuration cfg = make_cfg();
	char json[256], outside[512], created[512];
	snprintf(json, sizeof(json),
		 "{\"path\":\"x/../../boris_escape_%d.txt\",\"content\":\"evil\","
		 "\"create_dirs\":true}", (int)getpid());
	snprintf(outside, sizeof(outside), "/tmp/boris_escape_%d.txt", (int)getpid());
	snprintf(created, sizeof(created), "%s/x", g_sandbox);

	struct tool_result r = tool_write_fn(json, &cfg, g_scratch);

	struct stat st;
	ASSERT(r.is_error == true);
	ASSERT(stat(outside, &st) != 0);
	ASSERT(stat(created, &st) != 0);

	unlink(outside);
	tool_result_free(&r);
	configuration_destroy(&cfg);
}

/* -------------------------------------------------------------------------
 * List dir tool
 * ---------------------------------------------------------------------- */

static void test_list_dir_basic(void)
{
	printf("  Test: list_dir - lists files in directory\n");

	char path[512];
	snprintf(path, sizeof(path), "%s/visible.txt", g_sandbox);
	FILE *f = fopen(path, "w");
	if (f) {
		fputc('v', f);
		fclose(f);
	}

	struct agent_configuration cfg = make_cfg();
	struct tool_result r = tool_list_dir_fn("{\"path\":\".\"}", &cfg, g_scratch);

	ASSERT(r.is_error == false);
	ASSERT(r.content != NULL);
	if (r.content)
		ASSERT(strstr(r.content, "visible.txt") != NULL);

	tool_result_free(&r);
	configuration_destroy(&cfg);
	unlink(path);
}

static void test_list_dir_hidden_hidden_off(void)
{
	printf("  Test: list_dir - hidden files excluded by default\n");

	char visible[512], hidden[512];
	snprintf(visible, sizeof(visible), "%s/visible.txt", g_sandbox);
	snprintf(hidden, sizeof(hidden), "%s/.hidden", g_sandbox);
	FILE *f = fopen(visible, "w");
	if (f) {
		fputc('v', f);
		fclose(f);
	}
	f = fopen(hidden, "w");
	if (f) {
		fputc('h', f);
		fclose(f);
	}

	struct agent_configuration cfg = make_cfg();
	struct tool_result r = tool_list_dir_fn(
		"{\"path\":\".\",\"show_hidden\":false}", &cfg, g_scratch);

	ASSERT(r.is_error == false);
	if (r.content)
		ASSERT(strstr(r.content, ".hidden") == NULL);

	tool_result_free(&r);
	configuration_destroy(&cfg);
	unlink(visible);
	unlink(hidden);
}

static void test_list_dir_hidden_hidden_on(void)
{
	printf("  Test: list_dir - hidden files included when show_hidden=true\n");

	char visible[512], hidden[512];
	snprintf(visible, sizeof(visible), "%s/visible.txt", g_sandbox);
	snprintf(hidden, sizeof(hidden), "%s/.hidden", g_sandbox);
	FILE *f = fopen(visible, "w");
	if (f) {
		fputc('v', f);
		fclose(f);
	}
	f = fopen(hidden, "w");
	if (f) {
		fputc('h', f);
		fclose(f);
	}

	struct agent_configuration cfg = make_cfg();
	struct tool_result r = tool_list_dir_fn(
		"{\"path\":\".\",\"show_hidden\":true}", &cfg, g_scratch);

	ASSERT(r.is_error == false);
	if (r.content)
		ASSERT(strstr(r.content, ".hidden") != NULL);

	tool_result_free(&r);
	configuration_destroy(&cfg);
	unlink(visible);
	unlink(hidden);
}

static void test_list_dir_not_a_dir(void)
{
	printf("  Test: list_dir - listing a file returns error\n");

	char path[512];
	snprintf(path, sizeof(path), "%s/file.txt", g_sandbox);
	FILE *f = fopen(path, "w");
	if (f) {
		fputc('x', f);
		fclose(f);
	}

	struct agent_configuration cfg = make_cfg();
	struct tool_result r = tool_list_dir_fn(
		"{\"path\":\"file.txt\"}", &cfg, g_scratch);

	ASSERT(r.is_error == true);

	tool_result_free(&r);
	configuration_destroy(&cfg);
	unlink(path);
}

static void test_list_dir_not_found(void)
{
	printf("  Test: list_dir - non-existent directory returns error\n");

	struct agent_configuration cfg = make_cfg();
	struct tool_result r = tool_list_dir_fn(
		"{\"path\":\"no_such_dir\"}", &cfg, g_scratch);

	ASSERT(r.is_error == true);

	tool_result_free(&r);
	configuration_destroy(&cfg);
}

/* -------------------------------------------------------------------------
 * Memory tool
 * ---------------------------------------------------------------------- */

static struct agent_configuration make_memory_cfg(void)
{
	struct agent_configuration cfg;
	configuration_set_defaults(&cfg);
	cfg.memory_persist = true;
	return cfg;
}

static void test_memory_set_get(void)
{
	printf("  Test: memory - set then get\n");

	struct agent_configuration cfg = make_memory_cfg();

	struct tool_result s = tool_memory_fn(
		"{\"action\":\"set\",\"key\":\"greeting\",\"value\":\"hello world\"}",
		&cfg, g_scratch);
	ASSERT(s.is_error == false);
	tool_result_free(&s);

	struct tool_result g = tool_memory_fn(
		"{\"action\":\"get\",\"key\":\"greeting\"}", &cfg, g_scratch);
	ASSERT(g.is_error == false);
	ASSERT(g.content != NULL);
	if (g.content) {
		ASSERT(strstr(g.content, "\"found\":true") != NULL);
		ASSERT(strstr(g.content, "hello world") != NULL);
	}
	tool_result_free(&g);
	configuration_destroy(&cfg);
}

static void test_memory_get_missing(void)
{
	printf("  Test: memory - get missing key returns found=false\n");

	struct agent_configuration cfg = make_memory_cfg();
	struct tool_result r = tool_memory_fn(
		"{\"action\":\"get\",\"key\":\"no_such_key_xyzzy\"}", &cfg, g_scratch);

	ASSERT(r.is_error == false);
	if (r.content)
		ASSERT(strstr(r.content, "\"found\":false") != NULL);

	tool_result_free(&r);
	configuration_destroy(&cfg);
}

static void test_memory_delete(void)
{
	printf("  Test: memory - set then delete, then get returns found=false\n");

	struct agent_configuration cfg = make_memory_cfg();

	struct tool_result s = tool_memory_fn(
		"{\"action\":\"set\",\"key\":\"temp\",\"value\":\"bye\"}",
		&cfg, g_scratch);
	ASSERT(s.is_error == false);
	tool_result_free(&s);

	struct tool_result d = tool_memory_fn(
		"{\"action\":\"delete\",\"key\":\"temp\"}", &cfg, g_scratch);
	ASSERT(d.is_error == false);
	if (d.content)
		ASSERT(strstr(d.content, "\"ok\":true") != NULL);
	tool_result_free(&d);

	struct tool_result g = tool_memory_fn(
		"{\"action\":\"get\",\"key\":\"temp\"}", &cfg, g_scratch);
	ASSERT(g.is_error == false);
	if (g.content)
		ASSERT(strstr(g.content, "\"found\":false") != NULL);
	tool_result_free(&g);

	configuration_destroy(&cfg);
}

static void test_memory_list(void)
{
	printf("  Test: memory - list returns entries array\n");

	struct agent_configuration cfg = make_memory_cfg();

	struct tool_result s = tool_memory_fn(
		"{\"action\":\"set\",\"key\":\"listed\",\"value\":\"yes\"}",
		&cfg, g_scratch);
	tool_result_free(&s);

	struct tool_result r = tool_memory_fn("{\"action\":\"list\"}", &cfg, g_scratch);
	ASSERT(r.is_error == false);
	ASSERT(r.content != NULL);

	tool_result_free(&r);
	configuration_destroy(&cfg);
}

static void test_memory_unknown_action(void)
{
	printf("  Test: memory - unknown action returns error\n");

	struct agent_configuration cfg = make_memory_cfg();
	struct tool_result r = tool_memory_fn(
		"{\"action\":\"explode\"}", &cfg, g_scratch);

	ASSERT(r.is_error == true);
	if (r.content)
		ASSERT(strstr(r.content, "Unknown action") != NULL);

	tool_result_free(&r);
	configuration_destroy(&cfg);
}

static void test_memory_no_persist(void)
{
	printf("  Test: memory - memory_persist=false returns error\n");

	struct agent_configuration cfg;
	configuration_set_defaults(&cfg);
	cfg.memory_persist = false;

	struct tool_result r = tool_memory_fn(
		"{\"action\":\"get\",\"key\":\"foo\"}", &cfg, g_scratch);
	ASSERT(r.is_error == true);

	tool_result_free(&r);
	configuration_destroy(&cfg);

}

static void test_memory_keys_case_sensitive(void)
{
	printf("  Test: memory - keys are case-sensitive\n");

	struct agent_configuration cfg = make_cfg();
	cfg.memory_persist = true;

	struct tool_result r = tool_memory_fn(
		"{\"action\":\"set\",\"key\":\"apple\",\"value\":\"red\"}", &cfg, g_scratch);
	tool_result_free(&r);
	r = tool_memory_fn(
		"{\"action\":\"set\",\"key\":\"APPLE\",\"value\":\"green\"}", &cfg, g_scratch);
	ASSERT(r.content && strstr(r.content, "\"created\":true"));
	tool_result_free(&r);

	r = tool_memory_fn("{\"action\":\"get\",\"key\":\"apple\"}", &cfg, g_scratch);
	ASSERT(r.content && strstr(r.content, "\"value\":\"red\""));
	tool_result_free(&r);

	r = tool_memory_fn("{\"action\":\"delete\",\"key\":\"APPLE\"}", &cfg, g_scratch);
	tool_result_free(&r);
	r = tool_memory_fn("{\"action\":\"get\",\"key\":\"apple\"}", &cfg, g_scratch);
	ASSERT(r.content && strstr(r.content, "\"found\":true"));
	tool_result_free(&r);

	/* The store lives under the test HOME, not the developer's */
	char path[512];
	snprintf(path, sizeof(path), "%s/.boris/memory/memory.json", g_home);
	struct stat st;
	ASSERT(stat(path, &st) == 0);

	configuration_destroy(&cfg);
}

static void test_memory_empty_file(void)
{
	printf("  Test: memory - an empty store file is treated as empty\n");

	struct agent_configuration cfg = make_cfg();
	cfg.memory_persist = true;

	char path[512];
	snprintf(path, sizeof(path), "%s/.boris/memory/memory.json", g_home);
	FILE *f = fopen(path, "w");
	if (f)
		fclose(f);

	struct tool_result r = tool_memory_fn(
		"{\"action\":\"set\",\"key\":\"k\",\"value\":\"v\"}", &cfg, g_scratch);
	ASSERT(r.is_error == false);
	tool_result_free(&r);
	configuration_destroy(&cfg);
}

static int g_confirm_calls;
static bool g_confirm_answer;
static char g_confirm_summary[256];

static bool test_confirm_cb(const struct tool_call *call, const char *summary,
			    void *ctx)
{
	(void)call;
	(void)ctx;
	g_confirm_calls++;
	snprintf(g_confirm_summary, sizeof(g_confirm_summary), "%s", summary);
	return g_confirm_answer;
}

static const struct tool_observer g_confirm_observer = {.confirm = test_confirm_cb};

static void test_confirm_writes(void)
{
	printf("  Test: confirm_writes asks before writing\n");

	struct agent_configuration cfg = make_cfg();
	cfg.tools_enabled = TOOL_ALL;
	tools_register_builtins();

	struct tool_call tc;
	memset(&tc, 0, sizeof(tc));
	snprintf(tc.id, sizeof(tc.id), "c1");
	snprintf(tc.name, sizeof(tc.name), "write");
	tc.argument_json = "{\"path\":\"confirm.txt\",\"content\":\"x\"}";

	char path[512];
	snprintf(path, sizeof(path), "%s/confirm.txt", g_sandbox);
	struct stat st;

	/* Off: no prompt */
	g_confirm_calls = 0;
	tools_set_observer(&g_confirm_observer);
	struct tool_result r = tools_dispatch(&tc, &cfg);
	ASSERT(g_confirm_calls == 0 && r.is_error == false);
	tool_result_free(&r);
	unlink(path);

	/* On, declined: nothing written */
	cfg.confirm_writes = true;
	g_confirm_answer = false;
	r = tools_dispatch(&tc, &cfg);
	ASSERT(g_confirm_calls == 1 && r.is_error == true);
	ASSERT(strcmp(g_confirm_summary, "confirm.txt (overwrite, 1 bytes)") == 0);
	ASSERT(stat(path, &st) != 0);
	tool_result_free(&r);

	/* On, accepted */
	g_confirm_answer = true;
	r = tools_dispatch(&tc, &cfg);
	ASSERT(g_confirm_calls == 2 && r.is_error == false);
	ASSERT(stat(path, &st) == 0);
	tool_result_free(&r);

	/* On, no callback registered: refused */
	tools_set_observer(NULL);
	r = tools_dispatch(&tc, &cfg);
	ASSERT(r.is_error == true);
	tool_result_free(&r);

	unlink(path);
	configuration_destroy(&cfg);
}

/* -------------------------------------------------------------------------
 * Tool call display (based on upstream PR #3 by @ilmich)
 * ---------------------------------------------------------------------- */

static void format_call(const char *name, const char *args, char *buf, size_t size)
{
	struct tool_call tc;
	memset(&tc, 0, sizeof(tc));
	snprintf(tc.name, sizeof(tc.name), "%s", name);
	tc.argument_json = (char *)args;
	tools_format_call(&tc, buf, size);
}

static void test_format_call_summaries(void)
{
	printf("  Test: tools_format_call summaries for each tool\n");
	tools_register_builtins();
	char buf[256];

	format_call("read", "{\"path\":\"notes.md\"}", buf, sizeof(buf));
	ASSERT(strcmp(buf, "notes.md") == 0);
	format_call("read", "{\"path\":\"notes.md\",\"offset\":4096}", buf, sizeof(buf));
	ASSERT(strcmp(buf, "notes.md (from byte 4096)") == 0);
	format_call("list_dir", "{\"path\":\"\",\"show_hidden\":true}", buf, sizeof(buf));
	ASSERT(strcmp(buf, ". (including hidden)") == 0);
	format_call("memory", "{\"action\":\"set\",\"key\":\"colour\",\"value\":\"x\"}", buf, sizeof(buf));
	ASSERT(strcmp(buf, "set colour") == 0);
	format_call("run", "{\"path\":\"b.sh\",\"args\":[\"-v\",\"x\"],\"stdin\":\"abc\"}", buf, sizeof(buf));
	ASSERT(strcmp(buf, "b.sh -v x (stdin: 3 bytes)") == 0);
	format_call("read", "not json", buf, sizeof(buf));
	ASSERT(strcmp(buf, "(invalid arguments)") == 0);

	/* write measures 100 KB of content in place (the PR copied it into a
	 * 4 KB arena, overflowing the heap) */
	size_t n = 100000;
	char *json = malloc(n + 64);
	ASSERT(json != NULL);
	if (json) {
		int p = snprintf(json, 64, "{\"path\":\"big.txt\",\"mode\":\"append\",\"content\":\"");
		memset(json + p, 'A', n);
		snprintf(json + p + n, 8, "\"}");
		format_call("write", json, buf, sizeof(buf));
		ASSERT(strcmp(buf, "big.txt (append, 100000 bytes)") == 0);
		free(json);
	}

	/* Unknown tools fall back to the raw arguments */
	format_call("mystery", "{\"a\":1}", buf, sizeof(buf));
	ASSERT(strcmp(buf, "{\"a\":1}") == 0);

	/* Model-written escape sequences never reach the terminal */
	format_call("read", "{\"path\":\"x\u001b]0;pwned\u0007\u001b[2J.md\"}", buf, sizeof(buf));
	ASSERT(strchr(buf, '\x1b') == NULL && strchr(buf, '\x07') == NULL);
}

static void test_term_sanitize(void)
{
	printf("  Test: term_sanitize strips controls and cuts on UTF-8 boundaries\n");
	char buf[64];

	term_sanitize("a\x1b[2Jb\tc\nd\x7f", buf, sizeof(buf));
	ASSERT(strcmp(buf, "a [2Jb\tc d ") == 0);

	/* C1 control (U+009B, 8-bit CSI) and a raw invalid 0x9B byte */
	term_sanitize("x\xc2\x9by\x9bz", buf, sizeof(buf));
	ASSERT(strcmp(buf, "x  y?z") == 0);

	/* Valid multi-byte text passes through */
	term_sanitize("caf\xc3\xa9 \xe2\x82\xac", buf, sizeof(buf));
	ASSERT(strcmp(buf, "caf\xc3\xa9 \xe2\x82\xac") == 0);

	/* Truncation never splits a character (the euro sign is 3 bytes) */
	char small[10];
	term_sanitize("ab\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac", small, sizeof(small));
	ASSERT(strcmp(small, "ab\xe2\x82\xac...") == 0);
	term_sanitize("ab\xe2\x82\xac\xe2\x82\xac", small, 8);
	ASSERT(strcmp(small, "ab...") == 0);
	term_sanitize("ab\xe2\x82\xac\xe2\x82\xac", small, 9); /* fits exactly */
	ASSERT(strcmp(small, "ab\xe2\x82\xac\xe2\x82\xac") == 0);

	term_sanitize(NULL, buf, sizeof(buf));
	ASSERT(buf[0] == '\0');
}

static int g_result_calls;
static int g_result_errors;

static void count_results(const struct tool_call *call, const char *summary,
			  const struct tool_result *result, void *ctx)
{
	(void)call;
	(void)summary;
	(void)ctx;
	g_result_calls++;
	if (result->is_error)
		g_result_errors++;
}

static bool decline_all(const struct tool_call *call, const char *summary, void *ctx)
{
	(void)call;
	(void)summary;
	(void)ctx;
	return false;
}

static void test_observer_sees_every_outcome(void)
{
	printf("  Test: observer is told about unknown, disabled, declined and successful calls\n");

	struct agent_configuration cfg = make_cfg();
	cfg.tools_enabled = TOOL_READ | TOOL_WRITE | TOOL_LIST_DIR;
	cfg.confirm_writes = true;
	tools_register_builtins();
	const struct tool_observer obs = {.confirm = decline_all, .on_result = count_results};
	tools_set_observer(&obs);
	g_result_calls = g_result_errors = 0;

	struct tool_call tc;
	memset(&tc, 0, sizeof(tc));
	const struct {
		const char *name, *args;
	} calls[] = {
		{"mystery", "{}"},                                     /* unknown  */
		{"memory", "{\"action\":\"list\"}"},                   /* disabled */
		{"write", "{\"path\":\"no.txt\",\"content\":\"x\"}"}, /* declined */
		{"list_dir", "{\"path\":\".\"}"},                      /* success  */
	};
	for (size_t i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
		snprintf(tc.name, sizeof(tc.name), "%s", calls[i].name);
		tc.argument_json = (char *)calls[i].args;
		struct tool_result r = tools_dispatch(&tc, &cfg);
		tool_result_free(&r);
	}

	ASSERT(g_result_calls == 4);
	ASSERT(g_result_errors == 3);

	tools_set_observer(NULL);
	configuration_destroy(&cfg);
}

static void test_write_preserves_mode_and_validates(void)
{
	printf("  Test: write - overwrite keeps file mode; bad mode rejected\n");

	struct agent_configuration cfg = make_cfg();
	char path[512];
	snprintf(path, sizeof(path), "%s/script.sh", g_sandbox);
	FILE *f = fopen(path, "w");
	if (f) {
		fputs("#!/bin/sh\n", f);
		fclose(f);
	}
	chmod(path, 0755);

	struct tool_result r = tool_write_fn(
		"{\"path\":\"script.sh\",\"content\":\"#!/bin/sh\\necho hi\\n\"}",
		&cfg, g_scratch);
	struct stat st;
	ASSERT(r.is_error == false);
	ASSERT(stat(path, &st) == 0 && (st.st_mode & 0777) == 0755);
	tool_result_free(&r);

	r = tool_write_fn("{\"path\":\"x.txt\",\"content\":\"x\",\"mode\":\"bogus\"}",
			  &cfg, g_scratch);
	ASSERT(r.is_error == true);
	tool_result_free(&r);

	unlink(path);
	configuration_destroy(&cfg);
}

static void write_script(const char *name, const char *body)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", g_sandbox, name);
	FILE *f = fopen(path, "w");
	if (f) {
		fputs(body, f);
		fclose(f);
	}
}

static void test_run_script_ignores_stdin(void)
{
	printf("  Test: run - script that exits without reading stdin\n");

	write_script("nostdin.sh", "#!/bin/sh\nexit 0\n");
	struct agent_configuration cfg = make_cfg();

	char json[9000];
	int p = snprintf(json, sizeof(json), "{\"path\":\"nostdin.sh\",\"stdin\":\"");
	memset(json + p, 'x', 8000);
	snprintf(json + p + 8000, sizeof(json) - (size_t)p - 8000, "\"}");

	/* Without SIGPIPE handling this would kill the test process */
	struct tool_result r = tool_run_fn(json, &cfg, g_scratch);
	ASSERT(r.content && strstr(r.content, "Exit code: 0"));
	tool_result_free(&r);
	configuration_destroy(&cfg);
}

static void test_run_environment_allowlist(void)
{
	printf("  Test: run - scripts do not see BORIS_* variables\n");

	setenv("BORIS_API_KEY", "sk-secret-test-key", 1);
	setenv("BORIS_EXTRA", "also-secret", 1);
	setenv("RUN_ENV_TEST_VAR", "passed-through", 1);
	setenv("RUN_ENV_HIDDEN_VAR", "not-listed", 1);
	write_script("env.sh", "#!/bin/sh\nenv\n");

	struct agent_configuration cfg = make_cfg();
	struct tool_result r = tool_run_fn("{\"path\":\"env.sh\"}", &cfg, g_scratch);
	ASSERT(r.content && strstr(r.content, "Exit code: 0"));
	ASSERT(r.content && !strstr(r.content, "sk-secret-test-key"));
	ASSERT(r.content && !strstr(r.content, "RUN_ENV_TEST_VAR"));
	ASSERT(r.content && strstr(r.content, "PATH="));
	ASSERT(r.content && strstr(r.content, "PYTHONUNBUFFERED=1"));
	tool_result_free(&r);

	/* tools.run_env adds names, but never BORIS_* ones */
	cfg.run_env = strdup("RUN_ENV_TEST_VAR, BORIS_EXTRA");
	r = tool_run_fn("{\"path\":\"env.sh\"}", &cfg, g_scratch);
	ASSERT(r.content && strstr(r.content, "RUN_ENV_TEST_VAR=passed-through"));
	ASSERT(r.content && !strstr(r.content, "also-secret"));
	ASSERT(r.content && !strstr(r.content, "RUN_ENV_HIDDEN_VAR"));
	tool_result_free(&r);
	configuration_destroy(&cfg);

	unsetenv("BORIS_API_KEY");
	unsetenv("BORIS_EXTRA");
	unsetenv("RUN_ENV_TEST_VAR");
	unsetenv("RUN_ENV_HIDDEN_VAR");
}

static void test_run_timeout_after_stdout_closed(void)
{
	printf("  Test: run - timeout applies after the script closes stdout\n");

	write_script("detach.sh",
		     "#!/bin/sh\nexec >/dev/null 2>&1\nsleep 30 &\nsleep 30\n");
	struct agent_configuration cfg = make_cfg();

	time_t start = time(NULL);
	struct tool_result r = tool_run_fn(
		"{\"path\":\"detach.sh\",\"timeout_seconds\":1}", &cfg, g_scratch);
	ASSERT(time(NULL) - start < 5);
	ASSERT(r.content && strstr(r.content, "Timed out"));
	tool_result_free(&r);
	configuration_destroy(&cfg);
}

static void test_run_timeout_keeps_python_output(void)
{
	printf("  Test: run - a Python script killed at the timeout keeps its output\n");

	/* Found in a live eval: the model's script printed results, then hung;
	 * block-buffered stdout meant the tool reported nothing. */
	write_script("hang.py", "import time\nprint('before the hang')\ntime.sleep(30)\n");
	struct agent_configuration cfg = make_cfg();

	struct tool_result r = tool_run_fn(
		"{\"path\":\"hang.py\",\"timeout_seconds\":2}", &cfg, g_scratch);
	ASSERT(r.content && strstr(r.content, "Timed out"));
	ASSERT(r.content && strstr(r.content, "before the hang"));
	tool_result_free(&r);
	configuration_destroy(&cfg);
}

static void test_memory_set_missing_key(void)
{
	printf("  Test: memory - set without key returns error\n");

	struct agent_configuration cfg = make_memory_cfg();
	struct tool_result r = tool_memory_fn(
		"{\"action\":\"set\",\"value\":\"no_key\"}", &cfg, g_scratch);

	ASSERT(r.is_error == true);

	tool_result_free(&r);
	configuration_destroy(&cfg);
}

/* -------------------------------------------------------------------------
 * tools_mask_to_string
 * ---------------------------------------------------------------------- */

/* A test tool whose output is its "text" argument repeated "times" times. */
static struct tool_result repeat_tool_fn(const char *arguments_json,
					 const struct agent_configuration *cfg,
					 struct memory_arena *scratch)
{
	(void)cfg;
	(void)scratch;
	cJSON *root = cJSON_Parse(arguments_json);
	const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
	const cJSON *times = cJSON_GetObjectItemCaseSensitive(root, "times");
	size_t unit = strlen(text->valuestring);
	size_t n = (size_t)times->valuedouble;
	char *out = malloc(unit * n + 1);
	for (size_t i = 0; i < n; i++)
		memcpy(out + i * unit, text->valuestring, unit);
	out[unit * n] = '\0';
	cJSON_Delete(root);
	return tool_result_ok("repeat", out);
}

#define TOOL_TEST_REPEAT (1u << 30)

static void test_dispatch_caps_results(void)
{
	printf("  Test: dispatch caps every result at max_result_bytes\n");

	struct tool_definition def;
	memset(&def, 0, sizeof(def));
	snprintf(def.name, sizeof(def.name), "repeat");
	def.parameters_schema = "{\"type\":\"object\",\"properties\":{}}";
	def.fn = repeat_tool_fn;
	def.flag = TOOL_TEST_REPEAT;
	tools_register(def); /* a second registration is refused; that's fine */

	struct agent_configuration cfg = make_cfg();
	cfg.tools_enabled = TOOL_TEST_REPEAT;
	cfg.max_result_bytes = 1000;

	struct tool_call call;
	memset(&call, 0, sizeof(call));
	snprintf(call.id, sizeof(call.id), "c1");
	snprintf(call.name, sizeof(call.name), "repeat");

	/* Under the cap: untouched */
	call.argument_json = "{\"text\":\"a\",\"times\":1000}";
	struct tool_result r = tools_dispatch(&call, &cfg);
	ASSERT(r.content && strlen(r.content) == 1000);
	ASSERT(r.content && !strstr(r.content, "truncated"));
	tool_result_free(&r);

	/* Over the cap: cut, with a note giving both sizes */
	call.argument_json = "{\"text\":\"a\",\"times\":5000}";
	r = tools_dispatch(&call, &cfg);
	ASSERT(r.content && strncmp(r.content, "aaaa", 4) == 0);
	ASSERT(r.content && strstr(r.content, "[Output truncated: showing 1000 of 5000 bytes."));
	ASSERT(r.content && strlen(r.content) < 1200);
	tool_result_free(&r);

	/* A 3-byte character straddling the cap is dropped whole */
	call.argument_json = "{\"text\":\"\u20ac\",\"times\":1000}"; /* euro sign */
	r = tools_dispatch(&call, &cfg);
	ASSERT(r.content && strstr(r.content, "showing 999 of 3000 bytes"));
	tool_result_free(&r);

	configuration_destroy(&cfg);
}

static void test_builtin_definitions(void)
{
	printf("  Test: built-in definitions - unique names and flags, TOOL_ALL complete\n");

	size_t count = 0;
	const struct tool_definition *const *defs = tools_builtins(&count);
	unsigned int seen = 0;
	for (size_t i = 0; i < count; i++) {
		const struct tool_definition *d = defs[i];
		ASSERT(d->name[0] != '\0');
		ASSERT(d->fn != NULL);
		ASSERT(d->format_call != NULL);
		ASSERT(d->description[0] != '\0');
		/* exactly one bit, not used by an earlier tool */
		ASSERT(d->flag != 0 && (d->flag & (d->flag - 1)) == 0);
		ASSERT((seen & d->flag) == 0);
		seen |= d->flag;
		ASSERT(tools_flag_from_name(d->name) == d->flag);
		for (size_t j = 0; j < i; j++)
			ASSERT(strcmp(defs[j]->name, d->name) != 0);

		/* the schema is a JSON object with properties */
		cJSON *schema = cJSON_Parse(d->parameters_schema);
		ASSERT(schema != NULL);
		ASSERT(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(schema, "properties")));
		cJSON_Delete(schema);
	}
	ASSERT(seen == TOOL_ALL);
}

static void test_mask_to_string_none(void)
{
	printf("  Test: tools_mask_to_string - TOOL_NONE -> \"none\"\n");

	char buf[64];
	tools_mask_to_string(TOOL_NONE, buf, sizeof(buf));
	ASSERT(strcmp(buf, "none") == 0);
}

static void test_mask_to_string_all(void)
{
	printf("  Test: tools_mask_to_string - TOOL_ALL -> \"all\"\n");

	char buf[64];
	tools_mask_to_string(TOOL_ALL, buf, sizeof(buf));
	ASSERT(strcmp(buf, "all") == 0);
}

static void test_mask_to_string_subset(void)
{
	printf("  Test: tools_mask_to_string - subset lists names\n");

	char buf[128];
	tools_mask_to_string(TOOL_READ | TOOL_WRITE, buf, sizeof(buf));
	ASSERT(strstr(buf, "read") != NULL);
	ASSERT(strstr(buf, "write") != NULL);
	ASSERT(strstr(buf, "bash") == NULL);
	ASSERT(strstr(buf, "web") == NULL);
}

static void test_mask_to_string_single(void)
{
	printf("  Test: tools_mask_to_string - single flag\n");

	char buf[64];
	tools_mask_to_string(TOOL_MEMORY, buf, sizeof(buf));
	ASSERT(strcmp(buf, "memory") == 0);
}

/* -------------------------------------------------------------------------
 * Run tool
 * ---------------------------------------------------------------------- */

static void test_run_output_truncation(void)
{
	printf("  Test: run - output truncated at 65536 bytes\n");

	char script_path[512];
	snprintf(script_path, sizeof(script_path), "%s/big_output.sh", g_sandbox);
	FILE *f = fopen(script_path, "w");
	ASSERT(f != NULL);
	if (!f)
		return;
	/* awk outputs 70000 'A' chars - exceeds the 65536-byte cap */
	fputs("#!/bin/sh\n"
	      "awk 'BEGIN{for(i=0;i<70000;i++) printf \"A\"; print \"\"}'\n",
	      f);
	fclose(f);
	chmod(script_path, 0755);

	struct agent_configuration cfg = make_cfg();
	cfg.run_timeout_seconds = 10;
	cfg.tools_enabled = TOOL_RUN;

	struct tool_result r = tool_run_fn(
		"{\"path\":\"big_output.sh\"}", &cfg, g_scratch);

	ASSERT(r.is_error == false);
	ASSERT(r.content != NULL);
	if (r.content)
		ASSERT(strstr(r.content, "truncated") != NULL ||
		       strstr(r.content, "Truncated") != NULL);

	tool_result_free(&r);
	configuration_destroy(&cfg);
	unlink(script_path);
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

int main(void)
{
	printf("\n=== Tool Tests ===\n\n");

	setup();

	printf("--- Read ---\n");
	test_read_basic();
	test_read_not_found();
	test_read_is_directory();
	test_read_traversal_blocked();
	test_read_no_sandbox();
	test_read_offset_length();
	test_read_binary_blocked();
	test_read_truncation_note();

	printf("\n--- Write ---\n");
	test_write_overwrite();
	test_write_append();
	test_write_create_new_success();
	test_write_create_new_fails_if_exists();
	test_write_create_dirs();
	test_write_traversal_blocked();
	test_write_create_dirs_escape_blocked();
	test_write_large_content_via_dispatch();

	printf("\n--- List Dir ---\n");
	test_list_dir_basic();
	test_list_dir_hidden_hidden_off();
	test_list_dir_hidden_hidden_on();
	test_list_dir_not_a_dir();
	test_list_dir_not_found();

	printf("\n--- Memory ---\n");
	test_memory_set_get();
	test_memory_get_missing();
	test_memory_delete();
	test_memory_list();
	test_memory_unknown_action();
	test_memory_no_persist();
	test_memory_set_missing_key();
	test_memory_keys_case_sensitive();
	test_memory_empty_file();
	test_confirm_writes();
	test_format_call_summaries();
	test_term_sanitize();
	test_observer_sees_every_outcome();
	test_write_preserves_mode_and_validates();
	test_run_script_ignores_stdin();
	test_run_environment_allowlist();
	test_run_timeout_after_stdout_closed();
	test_run_timeout_keeps_python_output();

	printf("\n--- Run ---\n");
	test_run_output_truncation();

	printf("\n--- Tools Mask ---\n");
	test_builtin_definitions();
	test_dispatch_caps_results();
	test_mask_to_string_none();
	test_mask_to_string_all();
	test_mask_to_string_subset();
	test_mask_to_string_single();

	teardown();

	printf("\n--- Tool Results ---\n");
	printf("  Passed: %d\n", tests_passed);
	printf("  Failed: %d\n", tests_failed);
	printf("\n");

	return tests_failed > 0 ? 1 : 0;
}
