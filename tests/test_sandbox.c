/*
 * test_sandbox.c - Tests for path sandboxing.
 *
 * Exercises sandbox_resolve() and fs_mkdirp()
 * with real filesystem operations against a temporary sandbox directory.
 */

#define _POSIX_C_SOURCE 200809L

#include "sandbox.h"
#include "fs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "support.h"
#include <ctype.h>
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
		 "%s/boris_sandbox_test_%d", test_tmp_dir(), (int)getpid());
	mkdir(g_sandbox, 0755);
}

static void teardown_sandbox(void)
{
	test_remove_tree(g_sandbox);
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
	int rc = sandbox_resolve(NULL, test_tmp_dir(), out, sizeof(out));
	ASSERT(rc == 0);
	ASSERT(strcmp(out, test_tmp_dir()) == 0);
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

/* Test: ".." after a non-existent component is rejected (it would escape
 * once the missing directories were created) */
static void test_resolve_dotdot_in_missing_tail_blocked(void)
{
	printf("  Test: .. after a non-existent component is blocked\n");

	char out[PATH_MAX];
	ASSERT(sandbox_resolve(g_sandbox, "missing/../../escaped.txt",
			       out, sizeof(out)) != 0);
#ifdef _WIN32
	/* Win32 removes ".." from the string before the filesystem sees it, so
	 * nothing is left to be followed later: this is simply inside.txt. */
	ASSERT(sandbox_resolve(g_sandbox, "missing/../inside.txt",
			       out, sizeof(out)) == 0);
	ASSERT(strstr(out, "..") == NULL && strstr(out, "missing") == NULL);
#else
	ASSERT(sandbox_resolve(g_sandbox, "missing/../inside.txt",
			       out, sizeof(out)) != 0);
#endif
}

/* Test: a link inside the sandbox that points out of it is not followed */
static void test_resolve_link_escape_blocked(void)
{
	printf("  Test: a directory link out of the sandbox is blocked\n");

	char outside[512], link_path[512];
	snprintf(outside, sizeof(outside), "%s_outside", g_sandbox);
	snprintf(link_path, sizeof(link_path), "%s/way_out", g_sandbox);
	mkdir(outside, 0755);
	if (test_link(outside, link_path) != 0) {
		printf("  (skipped: cannot create links here)\n");
		rmdir(outside);
		return;
	}

	char out[PATH_MAX];
	ASSERT(sandbox_resolve(g_sandbox, "way_out", out, sizeof(out)) != 0);
	ASSERT(sandbox_resolve(g_sandbox, "way_out/file.txt", out, sizeof(out)) != 0);
	ASSERT(sandbox_resolve(g_sandbox, "way_out/new/file.txt", out, sizeof(out)) != 0);

	unlink(link_path);
	rmdir(outside);
}

#ifdef _WIN32
/* Test: the spellings of a path that only Windows has */
static void test_resolve_windows_paths(void)
{
	printf("  Test: Windows - backslashes, drive letters, letter case\n");

	char file_path[512], out[PATH_MAX], arg[600];
	snprintf(file_path, sizeof(file_path), "%s/sub", g_sandbox);
	mkdir(file_path, 0755);
	snprintf(file_path, sizeof(file_path), "%s/sub/Hello.txt", g_sandbox);
	FILE *f = fopen(file_path, "w");
	if (f)
		fclose(f);

	/* Backslashes are separators, and the result uses forward slashes */
	ASSERT(sandbox_resolve(g_sandbox, "sub\\Hello.txt", out, sizeof(out)) == 0);
	ASSERT(strcmp(out, file_path) == 0);

	/* ...so they traverse like forward slashes do */
	ASSERT(sandbox_resolve(g_sandbox, "..\\..\\Windows\\win.ini", out, sizeof(out)) != 0);
	ASSERT(sandbox_resolve(g_sandbox, "sub\\..\\..\\x.txt", out, sizeof(out)) != 0);

	/* The name on disk wins, whatever case was asked for */
	ASSERT(sandbox_resolve(g_sandbox, "SUB/hello.TXT", out, sizeof(out)) == 0);
	ASSERT(strcmp(out, file_path) == 0);

	/* An absolute path inside the sandbox, written the native way */
	snprintf(arg, sizeof(arg), "%s", file_path);
	for (char *p = arg; *p; p++)
		if (*p == '/')
			*p = '\\';
	ASSERT(sandbox_resolve(g_sandbox, arg, out, sizeof(out)) == 0);
	ASSERT(strcmp(out, file_path) == 0);

	/* Absolute paths elsewhere: another directory, the drive root, a share */
	ASSERT(sandbox_resolve(g_sandbox, "C:\\Windows\\win.ini", out, sizeof(out)) != 0);
	ASSERT(sandbox_resolve(g_sandbox, "C:/Windows/win.ini", out, sizeof(out)) != 0);
	ASSERT(sandbox_resolve(g_sandbox, "\\Windows\\win.ini", out, sizeof(out)) != 0);
	ASSERT(sandbox_resolve(g_sandbox, "C:\\", out, sizeof(out)) != 0);
	ASSERT(sandbox_resolve(g_sandbox, "\\\\localhost\\C$\\Windows\\win.ini", out,
			       sizeof(out)) != 0);

	/* A sandbox root given with backslashes or in another case is the same root */
	snprintf(arg, sizeof(arg), "%s", g_sandbox);
	for (char *p = arg; *p; p++)
		*p = *p == '/' ? '\\' : (char)toupper((unsigned char)*p);
	ASSERT(sandbox_resolve(arg, "sub/Hello.txt", out, sizeof(out)) == 0);
	ASSERT(strcmp(out, file_path) == 0);

	unlink(file_path);
}

static void test_resolve_windows_special_names(void)
{
	printf("  Test: Windows - devices, streams and names Win32 rewrites\n");

	static const char *const refused[] = {
		"NUL", "nul", "CON", "con.txt", "sub/COM1", "LPT9.log", "aux", "PRN",
		"CONIN$", "conout$", "nul .txt",    /* device names, in any directory */
		"notes.txt:secret", "notes.txt::$DATA", /* alternate data streams */
		"C:notes.txt", "D:",			    /* drive-relative */
		"//./C:/Windows/win.ini", "\\\\?\\C:\\Windows\\win.ini",
		"\\\\.\\NUL",			    /* the device namespace */
		"new/.. /.. /escape.txt", "new/.../x",	    /* ".. " is ".." to Win32 */
		"trailing./x.txt", "trailing /x.txt", "name.", "name ",
	};
	char out[PATH_MAX];
	for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
		int rc = sandbox_resolve(g_sandbox, refused[i], out, sizeof(out));
		if (rc == 0)
			printf("    accepted: '%s' -> '%s'\n", refused[i], out);
		ASSERT(rc != 0);
	}

	/* Names that merely look similar are ordinary files */
	static const char *const allowed[] = {
		"null.txt", "console", "com10", "COM", "lpt", "nul2/x", ".hidden",
		"..data", "a.b.c", "with space.txt",
	};
	for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
		int rc = sandbox_resolve(g_sandbox, allowed[i], out, sizeof(out));
		if (rc != 0)
			printf("    refused: '%s'\n", allowed[i]);
		ASSERT(rc == 0);
	}
}
#endif

