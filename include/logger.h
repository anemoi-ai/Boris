/*
 * logger.h - Level-filtered logger with optional file output.
 *
 * Four levels: DEBUG (0), INFO (1), WARNING (2), ERROR (3).
 * Messages below the configured threshold are discarded.
 */

#ifndef LOGGER_H
#define LOGGER_H

#include <stdarg.h>

enum log_level {
	LOG_LEVEL_DEBUG 	= 0,
	LOG_LEVEL_INFO 		= 1,
	LOG_LEVEL_WARNING 	= 2,
	LOG_LEVEL_ERROR 	= 3,
};

/*
 * Initialize the logger.
 * file_path: log destination; NULL writes to stderr.
 * Must be called before any log_* function.
 */
void logger_initialize(enum log_level level, const char *file_path);

/* Flush and close the log file (if open). */
void logger_close(void);

void log_debug(const char *format, ...);
void log_info(const char *format, ...);
void log_warning(const char *format, ...);
void log_error(const char *format, ...);

/*
 * Map a level name ("debug", "info", "warn", "error") or numeric string
 * to the corresponding enum value. Returns default_val for unknown input.
 */
int log_level_from_string(const char *name, int default_val);

#endif /* LOGGER_H */
