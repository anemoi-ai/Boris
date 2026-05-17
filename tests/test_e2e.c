/*
 * test_e2e.c - End-to-end tests for Boris.
 *
 * Tests the full agent loop with mock HTTP responses.
 * No real LLM endpoint needed.
 */

#define _POSIX_C_SOURCE 200809L

#include "mock_http.h"
#include "agent.h"
#include "llm.h"
#include "tools.h"
#include "conversation.h"
#include "configuration.h"
#include "boris_types.h"
#include "boris_errors.h"
#include "cJSON.h"

/* Forward declarations for tool implementations */
struct tool_result tool_read_fn(const char *arguments_json,
				const struct agent_configuration *cfg,
				struct memory_arena *scratch);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PASS() printf("    PASS\n")
#define FAIL(msg)                              \
	do {                                   \
		printf("    FAIL: %s\n", msg); \
		failures++;                    \
	} while (0)
#define ASSERT(cond, msg)          \
	do {                       \
		if (!(cond)) {     \
			FAIL(msg); \
		} else {           \
			PASS();    \
		}                  \
	} while (0)

static int failures = 0;

/*
 * Helper: create a minimal config for testing.
 */
static struct agent_configuration make_test_config(void)
{
	struct agent_configuration cfg;
	configuration_set_defaults(&cfg);
	cfg.model_endpoint = strdup("http://localhost:9999/v1/chat/completions");
	cfg.model_name = strdup("test-model");
	cfg.max_iterations = 4;
	cfg.max_retries = 0;
	cfg.tools_enabled = TOOL_NONE;
	return cfg;
}

/*
 * Helper: create a JSON stop response.
 */
static char *make_stop_response(const char *content)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *choices = cJSON_CreateArray();
	cJSON *choice = cJSON_CreateObject();
	cJSON *message = cJSON_CreateObject();
	cJSON_AddStringToObject(message, "role", "assistant");
	cJSON_AddStringToObject(message, "content", content);
	cJSON_AddItemToObject(choice, "message", message);
	cJSON_AddStringToObject(choice, "finish_reason", "stop");
	cJSON_AddItemToArray(choices, choice);
	cJSON_AddItemToObject(root, "choices", choices);
	cJSON *usage = cJSON_CreateObject();
	cJSON_AddNumberToObject(usage, "prompt_tokens", 10);
	cJSON_AddNumberToObject(usage, "completion_tokens", 5);
	cJSON_AddItemToObject(root, "usage", usage);
	char *str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return str;
}

/*
 * Helper: create a JSON tool_calls response.
 */
static char *make_tool_response(const char *content,
				const char *tool_id,
				const char *tool_name,
				const char *tool_args)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *choices = cJSON_CreateArray();
	cJSON *choice = cJSON_CreateObject();
	cJSON *message = cJSON_CreateObject();

	if (content)
		cJSON_AddStringToObject(message, "content", content);
	else
		cJSON_AddNullToObject(message, "content");

	cJSON *tc_arr = cJSON_CreateArray();
	cJSON *tc = cJSON_CreateObject();
	cJSON_AddStringToObject(tc, "id", tool_id);
	cJSON_AddStringToObject(tc, "type", "function");
	cJSON *fn = cJSON_CreateObject();
	cJSON_AddStringToObject(fn, "name", tool_name);
	cJSON_AddStringToObject(fn, "arguments", tool_args);
	cJSON_AddItemToObject(tc, "function", fn);
	cJSON_AddItemToArray(tc_arr, tc);
	cJSON_AddItemToObject(message, "tool_calls", tc_arr);

	cJSON_AddItemToObject(choice, "message", message);
	cJSON_AddStringToObject(choice, "finish_reason", "tool_calls");
	cJSON_AddItemToArray(choices, choice);
	cJSON_AddItemToObject(root, "choices", choices);

	cJSON *usage = cJSON_CreateObject();
	cJSON_AddNumberToObject(usage, "prompt_tokens", 20);
	cJSON_AddNumberToObject(usage, "completion_tokens", 15);
	cJSON_AddItemToObject(root, "usage", usage);

	char *str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return str;
}

