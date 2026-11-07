/*
 * trace.h - Record every model request and response to a JSONL file.
 *
 * Enabled with --trace FILE, logging.trace_file or BORIS_TRACE_FILE. Each
 * request and each response is one JSON line:
 *
 *   {"ts":"2026-11-07T18:00:44.123Z","seq":1,"kind":"request",
 *    "url":"...","body":{...the request JSON...}}
 *   {"ts":"...","seq":1,"kind":"response","status":200,"elapsed_ms":812.4,
 *    "body":{...} or "raw text (SSE)","error":null}
 *
 * Headers are never written, so the API key never reaches the file. The
 * file holds the whole conversation, so it is created with mode 0600.
 * evals/trace_prefix.py reads it to check prompt-prefix stability.
 */

#ifndef TRACE_H
#define TRACE_H

#include <stdbool.h>
#include <stddef.h>

/* Start appending to path; NULL or "" leaves tracing off. Returns 0 or -1. */
int trace_open(const char *path);

/* Stop tracing and close the file. Safe to call when tracing is off. */
void trace_close(void);

bool trace_enabled(void);

/* Record a request about to be sent. Returns its sequence number (0 when
 * tracing is off), to pass to trace_response(). */
int trace_request(const char *url, const char *body);

/* Record the outcome of request seq: a status and body, or a transport error. */
void trace_response(int seq, long status, const char *body, size_t body_len,
		    const char *error, double elapsed_ms);

#endif /* TRACE_H */
