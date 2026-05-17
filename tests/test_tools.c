/*
 * test_tools.c - Unit tests for the built-in tool implementations.
 *
 * Tests each tool function (read, write, list_dir, memory) directly,
 * plus tools_mask_to_string(). Uses a real temporary sandbox directory
 * and controls the memory tool's storage path via XDG_DATA_HOME.
 */

#define _POSIX_C_SOURCE 200809L

#include "tools.h"
#include "arena.h"
#include "configuration.h"
#include "boris_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

/* Forward declarations — tool implementations are not exposed in tools.h */
struct tool_result tool_read_fn(const char *arguments_json,
				const struct agent_configuration *cfg,
				struct memory_arena *scratch);
struct tool_result tool_write_fn(const char *arguments_json,
				 const struct agent_configuration *cfg,
				 struct memory_arena *scratch);
struct tool_result tool_list_dir_fn(const char *arguments_json,
				    const struct agent_configuration *cfg,
				    struct memory_arena *scratch);
struct tool_result tool_memory_fn(const char *arguments_json,
				  const struct agent_configuration *cfg,
				  struct memory_arena *scratch);
struct tool_result tool_run_fn(const char *arguments_json,
			       const struct agent_configuration *cfg,
			       struct memory_arena *scratch);

static int tests_passed = 0;
static int tests_failed = 0;

/* Shared per-suite scratch arena — matches what tools_dispatch provides */
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
static char g_mem_xdg[256];

static void setup(void)
{
	snprintf(g_sandbox, sizeof(g_sandbox),
		 "/tmp/boris_tools_test_%d", (int)getpid());
	mkdir(g_sandbox, 0755);

	/* Redirect memory tool storage to a controlled temp dir */
	snprintf(g_mem_xdg, sizeof(g_mem_xdg),
		 "/tmp/boris_mem_xdg_%d", (int)getpid());
	mkdir(g_mem_xdg, 0755);
	setenv("XDG_DATA_HOME", g_mem_xdg, 1);

	g_scratch = arena_create(32768);
}

static void teardown(void)
{
	char cmd[PATH_MAX * 2 + 16];
	snprintf(cmd, sizeof(cmd), "rm -rf %s %s", g_sandbox, g_mem_xdg);
	system(cmd);
	unsetenv("XDG_DATA_HOME");
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

	/* Remove all path env vars so memory can't resolve its file */
	char *saved_home = getenv("HOME");
	char saved_home_buf[1024] = "";
	if (saved_home)
		snprintf(saved_home_buf, sizeof(saved_home_buf), "%s", saved_home);

	unsetenv("HOME");
	unsetenv("XDG_DATA_HOME");

	struct agent_configuration cfg;
	configuration_set_defaults(&cfg);
	cfg.memory_persist = false;

	struct tool_result r = tool_memory_fn(
		"{\"action\":\"get\",\"key\":\"foo\"}", &cfg, g_scratch);
	ASSERT(r.is_error == true);

	tool_result_free(&r);
	configuration_destroy(&cfg);

	/* Restore env */
	if (saved_home_buf[0])
		setenv("HOME", saved_home_buf, 1);
	setenv("XDG_DATA_HOME", g_mem_xdg, 1);
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
	/* awk outputs 70000 'A' chars — exceeds the 65536-byte cap */
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

	printf("\n--- Run ---\n");
	test_run_output_truncation();

	printf("\n--- Tools Mask ---\n");
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