/*
 * Helper: create a JSON error response from the API.
 */
static char *make_api_error_response(const char *error_msg)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *err = cJSON_CreateObject();
	cJSON_AddStringToObject(err, "message", error_msg);
	cJSON_AddItemToObject(root, "error", err);
	char *str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return str;
}

/* -------------------------------------------------------------------------
 * Test 1: Simple chat - user message gets a stop response
 * ---------------------------------------------------------------------- */
static void test_simple_chat(void)
{
	printf("  Test: Simple chat (stop response)\n");

	mock_http_reset();
	mock_http_enable();

	char *resp = make_stop_response("Hello! How can I help?");
	mock_http_queue_response(resp, 200);
	free(resp);

	struct agent_configuration cfg = make_test_config();
	struct agent_result result = agent_run("Hi Boris", &cfg);

	ASSERT(result.finish_reason == FINISHED_NATURALLY,
	       "finish_reason should be FINISHED_NATURALLY");
	ASSERT(result.final_response != NULL,
	       "should have a response");
	if (result.final_response)
		ASSERT(strcmp(result.final_response, "Hello! How can I help?") == 0,
		       "response content should match");
	ASSERT(result.turns_used == 1, "should use 1 turn");

	agent_result_free(&result);
	configuration_destroy(&cfg);
	mock_http_disable();
}

/*
 * Test 2: LLM error returns FINISHED_ERROR
 * ---------------------------------------------------------------------- */
static void test_llm_error(void)
{
	printf("  Test: LLM error returns FINISHED_ERROR\n");

	mock_http_reset();
	mock_http_enable();

	char *err_resp = make_api_error_response("Model overloaded");
	mock_http_queue_response(err_resp, 500);
	free(err_resp);

	struct agent_configuration cfg = make_test_config();
	struct agent_result result = agent_run("Hi", &cfg);

	ASSERT(result.finish_reason == FINISHED_ERROR,
	       "finish_reason should be FINISHED_ERROR");
	ASSERT(result.error_code == BORIS_ERROR_LLM,
	       "error_code should be BORIS_ERROR_LLM");

	agent_result_free(&result);
	configuration_destroy(&cfg);
	mock_http_disable();
}

/*
 * Test 3: Max iterations reached
 * ---------------------------------------------------------------------- */
static void test_max_iterations(void)
{
	printf("  Test: Max iterations reached\n");

	mock_http_reset();
	mock_http_enable();

	/* Queue tool_call responses that keep looping */
	char *tc1 = make_tool_response(NULL, "call_1", "memory",
				       "{\"action\":\"get\",\"key\":\"x\"}");
	char *tc2 = make_tool_response(NULL, "call_2", "memory",
				       "{\"action\":\"get\",\"key\":\"y\"}");
	char *tc3 = make_tool_response(NULL, "call_3", "memory",
				       "{\"action\":\"get\",\"key\":\"z\"}");
	char *tc4 = make_tool_response(NULL, "call_4", "memory",
				       "{\"action\":\"get\",\"key\":\"w\"}");
	char *tc5 = make_tool_response(NULL, "call_5", "memory",
				       "{\"action\":\"get\",\"key\":\"v\"}");

	mock_http_queue_response(tc1, 200);
	mock_http_queue_response(tc2, 200);
	mock_http_queue_response(tc3, 200);
	mock_http_queue_response(tc4, 200);
	mock_http_queue_response(tc5, 200);

	free(tc1);
	free(tc2);
	free(tc3);
	free(tc4);
	free(tc5);

	struct agent_configuration cfg = make_test_config();
	cfg.max_iterations = 4;
	cfg.tools_enabled = TOOL_MEMORY;
	tools_register_builtins();

	struct agent_result result = agent_run("Test", &cfg);

	ASSERT(result.finish_reason == FINISHED_MAX_ITERATIONS,
	       "should hit max iterations");
	ASSERT(result.turns_used == 4, "should use exactly 4 turns");

	agent_result_free(&result);
	configuration_destroy(&cfg);
	mock_http_disable();
}

