/*
 * mock_http.h - Mock HTTP for end-to-end testing.
 *
 * Replaces http_post() with a mock that returns pre-canned responses.
 * This lets us test the full agent loop without a real LLM endpoint.
 *
 * Usage:
 *   1. Call mock_http_reset() before each test
 *   2. Call mock_http_queue_response(json, status_code) to queue responses
 *   3. Call mock_http_enable() to activate the mock
 *   4. Run the agent loop - it will use queued responses
 *   5. Call mock_http_disable() to restore real HTTP
 */

#ifndef MOCK_HTTP_H
#define MOCK_HTTP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of queued mock responses */
#define MOCK_HTTP_MAX_QUEUED 32

/*
 * Queue a mock HTTP response.
 *
 * json_body    : The JSON response body (will be copied)
 * status_code  : HTTP status code to return (200, 500, etc.)
 *
 * Responses are returned in FIFO order when http_post() is called.
 */
void mock_http_queue_response(const char *json_body, long status_code);

/*
 * Queue a mock HTTP error.
 *
 * error_msg : Error message to return
 *
 * Simulates a transport-level failure (no HTTP response received).
 */
void mock_http_queue_error(const char *error_msg);

/*
 * Enable the mock HTTP layer.
 *
 * After this call, all http_post() calls will use queued responses
 * instead of making real HTTP requests.
 */
void mock_http_enable(void);

/*
 * Disable the mock HTTP layer.
 *
 * Restores real HTTP functionality.
 */
void mock_http_disable(void);

/*
 * Reset the mock HTTP state.
 *
 * Clears all queued responses and call history.
 * Call this before each test.
 */
void mock_http_reset(void);

/*
 * Return the number of HTTP requests made since mock was enabled.
 */
int mock_http_call_count(void);

/*
 * Return the request body from the Nth call (0-indexed).
 * Returns NULL if the call index is out of range.
 * The returned string is owned by the mock - do not free it.
 */
const char *mock_http_request_body(int call_index);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_HTTP_H */
