/*
 * test_sandbox.c - Tests for path sandboxing.
 *
 * Exercises sandbox_resolve(), sandbox_mkdirp(), and sandbox_contains()
 * with real filesystem operations against a temporary sandbox directory.
 */

#define _POSIX_C_SOURCE 200809L

#include "sandbox.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

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

static char g_sandbox[256];

static void setup_sandbox(void)
{
	snprintf(g_sandbox, sizeof(g_sandbox),
		 "/tmp/boris_sandbox_test_%d", (int)getpid());
	mkdir(g_sandbox, 0755);
}

static void teardown_sandbox(void)
{
	char cmd[PATH_MAX + 16];
	snprintf(cmd, sizeof(cmd), "rm -rf %s", g_sandbox);
	system(cmd);
}

/* Test: relative path resolves inside sandbox */
static void test_resolve_relative_inside(void)
{
	printf("  Test: relative path resolves inside sandbox\n");

	char file_path[512];
	snprintf(file_path, sizeof(file_path), "%s/hello.txt", g_sandbox);
	FILE *f = fopen(file_path, "w");
	if (f) {
		fprintf(f, "hello");
		fclose(f);
	}

	char out[PATH_MAX];
	int rc = sandbox_resolve(g_sandbox, "hello.txt", out, sizeof(out));
	ASSERT(rc == 0);
	ASSERT(strncmp(out, g_sandbox, strlen(g_sandbox)) == 0);
	ASSERT(strstr(out, "hello.txt") != NULL);

	unlink(file_path);
}

/* Test: relative "." resolves to sandbox root and is allowed */
static void test_resolve_dot_allowed(void)
{
	printf("  Test: relative '.' resolves to sandbox root (allowed)\n");

	char out[PATH_MAX];
	int rc = sandbox_resolve(g_sandbox, ".", out, sizeof(out));
	ASSERT(rc == 0);
}

/* Test: "../" path traversal is blocked */
static void test_resolve_path_traversal_blocked(void)
{
	printf("  Test: path traversal with ../ is blocked\n");

	char out[PATH_MAX];
	int rc = sandbox_resolve(g_sandbox, "../../etc/passwd", out, sizeof(out));
	ASSERT(rc != 0);
}

/* Test: absolute path inside sandbox is allowed */
static void test_resolve_absolute_inside(void)
{
	printf("  Test: absolute path inside sandbox is allowed\n");

	char target[512];
	snprintf(target, sizeof(target), "%s/file.txt", g_sandbox);
	FILE *f = fopen(target, "w");
	if (f) {
		fprintf(f, "data");
		fclose(f);
	}

	char out[PATH_MAX];
	int rc = sandbox_resolve(g_sandbox, target, out, sizeof(out));
	ASSERT(rc == 0);
	ASSERT(strcmp(out, target) == 0);

	unlink(target);
}

/* Test: absolute path outside sandbox is blocked */
static void test_resolve_absolute_outside_blocked(void)
{
	printf("  Test: absolute path outside sandbox is blocked\n");

	char out[PATH_MAX];
	int rc = sandbox_resolve(g_sandbox, "/etc/passwd", out, sizeof(out));
	ASSERT(rc != 0);
}

/* Test: absolute path equal to sandbox root is blocked */
static void test_resolve_absolute_root_blocked(void)
{
	printf("  Test: absolute path == sandbox root is blocked\n");

	char out[PATH_MAX];
	int rc = sandbox_resolve(g_sandbox, g_sandbox, out, sizeof(out));
	ASSERT(rc != 0);
}

/* Test: non-existent path inside sandbox resolves OK (needed by write tool) */
static void test_resolve_nonexistent_inside(void)
{
	printf("  Test: non-existent path inside sandbox resolves OK\n");

	char out[PATH_MAX];
	int rc = sandbox_resolve(g_sandbox, "new_dir/new_file.txt",
				 out, sizeof(out));
	ASSERT(rc == 0);
	ASSERT(strncmp(out, g_sandbox, strlen(g_sandbox)) == 0);
}

/* Test: empty path is rejected */
static void test_resolve_empty_path_rejected(void)
{
	printf("  Test: empty path is rejected\n");

	char out[PATH_MAX];
	int rc = sandbox_resolve(g_sandbox, "", out, sizeof(out));
	ASSERT(rc != 0);
}