/*
 * Test 4: Tool call followed by stop response
 * ---------------------------------------------------------------------- */
static void test_tool_call_then_stop(void)
{
	printf("  Test: Tool call then stop\n");

	mock_http_reset();
	mock_http_enable();

	/* First response: tool call */
	char *tc = make_tool_response(NULL, "call_1", "memory",
				      "{\"action\":\"list\"}");
	/* Second response: stop after tool result */
	char *stop = make_stop_response("Here are your memories: (empty)");

	mock_http_queue_response(tc, 200);
	mock_http_queue_response(stop, 200);

	free(tc);
	free(stop);

	struct agent_configuration cfg = make_test_config();
	cfg.tools_enabled = TOOL_MEMORY;
	tools_register_builtins();

	struct agent_result result = agent_run("What do I have in memory?", &cfg);

	ASSERT(result.finish_reason == FINISHED_NATURALLY,
	       "should finish naturally");
	ASSERT(result.final_response != NULL, "should have a response");
	ASSERT(result.turns_used == 2,
	       "should use 2 turns (tool call + final)");

	agent_result_free(&result);
	configuration_destroy(&cfg);
	mock_http_disable();
}

/*
 * Test 5: Multiple tool calls in one turn
 * ---------------------------------------------------------------------- */
static void test_multiple_tool_calls(void)
{
	printf("  Test: Multiple tool calls in one turn\n");

	mock_http_reset();
	mock_http_enable();

	/* Response with two tool calls */
	cJSON *root = cJSON_CreateObject();
	cJSON *choices = cJSON_CreateArray();
	cJSON *choice = cJSON_CreateObject();
	cJSON *message = cJSON_CreateObject();
	cJSON_AddNullToObject(message, "content");

	cJSON *tc_arr = cJSON_CreateArray();

	cJSON *tc1 = cJSON_CreateObject();
	cJSON_AddStringToObject(tc1, "id", "call_a");
	cJSON_AddStringToObject(tc1, "type", "function");
	cJSON *fn1 = cJSON_CreateObject();
	cJSON_AddStringToObject(fn1, "name", "memory");
	cJSON_AddStringToObject(fn1, "arguments", "{\"action\":\"get\",\"key\":\"a\"}");
	cJSON_AddItemToObject(tc1, "function", fn1);
	cJSON_AddItemToArray(tc_arr, tc1);

	cJSON *tc2 = cJSON_CreateObject();
	cJSON_AddStringToObject(tc2, "id", "call_b");
	cJSON_AddStringToObject(tc2, "type", "function");
	cJSON *fn2 = cJSON_CreateObject();
	cJSON_AddStringToObject(fn2, "name", "memory");
	cJSON_AddStringToObject(fn2, "arguments", "{\"action\":\"get\",\"key\":\"b\"}");
	cJSON_AddItemToObject(tc2, "function", fn2);
	cJSON_AddItemToArray(tc_arr, tc2);

	cJSON_AddItemToObject(message, "tool_calls", tc_arr);
	cJSON_AddItemToObject(choice, "message", message);
	cJSON_AddStringToObject(choice, "finish_reason", "tool_calls");
	cJSON_AddItemToArray(choices, choice);
	cJSON_AddItemToObject(root, "choices", choices);

	cJSON *usage = cJSON_CreateObject();
	cJSON_AddNumberToObject(usage, "prompt_tokens", 30);
	cJSON_AddNumberToObject(usage, "completion_tokens", 20);
	cJSON_AddItemToObject(root, "usage", usage);

	char *resp = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	mock_http_queue_response(resp, 200);
	free(resp);

	/* Second response: stop */
	char *stop = make_stop_response("Done checking both keys.");
	mock_http_queue_response(stop, 200);
	free(stop);

	struct agent_configuration cfg = make_test_config();
	cfg.tools_enabled = TOOL_MEMORY;
	tools_register_builtins();

	struct agent_result result = agent_run("Check keys a and b", &cfg);

	ASSERT(result.finish_reason == FINISHED_NATURALLY,
	       "should finish naturally");
	ASSERT(result.turns_used == 2,
	       "should use 2 turns (multi-tool + final)");

	agent_result_free(&result);
	configuration_destroy(&cfg);
	mock_http_disable();
}

