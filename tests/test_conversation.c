/*
 * test_conversation.c - Testing Boris's memory.
 *
 * The conversation is the thread of your shared history with Boris.
 * These tests verify that messages are stored correctly, JSON
 * serialization works, and truncation behaves as expected.
 */

#define _POSIX_C_SOURCE 200809L

#include "conversation.h"
#include "boris_errors.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/*
 * Test: Creating and destroying a conversation.
 */
static void test_conversation_create_destroy(void)
{
	printf("  Test: create and destroy\n");

	struct conversation_history *conv = conversation_create();
	ASSERT(conv != NULL);
	ASSERT(conv->count == 0);
	ASSERT(conv->capacity >= 8);
	ASSERT(conv->total_characters == 0);

	conversation_destroy(conv);
	tests_passed++; /* If we got here without crashing */
}

/*
 * Test: Adding messages of different roles.
 */
static void test_conversation_add_messages(void)
{
	printf("  Test: add messages\n");

	struct conversation_history *conv = conversation_create();

	conversation_add_system(conv, "You are a helpful assistant.");
	ASSERT(conv->count == 1);
	ASSERT(conv->messages[0].role == MESSAGE_ROLE_SYSTEM);
	ASSERT(strcmp(conv->messages[0].content, "You are a helpful assistant.") == 0);

	conversation_add_user(conv, "Hello!");
	ASSERT(conv->count == 2);
	ASSERT(conv->messages[1].role == MESSAGE_ROLE_USER);

	conversation_add_assistant(conv, "Hi there!");
	ASSERT(conv->count == 3);
	ASSERT(conv->messages[2].role == MESSAGE_ROLE_ASSISTANT);

	conversation_destroy(conv);
}

/*
 * Test: Convenience wrappers work correctly.
 */
static void test_conversation_convenience_wrappers(void)
{
	printf("  Test: convenience wrappers\n");

	struct conversation_history *conv = conversation_create();

	conversation_add_system(conv, "System message");
	ASSERT(conv->messages[0].role == MESSAGE_ROLE_SYSTEM);

	conversation_add_user(conv, "User message");
	ASSERT(conv->messages[1].role == MESSAGE_ROLE_USER);

	conversation_add_assistant(conv, "Assistant message");
	ASSERT(conv->messages[2].role == MESSAGE_ROLE_ASSISTANT);

	conversation_destroy(conv);
}

/*
 * Test: Total characters is tracked correctly.
 */
static void test_conversation_character_count(void)
{
	printf("  Test: character count tracking\n");

	struct conversation_history *conv = conversation_create();

	conversation_add_user(conv, "Hello"); /* 5 chars */
	ASSERT(conv->total_characters == 5);

	conversation_add_assistant(conv, "Hi!"); /* 3 chars */
	ASSERT(conv->total_characters == 8);

	conversation_destroy(conv);
}

/*
 * Test: Token estimation is reasonable.
 */
static void test_conversation_token_estimate(void)
{
	printf("  Test: token estimation\n");

	struct conversation_history *conv = conversation_create();

	/* Empty conversation should estimate 0 or near 0 */
	ASSERT(conversation_estimate_tokens(conv) == 0);

	conversation_add_system(conv, "You are helpful.");
	conversation_add_user(conv, "Hello");

	/* Should be a positive number */
	size_t tokens = conversation_estimate_tokens(conv);
	ASSERT(tokens > 0);

	conversation_destroy(conv);
}

/*
 * Test: JSON serialization produces valid structure.
 */
static void test_conversation_json(void)
{
	printf("  Test: JSON serialization\n");

	struct conversation_history *conv = conversation_create();

	conversation_add_system(conv, "You are helpful.");
	conversation_add_user(conv, "Hello");
	conversation_add_assistant(conv, "Hi there!");

	struct cJSON *json = conversation_to_json(conv);
	ASSERT(json != NULL);

	char *str = cJSON_PrintUnformatted(json);
	ASSERT(str != NULL);
	ASSERT(strstr(str, "\"role\":\"system\"") != NULL);
	ASSERT(strstr(str, "\"role\":\"user\"") != NULL);
	ASSERT(strstr(str, "\"role\":\"assistant\"") != NULL);
	ASSERT(strstr(str, "You are helpful.") != NULL);
	ASSERT(strstr(str, "Hello") != NULL);
	ASSERT(strstr(str, "Hi there!") != NULL);

	free(str);
	cJSON_Delete(json);
	conversation_destroy(conv);
}

