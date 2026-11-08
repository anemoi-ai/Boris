/*
 * test_instructions.c - Per-project instructions (BORIS.md / AGENTS.md).
 */

#define _POSIX_C_SOURCE 200809L

#include "instructions.h"
#include "configuration.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

static char g_root[128];
static char g_sandbox[256];
static char g_outside[256];

static void put(const char *dir, const char *name, const char *text)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *f = fopen(path, "w");
	if (f) {
		fputs(text, f);
		fclose(f);
	}
}

static void clear(const char *name)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", g_sandbox, name);
	unlink(path);
	rmdir(path);
}

static struct agent_configuration make_cfg(void)
{
	struct agent_configuration cfg;
	configuration_set_defaults(&cfg);
	cfg.sandbox_root = strdup(g_sandbox);
	return cfg;
}

static void test_none_found(void)
{
	printf("  Test: no file -> no instructions\n");
	struct agent_configuration cfg = make_cfg();
	char src[256];
	char *t = instructions_load(&cfg, src, sizeof(src));
	ASSERT(t == NULL && src[0] == '\0');
	configuration_destroy(&cfg);
}

static void test_agents_md_and_precedence(void)
{
	printf("  Test: AGENTS.md is found; BORIS.md wins when both exist\n");
	struct agent_configuration cfg = make_cfg();
	char src[256];
	put(g_sandbox, "AGENTS.md", "Use tabs.\n");
	char *t = instructions_load(&cfg, src, sizeof(src));
	ASSERT(t && strcmp(t, "Use tabs.\n") == 0 && strcmp(src, "AGENTS.md") == 0);
	free(t);

	put(g_sandbox, "BORIS.md", "Use spaces.\n");
	t = instructions_load(&cfg, src, sizeof(src));
	ASSERT(t && strcmp(t, "Use spaces.\n") == 0 && strcmp(src, "BORIS.md") == 0);
	free(t);
	clear("AGENTS.md");
	clear("BORIS.md");
	configuration_destroy(&cfg);
}

static void test_symlink_out_of_sandbox_refused(void)
{
	printf("  Test: a BORIS.md symlink to a file outside the sandbox is refused\n");
	put(g_outside, "private.txt", "SECRET-VALUE\n");
	char target[PATH_MAX], link_path[PATH_MAX];
	snprintf(target, sizeof(target), "%s/private.txt", g_outside);
	snprintf(link_path, sizeof(link_path), "%s/BORIS.md", g_sandbox);
	symlink(target, link_path);
	put(g_sandbox, "AGENTS.md", "Fallback rules.\n");

	struct agent_configuration cfg = make_cfg();
	char src[256];
	char *t = instructions_load(&cfg, src, sizeof(src));
	ASSERT(t && !strstr(t, "SECRET-VALUE"));
	ASSERT(t && strcmp(src, "AGENTS.md") == 0); /* falls back to the next name */
	free(t);
	clear("BORIS.md");
	clear("AGENTS.md");
	configuration_destroy(&cfg);
}

static void test_symlink_inside_sandbox_ok(void)
{
	printf("  Test: a BORIS.md symlink to a file inside the sandbox is fine\n");
	put(g_sandbox, "docs_rules.md", "Inside rules.\n");
	char link_path[PATH_MAX];
	snprintf(link_path, sizeof(link_path), "%s/BORIS.md", g_sandbox);
	symlink("docs_rules.md", link_path);
	struct agent_configuration cfg = make_cfg();
	char src[256];
	char *t = instructions_load(&cfg, src, sizeof(src));
	ASSERT(t && strcmp(t, "Inside rules.\n") == 0);
	free(t);
	clear("BORIS.md");
	clear("docs_rules.md");
	configuration_destroy(&cfg);
}

static void test_not_a_file_empty_or_binary(void)
{
	printf("  Test: a directory, an empty file and a binary file are ignored\n");
	struct agent_configuration cfg = make_cfg();
	char src[256], path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/BORIS.md", g_sandbox);
	mkdir(path, 0755);
	ASSERT(instructions_load(&cfg, src, sizeof(src)) == NULL);
	clear("BORIS.md");

	put(g_sandbox, "BORIS.md", "  \n\t\n");
	ASSERT(instructions_load(&cfg, src, sizeof(src)) == NULL);

	FILE *f = fopen(path, "wb");
	if (f) {
		fwrite("ab\0cd", 1, 5, f);
		fclose(f);
	}
	ASSERT(instructions_load(&cfg, src, sizeof(src)) == NULL);
	clear("BORIS.md");
	configuration_destroy(&cfg);
}