/*
 * Test 6: Conversation truncation under load
 * ---------------------------------------------------------------------- */
static void test_conversation_truncation(void)
{
	printf("  Test: Conversation truncation\n");

	/* Create a conversation that's already long */
	struct conversation_history *conv = conversation_create();
	conversation_add_system(conv, "You are Boris.");

	/* Add many user/assistant pairs with long content to fill context */
	for (int i = 0; i < 50; i++) {
		char buf[1024];
		/* Each message is ~500 chars = ~125 tokens */
		snprintf(buf, sizeof(buf),
			 "This is message number %d. "
			 "It contains a lot of text to fill up the context window. "
			 "We need enough tokens to trigger truncation. "
			 "Lorem ipsum dolor sit amet, consectetur adipiscing elit. "
			 "Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.",
			 i);
		conversation_add_user(conv, buf);
		snprintf(buf, sizeof(buf),
			 "This is the response to message number %d. "
			 "It also contains a lot of text to fill up the context window. "
			 "We need enough tokens to trigger truncation properly. "
			 "Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris.",
			 i);
		conversation_add_assistant(conv, buf);
	}

	size_t tokens_before = conversation_estimate_tokens(conv);

	/* Truncate to a small target */
	size_t target = 2000; /* Much smaller than the ~12500 tokens we have */
	conversation_truncate(conv, target);

	size_t tokens_after = conversation_estimate_tokens(conv);

	ASSERT(tokens_after < tokens_before,
	       "tokens should decrease after truncation");
	ASSERT(conv->count > 0, "conversation should not be empty");
	/* System message should always be preserved */
	ASSERT(conv->messages[0].role == MESSAGE_ROLE_SYSTEM,
	       "system message should be preserved");

	conversation_destroy(conv);
}

/*
 * Test 7: Tool error is reported to model and loop continues
 * ---------------------------------------------------------------------- */
static void test_tool_error_continues_loop(void)
{
	printf("  Test: Tool error continues loop\n");

	mock_http_reset();
	mock_http_enable();

	/* Tool call for unknown tool */
	char *tc = make_tool_response(NULL, "call_1", "nonexistent_tool", "{}");
	mock_http_queue_response(tc, 200);
	free(tc);

	/* Model gets error result and stops */
	char *stop = make_stop_response("I see that tool doesn't exist.");
	mock_http_queue_response(stop, 200);
	free(stop);

	struct agent_configuration cfg = make_test_config();
	cfg.tools_enabled = TOOL_MEMORY;
	tools_register_builtins();

	struct agent_result result = agent_run("Use nonexistent_tool", &cfg);

	ASSERT(result.finish_reason == FINISHED_NATURALLY,
	       "should finish naturally despite tool error");
	ASSERT(result.turns_used == 2,
	       "should use 2 turns (tool error + final)");

	agent_result_free(&result);
	configuration_destroy(&cfg);
	mock_http_disable();
}

/*
 * Test 8: Empty response body
 * ---------------------------------------------------------------------- */
static void test_empty_response(void)
{
	printf("  Test: Empty response body\n");

	mock_http_reset();
	mock_http_enable();

	/* Queue a 200 with empty body */
	mock_http_queue_response("", 200);

	struct agent_configuration cfg = make_test_config();
	struct agent_result result = agent_run("Hi", &cfg);

	ASSERT(result.finish_reason == FINISHED_ERROR,
	       "empty response should be an error");

	agent_result_free(&result);
	configuration_destroy(&cfg);
	mock_http_disable();
}

/*
 * Test 9: Sandbox escape attempt
 * ---------------------------------------------------------------------- */
