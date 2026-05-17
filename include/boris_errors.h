/*
 * boris_errors.h - Error code enumeration.
 *
 * Negative values indicate failure; zero (BORIS_OK) indicates success.
 * Human-readable strings are available via boris_error_description().
 */

#ifndef BORIS_ERRORS_H
#define BORIS_ERRORS_H

enum boris_error {
	BORIS_OK = 0,
	BORIS_ERROR_OUT_OF_MEMORY = -1,
	BORIS_ERROR_BAD_ARGUMENT = -2,
	BORIS_ERROR_IO = -3,
	BORIS_ERROR_TIMEOUT = -4,
	BORIS_ERROR_HTTP = -5,
	BORIS_ERROR_PARSE = -6,
	BORIS_ERROR_SANDBOX = -7,
	BORIS_ERROR_TOOL = -8,
	BORIS_ERROR_LLM = -9,
	BORIS_ERROR_CONFIG = -10,
	BORIS_ERROR_INTERRUPTED = -11,
};

/* Return a short human-readable description for an error code. */
static inline const char *boris_error_description(enum boris_error error)
{
	switch (error) {
	case BORIS_OK:
		return "OK";
	case BORIS_ERROR_OUT_OF_MEMORY:
		return "Out of memory";
	case BORIS_ERROR_BAD_ARGUMENT:
		return "Invalid argument";
	case BORIS_ERROR_IO:
		return "I/O error";
	case BORIS_ERROR_TIMEOUT:
		return "Operation timed out";
	case BORIS_ERROR_HTTP:
		return "HTTP request failed";
	case BORIS_ERROR_PARSE:
		return "Parse error";
	case BORIS_ERROR_SANDBOX:
		return "Sandbox violation";
	case BORIS_ERROR_TOOL:
		return "Tool error";
	case BORIS_ERROR_LLM:
		return "LLM error";
	case BORIS_ERROR_CONFIG:
		return "Configuration error";
	case BORIS_ERROR_INTERRUPTED:
		return "Interrupted";
	default:
		return "Unknown error";
	}
}

#endif /* BORIS_ERRORS_H */