static void test_large_file_truncated(void)
{
	printf("  Test: a file over 32 KB is cut, with a note\n");
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/BORIS.md", g_sandbox);
	FILE *f = fopen(path, "w");
	for (int i = 0; f && i < 50000; i++)
		fputc('a' + i % 26, f);
	if (f)
		fclose(f);
	struct agent_configuration cfg = make_cfg();
	char src[256];
	char *t = instructions_load(&cfg, src, sizeof(src));
	ASSERT(t && strlen(t) < INSTRUCTIONS_MAX_BYTES + 200);
	ASSERT(t && strstr(t, "longer than 32 KB"));
	free(t);
	clear("BORIS.md");
	configuration_destroy(&cfg);
}

static void test_none_and_explicit_path(void)
{
	printf("  Test: 'none' disables; an explicit path is used wherever it is\n");
	put(g_sandbox, "BORIS.md", "Sandbox rules.\n");
	put(g_outside, "team-rules.md", "Team rules.\n");
	struct agent_configuration cfg = make_cfg();
	char src[256];

	configuration_set_key(&cfg, "prompt.instructions", "none");
	ASSERT(instructions_load(&cfg, src, sizeof(src)) == NULL);

	char explicit[PATH_MAX];
	snprintf(explicit, sizeof(explicit), "%s/team-rules.md", g_outside);
	configuration_set_key(&cfg, "prompt.instructions", explicit);
	char *t = instructions_load(&cfg, src, sizeof(src));
	ASSERT(t && strcmp(t, "Team rules.\n") == 0 && strcmp(src, explicit) == 0);
	free(t);

	configuration_set_key(&cfg, "prompt.instructions", "auto");
	t = instructions_load(&cfg, src, sizeof(src));
	ASSERT(t && strcmp(t, "Sandbox rules.\n") == 0);
	free(t);
	clear("BORIS.md");
	configuration_destroy(&cfg);
}

static void test_compose(void)
{
	printf("  Test: the instructions follow the system prompt under a heading\n");
	char *p = instructions_compose("You are Boris.", "Use tabs.", "BORIS.md");
	ASSERT(p && strncmp(p, "You are Boris.\n\n# Project instructions (from BORIS.md)", 54) == 0);
	ASSERT(p && strstr(p, "in BORIS.md. Follow them unless"));
	ASSERT(p && strlen(p) > 9 && strcmp(p + strlen(p) - 9, "Use tabs.") == 0);
	free(p);

	p = instructions_compose("Base.", NULL, NULL);
	ASSERT(p && strcmp(p, "Base.") == 0);
	free(p);
}

int main(void)
{
	printf("\n=== Instructions Tests ===\n\n");
	snprintf(g_root, sizeof(g_root), "/tmp/boris_instructions_test_%d", (int)getpid());
	snprintf(g_sandbox, sizeof(g_sandbox), "%s/sandbox", g_root);
	snprintf(g_outside, sizeof(g_outside), "%s/outside", g_root);
	mkdir(g_root, 0755);
	mkdir(g_sandbox, 0755);
	mkdir(g_outside, 0755);

	test_none_found();
	test_agents_md_and_precedence();
	test_symlink_out_of_sandbox_refused();
	test_symlink_inside_sandbox_ok();
	test_not_a_file_empty_or_binary();
	test_large_file_truncated();
	test_none_and_explicit_path();
	test_compose();

	char cmd[PATH_MAX];
	snprintf(cmd, sizeof(cmd), "rm -rf %s", g_root);
	if (system(cmd) != 0)
		printf("  (could not remove %s)\n", g_root);

	printf("\n--- Instructions Results ---\n");
	printf("  Passed: %d\n", tests_passed);
	printf("  Failed: %d\n", tests_failed);
	printf("\n");
	return tests_failed > 0 ? 1 : 0;
}
