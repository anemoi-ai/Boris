/*
 * mock_http.c - Mock HTTP implementation for testing.
 *
 * Provides a complete replacement for src/http_client.c that returns
 * pre-canned responses instead of making real HTTP requests. Linked into
 * test_e2e in place of the real http_client.o.
 *
 * mock_http_enable() and mock_http_disable() are no-ops: all http_post()
 * calls in this binary are mocked by definition since this file is linked
 * instead of http_client.c.
 */
#define _POSIX_C_SOURCE 200809L

#include "http_client.h"
#include "mock_http.h"
#include <stdlib.h>
#include <string.h>

struct mock_response {
	char *body;
	long status_code;
	int is_error;
};

static struct mock_response g_mock_responses[MOCK_HTTP_MAX_QUEUED];
static int g_mock_count = 0;
static int g_mock_index = 0;

static char *g_mock_request_bodies[MOCK_HTTP_MAX_QUEUED];
static int g_mock_request_count = 0;

/* -------------------------------------------------------------------------
 * Mock control API
 * ---------------------------------------------------------------------- */

void mock_http_queue_response(const char *json_body, long status_code)
{
	if (g_mock_count >= MOCK_HTTP_MAX_QUEUED)
		return;
	g_mock_responses[g_mock_count].body = strdup(json_body ? json_body : "");
	g_mock_responses[g_mock_count].status_code = status_code;
	g_mock_responses[g_mock_count].is_error = 0;
	g_mock_count++;
}

void mock_http_queue_error(const char *error_msg)
{
	if (g_mock_count >= MOCK_HTTP_MAX_QUEUED)
		return;
	g_mock_responses[g_mock_count].body = strdup(error_msg ? error_msg : "mock error");
	g_mock_responses[g_mock_count].status_code = 0;
	g_mock_responses[g_mock_count].is_error = 1;
	g_mock_count++;
}

void mock_http_enable(void)
{ /* no-op: always mocked when this file is linked */
}
void mock_http_disable(void)
{ /* no-op */
}

void mock_http_reset(void)
{
	for (int i = 0; i < g_mock_count; i++)
		free(g_mock_responses[i].body);
	for (int i = 0; i < g_mock_request_count; i++)
		free(g_mock_request_bodies[i]);
	g_mock_count = 0;
	g_mock_index = 0;
	g_mock_request_count = 0;
}

int mock_http_call_count(void)
{
	return g_mock_request_count;
}

const char *mock_http_request_body(int call_index)
{
	if (call_index < 0 || call_index >= g_mock_request_count)
		return NULL;
	return g_mock_request_bodies[call_index];
}

/* -------------------------------------------------------------------------
 * http_client.h interface - replaces the real curl implementation
 * ---------------------------------------------------------------------- */

int http_global_init(void)
{
	return 0;
}
void http_global_cleanup(void) {}

struct http_response http_post(const char *url,
			       const char *body,
			       const char *content_type,
			       const char *api_key,
			       int timeout_secs,
			       bool verify_ssl)
{
	(void)url;
	(void)content_type;
	(void)api_key;
	(void)timeout_secs;
	(void)verify_ssl;

	struct http_response resp;
	memset(&resp, 0, sizeof(resp));

	if (g_mock_request_count < MOCK_HTTP_MAX_QUEUED)
		g_mock_request_bodies[g_mock_request_count] = body ? strdup(body) : NULL;
	g_mock_request_count++;

	if (g_mock_index >= g_mock_count) {
		resp.error = strdup("No more mock responses queued");
		resp.status_code = 0;
		return resp;
	}

	struct mock_response *m = &g_mock_responses[g_mock_index++];
	if (m->is_error) {
		resp.error = strdup(m->body);
	} else {
		resp.body = strdup(m->body ? m->body : "");
		resp.body_length = resp.body ? strlen(resp.body) : 0;
		resp.status_code = m->status_code;
	}
	return resp;
}

void http_response_free(struct http_response *response)
{
	if (!response)
		return;
	free(response->body);
	free(response->error);
	response->body = NULL;
	response->error = NULL;
	response->body_length = 0;
	response->status_code = 0;
}