static void test_sandbox_escape(void)
{
	printf("  Test: Sandbox escape attempt\n");

	/* Create a temp sandbox directory */
	char sandbox[256];
	snprintf(sandbox, sizeof(sandbox), "/tmp/boris_test_sandbox_%d", getpid());
	mkdir(sandbox, 0755);

	/* Create a file inside sandbox */
	char safe_file[512];
	snprintf(safe_file, sizeof(safe_file), "%s/safe.txt", sandbox);
	FILE *f = fopen(safe_file, "w");
	if (f) {
		fprintf(f, "safe content\n");
		fclose(f);
	}

	/* Create a file outside sandbox */
	FILE *f2 = fopen("/tmp/boris_outside.txt", "w");
	if (f2) {
		fprintf(f2, "outside content\n");
		fclose(f2);
	}

	struct agent_configuration cfg = make_test_config();
	cfg.sandbox_root = strdup(sandbox);
	cfg.tools_enabled = TOOL_READ;
	tools_register_builtins();

	/* Test: read file inside sandbox - should succeed */
	char args_inside[512];
	snprintf(args_inside, sizeof(args_inside), "{\"path\":\"safe.txt\"}");
	struct tool_result r1 = tool_read_fn(args_inside, &cfg, NULL);
	ASSERT(r1.is_error == false, "reading file inside sandbox should succeed");
	tool_result_free(&r1);

	/* Test: read file outside sandbox via path traversal - should fail */
	char args_escape[512];
	snprintf(args_escape, sizeof(args_escape),
		 "{\"path\":\"../../tmp/boris_outside.txt\"}");
	struct tool_result r2 = tool_read_fn(args_escape, &cfg, NULL);
	ASSERT(r2.is_error == true, "path traversal should be blocked");
	tool_result_free(&r2);

	/* Cleanup */
	unlink(safe_file);
	unlink("/tmp/boris_outside.txt");
	rmdir(sandbox);

	configuration_destroy(&cfg);
}

/*
 * Test 10: HTTP mock tracks request bodies
 * ---------------------------------------------------------------------- */
static void test_mock_tracks_requests(void)
{
	printf("  Test: Mock tracks request bodies\n");

	mock_http_reset();
	mock_http_enable();

	char *resp = make_stop_response("OK");
	mock_http_queue_response(resp, 200);
	free(resp);

	struct agent_configuration cfg = make_test_config();
	struct agent_result result = agent_run("Hello Boris", &cfg);
	agent_result_free(&result);

	ASSERT(mock_http_call_count() == 1, "should have made 1 HTTP call");

	const char *body = mock_http_request_body(0);
	ASSERT(body != NULL, "request body should not be NULL");
	ASSERT(strstr(body, "\"messages\"") != NULL,
	       "request should contain messages array");
	ASSERT(strstr(body, "\"model\":\"test-model\"") != NULL,
	       "request should contain model name");

	configuration_destroy(&cfg);
	mock_http_disable();
}

/*
 * Test 11: Conversation save and load
 * ---------------------------------------------------------------------- */
static void test_conversation_save_load(void)
{
	printf("  Test: Conversation save and load\n");

	char path[256];
	snprintf(path, sizeof(path), "/tmp/boris_test_session_%d.json", getpid());

	/* Create a conversation with various message types */
	struct conversation_history *conv = conversation_create();
	conversation_add_system(conv, "You are Boris.");
	conversation_add_user(conv, "Hello!");
	conversation_add_assistant(conv, "Hi there!");
	conversation_add_tool_result(conv, "call_1", "memory", "{\"key\":\"value\"}");

	/* Save it */
	ASSERT(conversation_save(conv, path) == BORIS_OK,
	       "save should succeed");

	/* Load it back */
	struct conversation_history *loaded = conversation_load(path);
	ASSERT(loaded != NULL, "loaded conversation should not be NULL");
	ASSERT(loaded->count == conv->count,
	       "loaded message count should match");
	ASSERT(loaded->messages[0].role == MESSAGE_ROLE_SYSTEM,
	       "first message should be system");
	ASSERT(loaded->messages[1].role == MESSAGE_ROLE_USER,
	       "second message should be user");
	ASSERT(loaded->messages[2].role == MESSAGE_ROLE_ASSISTANT,
	       "third message should be assistant");
	ASSERT(loaded->messages[3].role == MESSAGE_ROLE_TOOL,
	       "fourth message should be tool");

	/* Verify content */
	ASSERT(strcmp(loaded->messages[1].content, "Hello!") == 0,
	       "user message content should match");
	ASSERT(strcmp(loaded->messages[2].content, "Hi there!") == 0,
	       "assistant message content should match");
	ASSERT(strcmp(loaded->messages[3].tool_call_id, "call_1") == 0,
	       "tool_call_id should match");
	ASSERT(strcmp(loaded->messages[3].tool_name, "memory") == 0,
	       "tool_name should match");

	conversation_destroy(loaded);
	conversation_destroy(conv);
	unlink(path);
}