/*
 * Test: JSON escaping handles special characters.
 */
static void test_conversation_json_escaping(void)
{
	printf("  Test: JSON special character escaping\n");

	struct conversation_history *conv = conversation_create();

	conversation_add_user(conv, "Line1\nLine2\tTab\"Quote\\Backslash");

	struct cJSON *json = conversation_to_json(conv);
	ASSERT(json != NULL);

	char *str = cJSON_PrintUnformatted(json);
	ASSERT(str != NULL);

	/* Newlines should be escaped as \n */
	ASSERT(strstr(str, "\\n") != NULL);
	/* Tabs should be escaped as \t */
	ASSERT(strstr(str, "\\t") != NULL);
	/* Quotes should be escaped as \" */
	ASSERT(strstr(str, "\\\"") != NULL);
	/* Backslashes should be escaped as \\ */
	ASSERT(strstr(str, "\\\\") != NULL);

	free(str);
	cJSON_Delete(json);
	conversation_destroy(conv);
}

/*
 * Test: Conversation grows when capacity is exceeded.
 */
static void test_conversation_growth(void)
{
	printf("  Test: dynamic growth\n");

	struct conversation_history *conv = conversation_create();

	/* Add more messages than the initial capacity */
	for (int i = 0; i < 20; i++) {
		char msg[64];
		snprintf(msg, sizeof(msg), "Message %d", i);
		conversation_add_user(conv, msg);
	}

	ASSERT(conv->count == 20);
	ASSERT(conv->capacity >= 20);

	conversation_destroy(conv);
}

/*
 * Test: Truncation keeps system message and removes oldest pairs.
 */
static void test_conversation_truncation(void)
{
	printf("  Test: truncation\n");

	struct conversation_history *conv = conversation_create();

	conversation_add_system(conv, "You are helpful.");
	conversation_add_user(conv, "Message 1");
	conversation_add_assistant(conv, "Reply 1");
	conversation_add_user(conv, "Message 2");
	conversation_add_assistant(conv, "Reply 2");

	ASSERT(conv->count == 5);

	/* Truncate to a very small token limit */
	conversation_truncate(conv, 10);

	/* System message should always be preserved */
	ASSERT(conv->count >= 1);
	ASSERT(conv->messages[0].role == MESSAGE_ROLE_SYSTEM);
	ASSERT(strcmp(conv->messages[0].content, "You are helpful.") == 0);

	conversation_destroy(conv);
}

/*
 * Test: conversation_last returns the final message.
 */
static void test_conversation_last(void)
{
	printf("  Test: last message\n");

	struct conversation_history *conv = conversation_create();

	/* Empty conversation returns NULL */
	ASSERT(conversation_last(conv) == NULL);

	conversation_add_user(conv, "First");
	conversation_add_assistant(conv, "Second");

	const struct conversation_message *last = conversation_last(conv);
	ASSERT(last != NULL);
	if (last) {
		ASSERT(last->role == MESSAGE_ROLE_ASSISTANT);
		ASSERT(strcmp(last->content, "Second") == 0);
	}

	conversation_destroy(conv);
}

/*
 * Test: conversation_count_role counts messages by role.
 */
static void test_conversation_count_role(void)
{
	printf("  Test: count by role\n");

	struct conversation_history *conv = conversation_create();

	conversation_add_system(conv, "System");
	conversation_add_user(conv, "User 1");
	conversation_add_assistant(conv, "Assistant 1");
	conversation_add_user(conv, "User 2");

	ASSERT(conversation_count_role(conv, MESSAGE_ROLE_SYSTEM) == 1);
	ASSERT(conversation_count_role(conv, MESSAGE_ROLE_USER) == 2);
	ASSERT(conversation_count_role(conv, MESSAGE_ROLE_ASSISTANT) == 1);
	ASSERT(conversation_count_role(conv, MESSAGE_ROLE_TOOL) == 0);

	conversation_destroy(conv);
}

/*
 * Test: conversation_clear removes non-system messages.
 */
static void test_conversation_clear(void)
{
	printf("  Test: clear preserves system message\n");

	struct conversation_history *conv = conversation_create();

	conversation_add_system(conv, "You are Boris.");
	conversation_add_user(conv, "Hello");
	conversation_add_assistant(conv, "Hi");
	ASSERT(conv->count == 3);

	conversation_clear(conv);

	/* System message is preserved; the rest are gone */
	ASSERT(conv->count == 1);
	ASSERT(conv->messages[0].role == MESSAGE_ROLE_SYSTEM);
	ASSERT(strcmp(conv->messages[0].content, "You are Boris.") == 0);
	/* Character count reflects only the system message */
	ASSERT(conv->total_characters == strlen("You are Boris."));

	conversation_destroy(conv);
}

