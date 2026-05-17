/*
 * logger.c - Level-filtered logger with optional file output.
 *
 * Each message is prefixed with a UTC timestamp and level label, then written
 * to the configured destination (stderr by default) and flushed immediately.
 */

#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <errno.h>

static enum log_level current_log_level = LOG_LEVEL_ERROR;
static FILE *log_output = NULL;
static bool logger_owns_file = false; /* Did we open the file ourselves? */

static const char *level_labels[] = {
	"DEBUG",
	"INFO",
	"WARN",
	"ERROR",
};

/* Write a [YYYY-MM-DD HH:MM:SS] timestamp prefix to output. */
static void write_timestamp(FILE *output)
{
	time_t now;
	struct tm time_info;
	char time_buffer[32];

	time(&now);
	gmtime_r(&now, &time_info);
	strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S",
		 &time_info);
	fprintf(output, "[%s] ", time_buffer);
}

/* Initialise the logger. Writes to file_path if set, otherwise stderr. */
void logger_initialize(enum log_level level, const char *file_path)
{
	/* Close any previous logger */
	logger_close();

	current_log_level = level;

	if (file_path) {
		log_output = fopen(file_path, "a");
		if (log_output)
			logger_owns_file = true;
		else
			fprintf(stderr, "[logger] Cannot open log file '%s': %s "
					"(falling back to stderr)\n",
				file_path, strerror(errno));
	}

	/* Fall back to stderr if no file or file couldn't be opened */
	if (!log_output)
		log_output = stderr;
}

/* Flush and close the log file if one was opened by logger_initialize(). */
void logger_close(void)
{
	if (logger_owns_file && log_output && log_output != stderr) {
		fclose(log_output);
		logger_owns_file = false;
	}
	log_output = NULL;
}

/* Internal: filter, format, and write a log line. All public wrappers call this. */
static void log_message(enum log_level level, const char *format, va_list arguments)
{
	/* Skip messages below the current level */
	if (level < current_log_level)
		return;

	if (!log_output)
		return;

	write_timestamp(log_output);
	if (level < 0 || level >= (int)(sizeof(level_labels) / sizeof(level_labels[0])))
		fprintf(log_output, "%-5s ", "?????");
	else
		fprintf(log_output, "%-5s ", level_labels[level]);
	vfprintf(log_output, format, arguments);
	fprintf(log_output, "\n");

	fflush(log_output);
}

void log_debug(const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	log_message(LOG_LEVEL_DEBUG, format, arguments);
	va_end(arguments);
}

void log_info(const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	log_message(LOG_LEVEL_INFO, format, arguments);
	va_end(arguments);
}

void log_warning(const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	log_message(LOG_LEVEL_WARNING, format, arguments);
	va_end(arguments);
}

void log_error(const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	log_message(LOG_LEVEL_ERROR, format, arguments);
	va_end(arguments);
}

int log_level_from_string(const char *name, int default_val)
{
	if (!name || !name[0])
		return default_val;
	if (strcmp(name, "debug") == 0)
		return LOG_LEVEL_DEBUG;
	if (strcmp(name, "info") == 0)
		return LOG_LEVEL_INFO;
	if (strcmp(name, "warn") == 0 || strcmp(name, "warning") == 0)
		return LOG_LEVEL_WARNING;
	if (strcmp(name, "error") == 0)
		return LOG_LEVEL_ERROR;
	char *end;
	long val = strtol(name, &end, 10);
	if (end != name && *end == '\0')
		return (int)val;
	return default_val;
}
