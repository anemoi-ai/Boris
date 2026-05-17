/*
 * metrics.h - Observability data about Boris's operation.
 *
 * Tracks request counts, latency, token usage, tool calls, and errors.
 * Printed at exit with --show-metrics or queried via /status.
 *
 * Thread safety: not thread-safe. Designed for single-threaded REPL use.
 */

#ifndef METRICS_H
#define METRICS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Global metrics counters.
 *
 * Updated by llm.c (requests, tokens, errors) and agent.c (tool calls).
 * Reset with metrics_reset().
 */
struct metrics {
	int request_count;
	double total_latency_ms;
	double max_latency_ms;
	int total_prompt_tokens;
	int total_completion_tokens;
	int tool_call_count;
	int error_count;
	int retry_count;
};

/* Get a pointer to the global metrics struct */
struct metrics *metrics_get(void);

/* Reset all counters to zero */
void metrics_reset(void);

/* Record a completed LLM request */
void metrics_record_request(double latency_ms, int prompt_tokens,
			    int completion_tokens, bool had_error);

/* Record a tool call */
void metrics_record_tool_call(void);

/* Record an error */
void metrics_record_error(void);

/* Record a retry attempt */
void metrics_record_retry(void);

/* Print a human-readable metrics summary */
void metrics_print_summary(void);

#ifdef __cplusplus
}
#endif

#endif /* METRICS_H */