/*
 * Test: conversation_clear on a conversation with no system message.
 */
static void test_conversation_clear_no_system(void)
{
	printf("  Test: clear with no system message\n");

	struct conversation_history *conv = conversation_create();

	conversation_add_user(conv, "Hello");
	conversation_add_assistant(conv, "Hi");
	ASSERT(conv->count == 2);

	conversation_clear(conv); /* Should not crash */

	conversation_destroy(conv);
	tests_passed++;
}

/*
 * Test: conversation_clone deep-copies the source conversation.
 */
static void test_conversation_clone(void)
{
	printf("  Test: clone deep-copies conversation\n");

	struct conversation_history *src = conversation_create();
	conversation_add_system(src, "System prompt");
	conversation_add_user(src, "User message");
	conversation_add_assistant(src, "Assistant reply");

	struct conversation_history *dst = conversation_create();
	enum boris_error err = conversation_clone(dst, src);

	ASSERT(err == BORIS_OK);
	ASSERT(dst->count == src->count);
	ASSERT(dst->total_characters == src->total_characters);

	/* Content must be equal but pointers distinct */
	for (size_t i = 0; i < src->count; i++) {
		ASSERT(dst->messages[i].role == src->messages[i].role);
		if (src->messages[i].content) {
			ASSERT(dst->messages[i].content != NULL);
			ASSERT(strcmp(dst->messages[i].content,
				      src->messages[i].content) == 0);
			ASSERT(dst->messages[i].content !=
			       src->messages[i].content);
		}
	}

	/* Modifying dst should not affect src */
	free(dst->messages[1].content);
	dst->messages[1].content = strdup("Modified");
	ASSERT(strcmp(src->messages[1].content, "User message") == 0);

	conversation_destroy(src);
	conversation_destroy(dst);
}

/*
 * Test: conversation_load_saved_at extracts the timestamp from a session file.
 */
static void test_conversation_load_saved_at(void)
{
	printf("  Test: load_saved_at reads timestamp from session file\n");

	char path[256];
	snprintf(path, sizeof(path),
		 "/tmp/boris_test_saved_at_%d.json", (int)getpid());

	struct conversation_history *conv = conversation_create();
	conversation_add_system(conv, "System");
	conversation_add_user(conv, "Hello");

	enum boris_error err = conversation_save(conv, path);
	ASSERT(err == BORIS_OK);

	char *saved_at = conversation_load_saved_at(path);
	ASSERT(saved_at != NULL);
	if (saved_at) {
		/* Should look like an ISO-8601 date */
		ASSERT(strlen(saved_at) >= 10);
		free(saved_at);
	}

	/* Non-existent file returns NULL */
	char *missing = conversation_load_saved_at(
		"/tmp/boris_no_such_session_99999.json");
	ASSERT(missing == NULL);

	conversation_destroy(conv);
	unlink(path);
}

/*
 * Test: NULL handling is safe.
 */
static void test_conversation_null_safety(void)
{
	printf("  Test: NULL safety\n");

	/* Destroying NULL should not crash */
	conversation_destroy(NULL);
	tests_passed++;

	/* Adding to NULL should return error */
	enum boris_error err = conversation_add(NULL, MESSAGE_ROLE_USER, "test");
	ASSERT(err != BORIS_OK);

	/* Token estimation of NULL should return 0 */
	ASSERT(conversation_estimate_tokens(NULL) == 0);
}

int main(void)
{
	printf("\n=== Conversation Tests ===\n\n");

	test_conversation_create_destroy();
	test_conversation_add_messages();
	test_conversation_convenience_wrappers();
	test_conversation_character_count();
	test_conversation_token_estimate();
	test_conversation_json();
	test_conversation_json_escaping();
	test_conversation_growth();
	test_conversation_truncation();
	test_conversation_last();
	test_conversation_count_role();
	test_conversation_clear();
	test_conversation_clear_no_system();
	test_conversation_clone();
	test_conversation_load_saved_at();
	test_conversation_null_safety();

	printf("\n--- Conversation Results ---\n");
	printf("  Passed: %d\n", tests_passed);
	printf("  Failed: %d\n", tests_failed);
	printf("\n");

	return tests_failed > 0 ? 1 : 0;
}
