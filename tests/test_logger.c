/*
 * test_logger.c - Testing Boris's voice.
 *
 * The logger is simpler to test than the arena because it has
 * fewer moving parts. We test that:
 *   1. It initializes without crashing
 *   2. It respects log levels (debug messages don't show at error level)
 *   3. It can write to a file
 *   4. It closes cleanly
 */

#include "logger.h"
#include <stdio.h>
#include <string.h>

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
 * Test: Logger initializes and closes without issues.
 */
static void test_logger_init_and_close(void)
{
	printf("  Test: initialize and close\n");

	logger_initialize(LOG_LEVEL_DEBUG, NULL);
	logger_close();

	tests_passed++; /* If we got here without crashing, it worked */
}

/*
 * Test: Logger can write to a file.
 *
 * We log to a temporary file, then read it back to verify
 * the content was written.
 */
static void test_logger_file_output(void)
{
	printf("  Test: file output\n");

	const char *test_file = "/tmp/boris_logger_test.log";

	/* Remove any previous test file */
	remove(test_file);

	logger_initialize(LOG_LEVEL_DEBUG, test_file);
	log_info("Test message from Boris");
	log_error("Error message from Boris");
	logger_close();

	/* Read the file back and check its contents */
	FILE *file = fopen(test_file, "r");
	ASSERT(file != NULL);

	if (file) {
		char line[256];
		int found_info = 0;
		int found_error = 0;

		while (fgets(line, sizeof(line), file)) {
			if (strstr(line, "Test message from Boris"))
				found_info = 1;
			if (strstr(line, "Error message from Boris"))
				found_error = 1;
		}

		ASSERT(found_info);
		ASSERT(found_error);

		fclose(file);
	}

	/* Clean up */
	remove(test_file);
}

/*
 * Test: Log level filtering works.
 *
 * When the level is set to ERROR, debug messages should not appear.
 */
static void test_logger_level_filtering(void)
{
	printf("  Test: log level filtering\n");

	const char *test_file = "/tmp/boris_logger_level_test.log";

	remove(test_file);

	/* Set level to ERROR - only errors should appear */
	logger_initialize(LOG_LEVEL_ERROR, test_file);
	log_debug("This should not appear");
	log_info("This should not appear either");
	log_warning("Neither should this");
	log_error("But this one should");
	logger_close();

	/* Read the file */
	FILE *file = fopen(test_file, "r");
	ASSERT(file != NULL);

	if (file) {
		char line[256];
		int line_count = 0;

		while (fgets(line, sizeof(line), file)) {
			line_count++;
			/* The only line should contain our error message */
			ASSERT(strstr(line, "But this one should") != NULL);
		}

		ASSERT(line_count == 1);

		fclose(file);
	}

	remove(test_file);
}

/*
 * Test: NULL file path falls back to stderr.
 */
static void test_logger_null_file(void)
{
	printf("  Test: NULL file path (stderr fallback)\n");

	logger_initialize(LOG_LEVEL_DEBUG, NULL);
	log_debug("This goes to stderr");
	logger_close();

	tests_passed++; /* If we got here, it didn't crash */
}

/*
 * Test: Multiple initialize/close cycles work.
 */
static void test_logger_multiple_cycles(void)
{
	printf("  Test: multiple init/close cycles\n");

	logger_initialize(LOG_LEVEL_DEBUG, NULL);
	logger_close();

	logger_initialize(LOG_LEVEL_ERROR, NULL);
	logger_close();

	logger_initialize(LOG_LEVEL_INFO, NULL);
	log_info("Final message");
	logger_close();

	tests_passed++;
}

/*
 * Test: log_level_from_string converts names and numeric strings.
 */
static void test_log_level_from_string(void)
{
	printf("  Test: log_level_from_string\n");

	ASSERT(log_level_from_string("debug", 99) == 0);
	ASSERT(log_level_from_string("info", 99) == 1);
	ASSERT(log_level_from_string("warn", 99) == 2);
	ASSERT(log_level_from_string("warning", 99) == 2);
	ASSERT(log_level_from_string("error", 99) == 3);

	/* Numeric strings */
	ASSERT(log_level_from_string("0", 99) == 0);
	ASSERT(log_level_from_string("2", 99) == 2);
	ASSERT(log_level_from_string("3", 99) == 3);

	/* Unrecognized strings fall back to default */
	ASSERT(log_level_from_string("trace", 7) == 7);
	ASSERT(log_level_from_string("verbose", 5) == 5);
	ASSERT(log_level_from_string("", 4) == 4);

	/* NULL falls back to default */
	ASSERT(log_level_from_string(NULL, 2) == 2);
}

int main(void)
{
	printf("\n=== Logger Tests ===\n\n");

	test_logger_init_and_close();
	test_logger_file_output();
	test_logger_level_filtering();
	test_logger_null_file();
	test_logger_multiple_cycles();
	test_log_level_from_string();

	printf("\n--- Logger Results ---\n");
	printf("  Passed: %d\n", tests_passed);
	printf("  Failed: %d\n", tests_failed);
	printf("\n");

	return tests_failed > 0 ? 1 : 0;
}