/*
 * Test 12: Load non-existent file returns NULL
 * ---------------------------------------------------------------------- */
static void test_load_nonexistent(void)
{
	printf("  Test: Load non-existent file returns NULL\n");

	struct conversation_history *loaded = conversation_load(
		"/tmp/boris_nonexistent_session_99999.json");
	ASSERT(loaded == NULL, "loading non-existent file should return NULL");
}

/*
 * Test 14: Transport-level error (curl failure, no HTTP response)
 * ---------------------------------------------------------------------- */
static void test_transport_error(void)
{
	printf("  Test: Transport-level error returns FINISHED_ERROR\n");

	mock_http_reset();
	mock_http_enable();

	mock_http_queue_error("Connection refused");

	struct agent_configuration cfg = make_test_config();
	struct agent_result result = agent_run("Hi", &cfg);

	ASSERT(result.finish_reason == FINISHED_ERROR,
	       "transport error should be FINISHED_ERROR");

	agent_result_free(&result);
	configuration_destroy(&cfg);
	mock_http_disable();
}

/*
 * Test 15: Text tool fallback parses <tool_call> tags in stop responses
 * ---------------------------------------------------------------------- */
static void test_text_tool_fallback(void)
{
	printf("  Test: Text tool fallback parses <tool_call> tags\n");

	mock_http_reset();
	mock_http_enable();

	/* Stop response whose content embeds a tool call in XML tags */
	cJSON *root = cJSON_CreateObject();
	cJSON *choices = cJSON_CreateArray();
	cJSON *choice = cJSON_CreateObject();
	cJSON *message = cJSON_CreateObject();
	cJSON_AddStringToObject(message, "role", "assistant");
	cJSON_AddStringToObject(message, "content",
				"<tool_call>"
				"{\"name\":\"memory\","
				"\"arguments\":{\"action\":\"list\"}}"
				"</tool_call>");
	cJSON_AddItemToObject(choice, "message", message);
	cJSON_AddStringToObject(choice, "finish_reason", "stop");
	cJSON_AddItemToArray(choices, choice);
	cJSON_AddItemToObject(root, "choices", choices);
	cJSON *usage = cJSON_CreateObject();
	cJSON_AddNumberToObject(usage, "prompt_tokens", 10);
	cJSON_AddNumberToObject(usage, "completion_tokens", 5);
	cJSON_AddItemToObject(root, "usage", usage);
	char *resp1 = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	mock_http_queue_response(resp1, 200);
	free(resp1);

	/* Second call: final stop after tool result is fed back */
	char *resp2 = make_stop_response("Memory list complete.");
	mock_http_queue_response(resp2, 200);
	free(resp2);

	struct agent_configuration cfg = make_test_config();
	cfg.text_tool_fallback = true;
	cfg.tools_enabled = TOOL_MEMORY;
	tools_register_builtins();

	struct agent_result result = agent_run("What's in memory?", &cfg);

	ASSERT(result.finish_reason == FINISHED_NATURALLY,
	       "should finish naturally after text tool fallback");
	ASSERT(result.turns_used == 2,
	       "should use 2 turns (tool call + final answer)");
	ASSERT(mock_http_call_count() == 2,
	       "should make exactly 2 HTTP calls");

	agent_result_free(&result);
	configuration_destroy(&cfg);
	mock_http_disable();
}