/* Test: NULL sandbox root disables sandboxing and still resolves */
static void test_resolve_no_sandbox(void)
{
	printf("  Test: NULL sandbox root resolves without restriction\n");

	char out[PATH_MAX];
	int rc = sandbox_resolve(NULL, "/tmp", out, sizeof(out));
	ASSERT(rc == 0);
	ASSERT(strcmp(out, "/tmp") == 0);
}

/* Test: deeply nested relative path with ".." components that stay inside */
static void test_resolve_dotdot_stays_inside(void)
{
	printf("  Test: relative path with .. that stays inside sandbox\n");

	char subdir[512];
	snprintf(subdir, sizeof(subdir), "%s/sub", g_sandbox);
	mkdir(subdir, 0755);

	char out[PATH_MAX];
	int rc = sandbox_resolve(g_sandbox, "sub/../sub", out, sizeof(out));
	ASSERT(rc == 0);
	ASSERT(strncmp(out, g_sandbox, strlen(g_sandbox)) == 0);
}

/* Test: sandbox_mkdirp creates a nested directory tree */
static void test_mkdirp_nested(void)
{
	printf("  Test: sandbox_mkdirp creates nested directories\n");

	char nested[512];
	snprintf(nested, sizeof(nested), "%s/a/b/c", g_sandbox);

	int rc = sandbox_mkdirp(nested, 0755);
	ASSERT(rc == 0);

	struct stat st;
	ASSERT(stat(nested, &st) == 0);
	ASSERT(S_ISDIR(st.st_mode));
}

/* Test: sandbox_mkdirp on existing path is idempotent */
static void test_mkdirp_idempotent(void)
{
	printf("  Test: sandbox_mkdirp on existing path is idempotent\n");

	char dir[512];
	snprintf(dir, sizeof(dir), "%s/existing", g_sandbox);
	mkdir(dir, 0755);

	int rc = sandbox_mkdirp(dir, 0755);
	ASSERT(rc == 0);
}

/* Test: sandbox_mkdirp with NULL returns error */
static void test_mkdirp_null(void)
{
	printf("  Test: sandbox_mkdirp NULL returns error\n");

	int rc = sandbox_mkdirp(NULL, 0755);
	ASSERT(rc != 0);
}

/* Test: sandbox_contains returns 1 for a path inside the sandbox */
static void test_contains_true(void)
{
	printf("  Test: sandbox_contains true for path inside\n");

	char inside[512];
	snprintf(inside, sizeof(inside), "%s/some/deep/path", g_sandbox);

	ASSERT(sandbox_contains(g_sandbox, inside) == 1);
}

/* Test: sandbox_contains returns 0 for a path outside the sandbox */
static void test_contains_false(void)
{
	printf("  Test: sandbox_contains false for path outside\n");

	ASSERT(sandbox_contains(g_sandbox, "/etc/passwd") == 0);
}

/* Test: sandbox_contains returns 0 if path equals root (no trailing slash) */
static void test_contains_root_itself(void)
{
	printf("  Test: sandbox_contains false when path equals root\n");

	ASSERT(sandbox_contains(g_sandbox, g_sandbox) == 0);
}

/* Test: sandbox_contains with NULL arguments is safe */
static void test_contains_null(void)
{
	printf("  Test: sandbox_contains NULL arguments are safe\n");

	ASSERT(sandbox_contains(NULL, "/tmp/foo") == 0);
	ASSERT(sandbox_contains(g_sandbox, NULL) == 0);
}

int main(void)
{
	printf("\n=== Sandbox Tests ===\n\n");

	setup_sandbox();

	test_resolve_relative_inside();
	test_resolve_dot_allowed();
	test_resolve_path_traversal_blocked();
	test_resolve_absolute_inside();
	test_resolve_absolute_outside_blocked();
	test_resolve_absolute_root_blocked();
	test_resolve_nonexistent_inside();
	test_resolve_empty_path_rejected();
	test_resolve_no_sandbox();
	test_resolve_dotdot_stays_inside();
	test_mkdirp_nested();
	test_mkdirp_idempotent();
	test_mkdirp_null();
	test_contains_true();
	test_contains_false();
	test_contains_root_itself();
	test_contains_null();

	teardown_sandbox();

	printf("\n--- Sandbox Results ---\n");
	printf("  Passed: %d\n", tests_passed);
	printf("  Failed: %d\n", tests_failed);
	printf("\n");

	return tests_failed > 0 ? 1 : 0;
}
