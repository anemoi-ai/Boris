/*
 * test_configuration.c - Testing how Boris learns who to be.
 *
 * We test the configuration system by:
 *   1. Checking that defaults are sensible
 *   2. Loading a config file and verifying values change
 *   3. Applying environment variables
 *   4. Making sure cleanup works properly
 */

#define _POSIX_C_SOURCE 200809L

#include "configuration.h"
#include "boris_errors.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

/*
 * Test: Default configuration has sensible values.
 */
static void test_configuration_defaults(void)
{
	printf("  Test: default values\n");

	struct agent_configuration config;
	configuration_set_defaults(&config);

	/* Pointers should be NULL */
	ASSERT(config.model_endpoint == NULL);
	ASSERT(config.model_name == NULL);
	ASSERT(config.api_key == NULL);
	ASSERT(config.system_prompt == NULL);
	ASSERT(config.system_prompt_file == NULL);
	ASSERT(config.sandbox_root == NULL);
	ASSERT(config.log_file == NULL);

	/* Numbers should have sensible defaults */
	ASSERT(config.temperature == 0.7f);
	ASSERT(config.max_tokens == 2048);
	ASSERT(config.max_conversation_turns == 32);
	ASSERT(config.request_timeout_seconds == 120);
	ASSERT(config.log_level == 1); /* INFO */
	ASSERT(config.stream_responses == false);
	ASSERT(config.confirm_writes == false);
	ASSERT(config.json_output == false);

	configuration_destroy(&config);
}

/*
 * Test: Loading from a config file changes values.
 */
static void test_configuration_load_from_file(void)
{
	printf("  Test: load from file\n");

	/* Create a temporary config file */
	const char *test_file = "/tmp/boris_config_test.ini";
	FILE *file = fopen(test_file, "w");
	ASSERT(file != NULL);

	if (file) {
		fprintf(file, "[model]\n");
		fprintf(file, "endpoint = http://localhost:11434/v1/chat/completions\n");
		fprintf(file, "name = llama3\n");
		fprintf(file, "temperature = 0.5\n");
		fprintf(file, "max_tokens = 4096\n");
		fprintf(file, "\n");
		fprintf(file, "[logging]\n");
		fprintf(file, "level = debug\n");
		fclose(file);
	}

	struct agent_configuration config;
	configuration_set_defaults(&config);

	enum boris_error result = configuration_load_from_file(&config, test_file);
	ASSERT(result == BORIS_OK);

	/* Check that values were loaded */
	ASSERT(config.model_endpoint != NULL);
	ASSERT(strcmp(config.model_endpoint, "http://localhost:11434/v1/chat/completions") == 0);
	ASSERT(config.model_name != NULL);
	ASSERT(strcmp(config.model_name, "llama3") == 0);
	ASSERT(config.temperature == 0.5f);
	ASSERT(config.max_tokens == 4096);
	ASSERT(config.log_level == 0); /* debug */

	configuration_destroy(&config);

	/* Clean up */
	remove(test_file);
}

/*
 * Test: Loading from a non-existent file is OK (uses defaults).
 */
static void test_configuration_missing_file(void)
{
	printf("  Test: missing config file\n");

	struct agent_configuration config;
	configuration_set_defaults(&config);

	enum boris_error result = configuration_load_from_file(&config, "/tmp/boris_does_not_exist.ini");
	ASSERT(result == BORIS_OK); /* Missing file is not an error */

	/* Values should still be defaults */
	ASSERT(config.model_endpoint == NULL);
	ASSERT(config.temperature == 0.7f);

	configuration_destroy(&config);
}

/*
 * Test: Environment variables override config.
 */
static void test_configuration_environment(void)
{
	printf("  Test: environment variables\n");

	/* Set environment variables */
	setenv("BORIS_MODEL_ENDPOINT", "http://env-test:8080", 1);
	setenv("BORIS_MODEL_NAME", "env-model", 1);
	setenv("BORIS_LOG_LEVEL", "error", 1);

	struct agent_configuration config;
	configuration_set_defaults(&config);

	configuration_apply_environment(&config);

	ASSERT(config.model_endpoint != NULL);
	ASSERT(strcmp(config.model_endpoint, "http://env-test:8080") == 0);
	ASSERT(config.model_name != NULL);
	ASSERT(strcmp(config.model_name, "env-model") == 0);
	ASSERT(config.log_level == 3); /* error */

	configuration_destroy(&config);

	/* Clean up environment */
	unsetenv("BORIS_MODEL_ENDPOINT");
	unsetenv("BORIS_MODEL_NAME");
	unsetenv("BORIS_LOG_LEVEL");
}

/*
 * Test: Configuration cleanup frees all memory.
 */
static void test_configuration_destroy(void)
{
	printf("  Test: destroy frees memory\n");

	struct agent_configuration config;
	configuration_set_defaults(&config);

	/* Set some values */
	config.model_endpoint = strdup("http://test:8080");
	config.model_name = strdup("test-model");

	configuration_destroy(&config);

	/* After destroy, pointers should be NULL */
	ASSERT(config.model_endpoint == NULL);
	ASSERT(config.model_name == NULL);

	/* Destroying again should be safe (no double-free) */
	configuration_destroy(&config);

	tests_passed++; /* If we got here, no crash */
}