/*
 * Test 16: Text tool fallback parses markdown code-fenced JSON
 * ---------------------------------------------------------------------- */
static void test_text_tool_fallback_markdown(void)
{
	printf("  Test: Text tool fallback parses markdown code-fenced JSON\n");

	mock_http_reset();
	mock_http_enable();

	cJSON *root = cJSON_CreateObject();
	cJSON *choices = cJSON_CreateArray();
	cJSON *choice = cJSON_CreateObject();
	cJSON *message = cJSON_CreateObject();
	cJSON_AddStringToObject(message, "role", "assistant");
	cJSON_AddStringToObject(message, "content",
				"```json\n"
				"{\"tool\":\"memory\","
				"\"action\":\"list\"}\n"
				"```");
	cJSON_AddItemToObject(choice, "message", message);
	cJSON_AddStringToObject(choice, "finish_reason", "stop");
	cJSON_AddItemToArray(choices, choice);
	cJSON_AddItemToObject(root, "choices", choices);
	cJSON *usage = cJSON_CreateObject();
	cJSON_AddNumberToObject(usage, "prompt_tokens", 10);
	cJSON_AddNumberToObject(usage, "completion_tokens", 5);
	cJSON_AddItemToObject(root, "usage", usage);
	char *resp1 = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	mock_http_queue_response(resp1, 200);
	free(resp1);

	char *resp2 = make_stop_response("Memory list complete.");
	mock_http_queue_response(resp2, 200);
	free(resp2);

	struct agent_configuration cfg = make_test_config();
	cfg.text_tool_fallback = true;
	cfg.tools_enabled = TOOL_MEMORY;
	cfg.memory_persist = false;
	tools_register_builtins();

	struct agent_result result = agent_run("What's in memory?", &cfg);

	ASSERT(result.finish_reason == FINISHED_NATURALLY,
	       "should finish naturally after markdown fallback");
	ASSERT(result.turns_used == 2,
	       "should use 2 turns (tool call + final answer)");
	ASSERT(mock_http_call_count() == 2,
	       "should make exactly 2 HTTP calls");

	agent_result_free(&result);
	configuration_destroy(&cfg);
	mock_http_disable();
}

/*
 * Test 17: length finish_reason with short conversation returns error
 * ---------------------------------------------------------------------- */
static void test_length_short_conversation(void)
{
	printf("  Test: length finish_reason with short conversation\n");

	mock_http_reset();
	mock_http_enable();

	/* Build a length response */
	cJSON *root = cJSON_CreateObject();
	cJSON *choices = cJSON_CreateArray();
	cJSON *choice = cJSON_CreateObject();
	cJSON *message = cJSON_CreateObject();
	cJSON_AddStringToObject(message, "role", "assistant");
	cJSON_AddStringToObject(message, "content", "partial...");
	cJSON_AddItemToObject(choice, "message", message);
	cJSON_AddStringToObject(choice, "finish_reason", "length");
	cJSON_AddItemToArray(choices, choice);
	cJSON_AddItemToObject(root, "choices", choices);
	cJSON *usage = cJSON_CreateObject();
	cJSON_AddNumberToObject(usage, "prompt_tokens", 10);
	cJSON_AddNumberToObject(usage, "completion_tokens", 5);
	cJSON_AddItemToObject(root, "usage", usage);
	char *resp = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	mock_http_queue_response(resp, 200);
	free(resp);

	struct agent_configuration cfg = make_test_config();
	/* Tiny context window so tokens > target, but conversation has only
	 * 2 messages (≤3), so the truncation branch is skipped and we error */
	cfg.context_window = 50;

	struct agent_result result = agent_run("Short", &cfg);

	ASSERT(result.finish_reason == FINISHED_ERROR,
	       "length with short conversation should return error");

	agent_result_free(&result);
	configuration_destroy(&cfg);
	mock_http_disable();
}

/*
 * Test 13: Session path resolution
 * ---------------------------------------------------------------------- */