/* Test: fs_mkdirp creates a nested directory tree */
static void test_mkdirp_nested(void)
{
	printf("  Test: fs_mkdirp creates nested directories\n");

	char nested[512];
	snprintf(nested, sizeof(nested), "%s/a/b/c", g_sandbox);

	int rc = fs_mkdirp(nested, 0755);
	ASSERT(rc == 0);

	struct stat st;
	ASSERT(stat(nested, &st) == 0);
	ASSERT(S_ISDIR(st.st_mode));
}

/* Test: fs_mkdirp on existing path is idempotent */
static void test_mkdirp_idempotent(void)
{
	printf("  Test: fs_mkdirp on existing path is idempotent\n");

	char dir[512];
	snprintf(dir, sizeof(dir), "%s/existing", g_sandbox);
	mkdir(dir, 0755);

	int rc = fs_mkdirp(dir, 0755);
	ASSERT(rc == 0);
}

/* Test: fs_mkdirp with NULL returns error */
static void test_mkdirp_null(void)
{
	printf("  Test: fs_mkdirp NULL returns error\n");

	int rc = fs_mkdirp(NULL, 0755);
	ASSERT(rc != 0);
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
	test_resolve_dotdot_in_missing_tail_blocked();
	test_resolve_link_escape_blocked();
#ifdef _WIN32
	test_resolve_windows_paths();
	test_resolve_windows_special_names();
#endif
	test_mkdirp_nested();
	test_mkdirp_idempotent();
	test_mkdirp_null();

	teardown_sandbox();

	printf("\n--- Sandbox Results ---\n");
	printf("  Passed: %d\n", tests_passed);
	printf("  Failed: %d\n", tests_failed);
	printf("\n");

	return tests_failed > 0 ? 1 : 0;
}