/*
 * Test: Destroying a NULL config is safe.
 */
static void test_configuration_destroy_null(void)
{
	printf("  Test: destroy NULL config\n");

	configuration_destroy(NULL);
	tests_passed++; /* If we got here, it didn't crash */
}

/*
 * Test: configuration_parse_tools_mask converts token strings to bitmasks.
 */
static void test_parse_tools_mask(void)
{
	printf("  Test: configuration_parse_tools_mask\n");

	ASSERT(configuration_parse_tools_mask(NULL) == TOOL_NONE);
	ASSERT(configuration_parse_tools_mask("") == TOOL_NONE);
	ASSERT(configuration_parse_tools_mask("none") == TOOL_NONE);
	ASSERT(configuration_parse_tools_mask("all") == TOOL_ALL);

	/* Individual tokens */
	ASSERT(configuration_parse_tools_mask("read") == TOOL_READ);
	ASSERT(configuration_parse_tools_mask("write") == TOOL_WRITE);
	ASSERT(configuration_parse_tools_mask("list_dir") == TOOL_LIST_DIR);
	ASSERT(configuration_parse_tools_mask("memory") == TOOL_MEMORY);
	ASSERT(configuration_parse_tools_mask("run") == TOOL_RUN);

	/* Unimplemented / unknown tokens are silently ignored */
	ASSERT(configuration_parse_tools_mask("bash") == TOOL_NONE);
	ASSERT(configuration_parse_tools_mask("web") == TOOL_NONE);

	/* Combinations */
	unsigned int mask = configuration_parse_tools_mask("read,write");
	ASSERT((mask & TOOL_READ) != 0);
	ASSERT((mask & TOOL_WRITE) != 0);
	ASSERT((mask & TOOL_LIST_DIR) == 0);

	/* Three tokens */
	mask = configuration_parse_tools_mask("read,run,memory");
	ASSERT((mask & TOOL_READ) != 0);
	ASSERT((mask & TOOL_RUN) != 0);
	ASSERT((mask & TOOL_MEMORY) != 0);
	ASSERT((mask & TOOL_WRITE) == 0);

	/* Unknown token is silently ignored */
	mask = configuration_parse_tools_mask("read,unknown_tool");
	ASSERT((mask & TOOL_READ) != 0);
}

/*
 * Test: Config file with comments and blank lines.
 */
static void test_configuration_comments_and_blanks(void)
{
	printf("  Test: comments and blank lines in config\n");

	const char *test_file = "/tmp/boris_config_comments.ini";
	FILE *file = fopen(test_file, "w");
	ASSERT(file != NULL);

	if (file) {
		fprintf(file, "# This is a comment\n");
		fprintf(file, "; This is also a comment\n");
		fprintf(file, "\n");
		fprintf(file, "[model]\n");
		fprintf(file, "# Comment inside a section\n");
		fprintf(file, "name = comment-test\n");
		fprintf(file, "\n");
		fprintf(file, "# Another comment\n");
		fclose(file);
	}

	struct agent_configuration config;
	configuration_set_defaults(&config);

	enum boris_error result = configuration_load_from_file(&config, test_file);
	ASSERT(result == BORIS_OK);

	ASSERT(config.model_name != NULL);
	ASSERT(strcmp(config.model_name, "comment-test") == 0);

	configuration_destroy(&config);
	remove(test_file);
}

/*
 * Test: Layer priority — env var beats INI file for the same key.
 */
static void test_layer_priority_env_beats_ini(void)
{
	printf("  Test: env var overrides INI file value for same key\n");

	const char *test_file = "/tmp/boris_priority_test.ini";
	FILE *file = fopen(test_file, "w");
	ASSERT(file != NULL);
	if (file) {
		fprintf(file, "[model]\n");
		fprintf(file, "name = ini-model\n");
		fclose(file);
	}

	setenv("BORIS_MODEL_NAME", "env-model", 1);

	struct agent_configuration config;
	configuration_set_defaults(&config);
	configuration_load_from_file(&config, test_file);
	configuration_apply_environment(&config);

	ASSERT(config.model_name != NULL);
	if (config.model_name)
		ASSERT(strcmp(config.model_name, "env-model") == 0);

	configuration_destroy(&config);
	unsetenv("BORIS_MODEL_NAME");
	remove(test_file);
}

int main(void)
{
	printf("\n=== Configuration Tests ===\n\n");

	test_configuration_defaults();
	test_configuration_load_from_file();
	test_configuration_missing_file();
	test_configuration_environment();
	test_configuration_destroy();
	test_configuration_destroy_null();
	test_configuration_comments_and_blanks();
	test_parse_tools_mask();
	test_layer_priority_env_beats_ini();

	printf("\n--- Configuration Results ---\n");
	printf("  Passed: %d\n", tests_passed);
	printf("  Failed: %d\n", tests_failed);
	printf("\n");

	return tests_failed > 0 ? 1 : 0;
}
