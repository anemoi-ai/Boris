/*
 * trace.c - Record every model request and response to a JSONL file.
 *
 * See trace.h for the format. Bodies that parse as JSON are embedded as
 * JSON (so the file can be queried with jq); anything else, such as an SSE
 * stream, is embedded as a string.
 */

#define _POSIX_C_SOURCE 200809L

#include "trace.h"
#include "cJSON.h"
#include "logger.h"
#include <errno.h>
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static FILE *g_trace = NULL;
static int g_seq = 0;

int trace_open(const char *path)
{
	trace_close();
	if (!path || !path[0])
		return 0;

	int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (fd < 0) {
		log_warning("trace: cannot open %s: %s", path, strerror(errno));
		return -1;
	}
	g_trace = fdopen(fd, "a");
	if (!g_trace) {
		close(fd);
		return -1;
	}
	g_seq = 0;
	log_info("trace: recording requests to %s", path);
	return 0;
}

void trace_close(void)
{
	if (g_trace)
		fclose(g_trace);
	g_trace = NULL;
}

bool trace_enabled(void)
{
	return g_trace != NULL;
}

/* An object with "ts", "seq" and "kind" already set. */
static cJSON *trace_line(int seq, const char *kind)
{
	cJSON *line = cJSON_CreateObject();
	if (!line)
		return NULL;

	time_t secs;
	long millis;
#ifdef _WIN32
	struct timespec now;
	timespec_get(&now, TIME_UTC);
	secs = now.tv_sec;
	millis = now.tv_nsec / 1000000;
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	secs = tv.tv_sec;
	millis = (long)(tv.tv_usec / 1000);
#endif
	struct tm tm;
	char ts[40] = "";
	if (gmtime_r(&secs, &tm)) {
		size_t n = strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm);
		snprintf(ts + n, sizeof(ts) - n, ".%03ldZ", millis);
	}
	cJSON_AddStringToObject(line, "ts", ts);
	cJSON_AddNumberToObject(line, "seq", seq);
	cJSON_AddStringToObject(line, "kind", kind);
	return line;
}

/* Add body as parsed JSON when it is JSON, otherwise as a string. */
static void add_body(cJSON *line, const char *body, size_t len)
{
	if (!body) {
		cJSON_AddNullToObject(line, "body");
		return;
	}
	cJSON *parsed = cJSON_ParseWithLength(body, len);
	if (parsed) {
		cJSON_AddItemToObject(line, "body", parsed);
		return;
	}
	char *copy = malloc(len + 1);
	if (!copy)
		return;
	memcpy(copy, body, len);
	copy[len] = '\0';
	cJSON_AddStringToObject(line, "body", copy);
	free(copy);
}

static void write_line(cJSON *line)
{
	char *text = cJSON_PrintUnformatted(line);
	if (text) {
		fprintf(g_trace, "%s\n", text);
		fflush(g_trace);
		cJSON_free(text);
	}
	cJSON_Delete(line);
}

int trace_request(const char *url, const char *body)
{
	if (!g_trace)
		return 0;
	int seq = ++g_seq;
	cJSON *line = trace_line(seq, "request");
	if (!line)
		return seq;
	cJSON_AddStringToObject(line, "url", url ? url : "");
	add_body(line, body, body ? strlen(body) : 0);
	write_line(line);
	return seq;
}

void trace_response(int seq, long status, const char *body, size_t body_len,
		    const char *error, double elapsed_ms)
{
	if (!g_trace)
		return;
	cJSON *line = trace_line(seq, "response");
	if (!line)
		return;
	cJSON_AddNumberToObject(line, "status", (double)status);
	cJSON_AddNumberToObject(line, "elapsed_ms", (double)(long)(elapsed_ms * 10) / 10.0);
	add_body(line, body, body_len);
	if (error)
		cJSON_AddStringToObject(line, "error", error);
	else
		cJSON_AddNullToObject(line, "error");
	write_line(line);
}