static void test_session_path(void)
{
	printf("  Test: Session path resolution\n");

	char *path = conversation_session_path();
	/* Should return a path or NULL if no HOME/XDG */
	if (path) {
		ASSERT(strstr(path, "session.json") != NULL,
		       "session path should end with session.json");
		free(path);
	}
}

/*
 * Test 18: SSE stream with malformed JSON is skipped gracefully
 * ---------------------------------------------------------------------- */
static void test_stream_malformed_json(void)
{
	printf("  Test: Stream with malformed SSE JSON is skipped gracefully\n");

	mock_http_reset();
	mock_http_enable();

	/* Body starts with "data:" so llm.c routes it to parse_streaming.
	 * The malformed line is silently skipped; [DONE] closes the stream. */
	mock_http_queue_response(
		"data: {this is not valid json}\n"
		"data: [DONE]\n",
		200);

	struct agent_configuration cfg = make_test_config();
	cfg.text_tool_fallback = false;
	struct agent_result result = agent_run("Hi", &cfg);

	ASSERT(result.finish_reason == FINISHED_NATURALLY,
	       "malformed SSE should still finish naturally");

	agent_result_free(&result);
	configuration_destroy(&cfg);
	mock_http_disable();
}

/*
 * Test 19: SSE stream without [DONE] still returns accumulated content
 * ---------------------------------------------------------------------- */
static void test_stream_no_done(void)
{
	printf("  Test: Stream without [DONE] returns accumulated content\n");

	mock_http_reset();
	mock_http_enable();

	mock_http_queue_response(
		"data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"},\"finish_reason\":null}]}\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\" world\"},\"finish_reason\":null}]}\n",
		200);

	struct agent_configuration cfg = make_test_config();
	struct agent_result result = agent_run("Hi", &cfg);

	ASSERT(result.finish_reason == FINISHED_NATURALLY,
	       "stream without [DONE] should finish naturally");
	ASSERT(result.final_response != NULL,
	       "should have accumulated content");
	if (result.final_response)
		ASSERT(strcmp(result.final_response, "Hello world") == 0,
		       "accumulated content should match");

	agent_result_free(&result);
	configuration_destroy(&cfg);
	mock_http_disable();
}

/*
 * Test 20: SSE stream where finish_reason is absent from the final delta
 * ---------------------------------------------------------------------- */
static void test_stream_no_finish_reason(void)
{
	printf("  Test: Stream with no finish_reason in delta uses default stop\n");

	mock_http_reset();
	mock_http_enable();

	/* No "finish_reason" field in the delta - default "stop" should apply. */
	mock_http_queue_response(
		"data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\n"
		"data: [DONE]\n",
		200);

	struct agent_configuration cfg = make_test_config();
	struct agent_result result = agent_run("Hello", &cfg);

	ASSERT(result.finish_reason == FINISHED_NATURALLY,
	       "absent finish_reason should default to stop");
	ASSERT(result.final_response != NULL,
	       "should have content");
	if (result.final_response)
		ASSERT(strcmp(result.final_response, "Hi") == 0,
		       "content should match");

	agent_result_free(&result);
	configuration_destroy(&cfg);
	mock_http_disable();
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

int main(void)
{
	printf("\n=== End-to-End Tests ===\n\n");

	test_simple_chat();
	test_llm_error();
	test_max_iterations();
	test_tool_call_then_stop();
	test_multiple_tool_calls();
	test_conversation_truncation();
	test_tool_error_continues_loop();
	test_empty_response();
	test_sandbox_escape();
	test_mock_tracks_requests();
	test_conversation_save_load();
	test_load_nonexistent();
	test_session_path();
	test_transport_error();
	test_text_tool_fallback();
	test_text_tool_fallback_markdown();
	test_length_short_conversation();
	test_stream_malformed_json();
	test_stream_no_done();
	test_stream_no_finish_reason();

	printf("\n--- E2E Results ---\n");
	if (failures == 0)
		printf("  All tests passed!\n");
	else
		printf("  %d test(s) failed\n", failures);
	printf("\n");

	return failures > 0 ? 1 : 0;
}
