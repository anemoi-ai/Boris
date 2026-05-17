/*
 * metrics.c - Observability data about Boris's operation.
 */

#define _POSIX_C_SOURCE 200809L

#include "metrics.h"
#include <stdio.h>
#include <string.h>

static struct metrics g_metrics;

struct metrics *metrics_get(void)
{
	return &g_metrics;
}

void metrics_reset(void)
{
	memset(&g_metrics, 0, sizeof(g_metrics));
}

void metrics_record_request(double latency_ms, int prompt_tokens,
			    int completion_tokens, bool had_error)
{
	g_metrics.request_count++;
	g_metrics.total_latency_ms += latency_ms;
	if (latency_ms > g_metrics.max_latency_ms)
		g_metrics.max_latency_ms = latency_ms;
	g_metrics.total_prompt_tokens += prompt_tokens;
	g_metrics.total_completion_tokens += completion_tokens;
	if (had_error)
		g_metrics.error_count++;
}

void metrics_record_tool_call(void)
{
	g_metrics.tool_call_count++;
}

void metrics_record_error(void)
{
	g_metrics.error_count++;
}

void metrics_record_retry(void)
{
	g_metrics.retry_count++;
}

void metrics_print_summary(void)
{
	const struct metrics *m = &g_metrics;

	printf("\n");
	printf("  === Boris Metrics ===\n");
	printf("  Requests      : %d\n", m->request_count);
	if (m->request_count > 0) {
		printf("  Avg latency   : %.0f ms\n",
		       m->total_latency_ms / m->request_count);
		printf("  Max latency   : %.0f ms\n", m->max_latency_ms);
	}
	if (m->total_prompt_tokens > 0 || m->total_completion_tokens > 0) {
		printf("  Prompt tokens : %d\n", m->total_prompt_tokens);
		printf("  Output tokens : %d\n", m->total_completion_tokens);
		printf("  Total tokens  : %d\n",
		       m->total_prompt_tokens + m->total_completion_tokens);
	}
	printf("  Tool calls    : %d\n", m->tool_call_count);
	printf("  Errors        : %d\n", m->error_count);
	printf("  Retries       : %d\n", m->retry_count);
	printf("\n");
}
