/*
 * configuration.c - Layered configuration loader.
 *
 * Settings are applied in priority order:
 *   1. compiled-in defaults
 *   2. INI file (~/.boris/config.ini)
 *   3. environment variables
 *   (command-line flags are applied by the caller on top)
 *
 * The INI parser supports [sections], key = value pairs, and # / ;
 * comments. Section + key are joined as "section.key" before dispatch
 * to configuration_set_key().
 */

/* Enable POSIX functions like strdup() in strict C11 mode */
#define _POSIX_C_SOURCE 200809L

#include "configuration.h"
#include "boris_errors.h"
#include "boris_types.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

#define FREE_AND_NULL(p)    \
	do {                	\
		free(p);    		\
		(p) = NULL; 		\
	} while (0)

#define SET_STRING_FIELD(field, value)            \
	do {                                          \
		char *_tmp = strdup(value);               \
		if (!_tmp)                                \
			return BORIS_ERROR_OUT_OF_MEMORY; 	  \
		free(config->field);                      \
		config->field = _tmp;                     \
	} while (0)

#define ENV_SET_STRING(field, value)          	  \
	do {                                  		  \
		char *_tmp = strdup(value);   			  \
		if (_tmp) {                   	          \
			free(config->field);  				  \
			config->field = _tmp; 				  \
		}                             			  \
	} while (0)

static char *expand_tilde(const char *path)
{
	if (!path || path[0] != '~')
		return strdup(path);

	const char *home = getenv("HOME");
	if (!home || !home[0])
		return strdup(path);

	size_t home_len = strlen(home);
	size_t rest_len = strlen(path + 1);
	char *expanded = malloc(home_len + rest_len + 1);
	if (!expanded)
		return NULL;
	memcpy(expanded, home, home_len);
	memcpy(expanded + home_len, path + 1, rest_len + 1);
	return expanded;
}

static int parse_int(const char *str, int default_val)
{
	char *end = NULL;
	if (!str || !str[0])
		return default_val;
	errno = 0;
	long val = strtol(str, &end, 10);
	if (end == str || *end != '\0' || errno != 0)
		return default_val;
	if (val > INT_MAX)
		val = INT_MAX;
	if (val < INT_MIN)
		val = INT_MIN;
	return (int)val;
}

static bool parse_bool(const char *value)
{
	if (!value)
		return false;
	if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 ||
	    strcmp(value, "yes") == 0)
		return true;
	if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0 ||
	    strcmp(value, "no") == 0)
		return false;
	return false;
}

static char *trim_whitespace(char *string)
{
	char *end;

	/* Skip leading whitespace */
	while (isspace((unsigned char)*string))
		string++;

	/* All spaces? */
	if (*string == '\0')
		return string;

	/* Trim trailing whitespace */
	end = string + strlen(string) - 1;
	while (end > string && isspace((unsigned char)*end))
		end--;

	/* Null-terminate after the last non-space character */
	end[1] = '\0';

	return string;
}

unsigned int configuration_parse_tools_mask(const char *value)
{
	if (!value || !value[0])
		return TOOL_NONE;

	unsigned int mask = 0;
	char buf[512];
	snprintf(buf, sizeof(buf), "%s", value);
	char *tok = strtok(buf, ",");
	while (tok) {
		while (*tok == ' ')
			tok++;
		char *end = tok + strlen(tok) - 1;
		while (end > tok && *end == ' ')
			*end-- = '\0';
		if (strcmp(tok, "read") == 0)
			mask |= TOOL_READ;
		else if (strcmp(tok, "write") == 0)
			mask |= TOOL_WRITE;
		else if (strcmp(tok, "list_dir") == 0)
			mask |= TOOL_LIST_DIR;
		else if (strcmp(tok, "memory") == 0)
			mask |= TOOL_MEMORY;
		else if (strcmp(tok, "run") == 0)
			mask |= TOOL_RUN;
		else if (strcmp(tok, "all") == 0)
			mask = TOOL_ALL;
		else if (strcmp(tok, "none") == 0)
			mask = TOOL_NONE;
		tok = strtok(NULL, ",");
	}
	return mask;
}

void configuration_set_defaults(struct agent_configuration *config)
{
	if (!config)
		return;

	memset(config, 0, sizeof(struct agent_configuration));

	config->temperature = 0.7f;
	config->max_tokens = 2048;
	config->context_window = 8192;
	config->max_conversation_turns = 32;
	config->request_timeout_seconds = 120;

	config->run_timeout_seconds = 30;

	config->max_iterations = 16;
	config->text_tool_fallback = true; /* parse JSON tool calls embedded in plain text,
					      for models without native tool_calls support */
	config->max_retries = 2;
	config->retry_backoff_ms = 1000;

	config->verify_ssl = true;

	config->stream_responses = false;
	config->confirm_writes = false;
	config->confirm_bash = false;
	config->json_output = false;
	config->memory_persist = false;
	config->show_metrics = false;

	config->log_level = 1; /* LOG_LEVEL_INFO */
	config->log_format = strdup("text");
	if (!config->log_format)
		log_warning("configuration: failed to set default log_format");
}

enum boris_error configuration_set_key(struct agent_configuration *config,
				       const char *key, const char *value)
{

	if (strcmp(key, "model.endpoint") == 0) {
		SET_STRING_FIELD(model_endpoint, value);
	} else if (strcmp(key, "model.name") == 0) {
		SET_STRING_FIELD(model_name, value);
	} else if (strcmp(key, "model.api_key") == 0) {
		SET_STRING_FIELD(api_key, value);
	} else if (strcmp(key, "model.temperature") == 0) {
		config->temperature = strtof(value, NULL);
	} else if (strcmp(key, "model.max_tokens") == 0) {
		config->max_tokens = parse_int(value, config->max_tokens);
	} else if (strcmp(key, "model.context_window") == 0) {
		config->context_window = parse_int(value, config->context_window);
	} else if (strcmp(key, "model.timeout_seconds") == 0) {
		config->request_timeout_seconds = parse_int(value, config->request_timeout_seconds);
	} else if (strcmp(key, "model.stream") == 0) {
		config->stream_responses = parse_bool(value);
	} else if (strcmp(key, "model.verify_ssl") == 0) {
		config->verify_ssl = parse_bool(value);
	} else if (strcmp(key, "agent.max_turns") == 0) {
		config->max_conversation_turns = parse_int(value, config->max_conversation_turns);
	} else if (strcmp(key, "prompt.system_prompt_file") == 0) {
		SET_STRING_FIELD(system_prompt_file, value);
	} else if (strcmp(key, "prompt.system_prompt") == 0) {
		SET_STRING_FIELD(system_prompt, value);
	} else if (strcmp(key, "tools.sandbox_root") == 0) {
		char *expanded = expand_tilde(value);
		if (!expanded)
			return BORIS_ERROR_OUT_OF_MEMORY;
		free(config->sandbox_root);
		config->sandbox_root = expanded;
	} else if (strcmp(key, "tools.run_timeout") == 0) {
		config->run_timeout_seconds = parse_int(value,
							config->run_timeout_seconds);
	} else if (strcmp(key, "behaviour.confirm_writes") == 0) {
		config->confirm_writes = parse_bool(value);
	} else if (strcmp(key, "logging.level") == 0) {
		config->log_level = log_level_from_string(value, config->log_level);
	} else if (strcmp(key, "logging.file") == 0) {
		SET_STRING_FIELD(log_file, value);
	} else if (strcmp(key, "output.json") == 0) {
		config->json_output = parse_bool(value);
	} else if (strcmp(key, "model.model_path") == 0) {
		/* deprecated — ignored */
	} else if (strcmp(key, "agent.max_iterations") == 0) {
		config->max_iterations = parse_int(value, config->max_iterations);
	} else if (strcmp(key, "agent.text_tool_fallback") == 0) {
		config->text_tool_fallback = parse_bool(value);
	} else if (strcmp(key, "agent.max_retries") == 0) {
		config->max_retries = parse_int(value, config->max_retries);
	} else if (strcmp(key, "agent.retry_backoff_ms") == 0) {
		config->retry_backoff_ms = parse_int(value, config->retry_backoff_ms);
	} else if (strcmp(key, "tools.enabled") == 0) {
		config->tools_enabled = configuration_parse_tools_mask(value);
	} else if (strcmp(key, "tools.memory_persist") == 0) {
		config->memory_persist = parse_bool(value);
	} else if (strcmp(key, "behaviour.confirm_bash") == 0) {
		config->confirm_bash = parse_bool(value);
	} else if (strcmp(key, "logging.format") == 0) {
		SET_STRING_FIELD(log_format, value);
	} else if (strcmp(key, "output.show_metrics") == 0) {
		config->show_metrics = parse_bool(value);
	} else {
		log_warning("Unknown configuration key: %s", key);
		return BORIS_ERROR_BAD_ARGUMENT;
	}

	return BORIS_OK;
}

/*
 * Parse an INI file and apply each key-value pair to config.
 * Missing file is treated as empty (not an error).
 */
enum boris_error configuration_load_from_file(struct agent_configuration *config,
					      const char *file_path)
{
	FILE *file;
	char line[1024];
	char current_section[256];
	int line_number = 0;

	if (!config || !file_path)
		return BORIS_ERROR_BAD_ARGUMENT;

	file = fopen(file_path, "r");
	if (!file) {
		log_warning("Configuration file not found: %s (using defaults)", file_path);
		return BORIS_OK; /* Not having a config file is OK */
	}

	current_section[0] = '\0'; /* Start with no section */

	while (fgets(line, sizeof(line), file)) {
		char *trimmed;
		char *key;
		char *value;

		line_number++;

		/* Remove the newline at the end */
		line[strcspn(line, "\n")] = '\0';

		trimmed = trim_whitespace(line);

		/* Skip empty lines and comments */
		if (trimmed[0] == '\0' || trimmed[0] == '#' || trimmed[0] == ';')
			continue;

		/* Section header: [section] */
		if (trimmed[0] == '[') {
			char *close_bracket = strchr(trimmed, ']');
			if (close_bracket) {
				*close_bracket = '\0';
				snprintf(current_section, sizeof(current_section), "%s",
					 trimmed + 1);
				trim_whitespace(current_section);
			}
			continue;
		}

		/* Key-value pair: key = value */
		value = strchr(trimmed, '=');
		if (!value) {
			log_warning("Skipping malformed line %d in config: %s",
				    line_number, trimmed);
			continue;
		}

		/* Split on the '=' sign */
		*value = '\0';
		value++;

		key = trim_whitespace(trimmed);
		value = trim_whitespace(value);

		/* Build the full key: "section.key" */
		if (current_section[0] != '\0') {
			char full_key[512];
			snprintf(full_key, sizeof(full_key), "%s.%s",
				 current_section, key);
			configuration_set_key(config, full_key, value);
		} else {
			/* No section - use the key as-is */
			configuration_set_key(config, key, value);
		}
	}

	fclose(file);
	log_info("Configuration loaded from: %s", file_path);

	return BORIS_OK;
}

/*
 * Override config fields from environment variables. Recognised variables:
 *   BORIS_MODEL_ENDPOINT, BORIS_MODEL_NAME, BORIS_API_KEY,
 *   BORIS_LOG_LEVEL, BORIS_LOG_FILE, BORIS_SANDBOX_ROOT,
 *   BORIS_LLM_MODE, BORIS_MODEL_VERIFY_SSL, BORIS_MAX_ITERATIONS,
 *   BORIS_MAX_RETRIES, BORIS_TOOLS, BORIS_MEMORY_PERSIST,
 *   BORIS_SHOW_METRICS
 */
void configuration_apply_environment(struct agent_configuration *config)
{
	const char *value;

	if (!config)
		return;

	value = getenv("BORIS_MODEL_ENDPOINT");
	if (value)
		ENV_SET_STRING(model_endpoint, value);

	value = getenv("BORIS_MODEL_NAME");
	if (value)
		ENV_SET_STRING(model_name, value);

	value = getenv("BORIS_API_KEY");
	if (value)
		ENV_SET_STRING(api_key, value);

	value = getenv("BORIS_LOG_LEVEL");
	if (value)
		config->log_level = log_level_from_string(value, config->log_level);

	value = getenv("BORIS_LOG_FILE");
	if (value)
		ENV_SET_STRING(log_file, value);

	value = getenv("BORIS_SANDBOX_ROOT");
	if (value) {
		char *expanded = expand_tilde(value);
		if (expanded) {
			free(config->sandbox_root);
			config->sandbox_root = expanded;
		}
	}

	value = getenv("BORIS_MODEL_VERIFY_SSL");
	if (value)
		config->verify_ssl = parse_bool(value);

	value = getenv("BORIS_MAX_ITERATIONS");
	if (value)
		config->max_iterations = parse_int(value, config->max_iterations);

	value = getenv("BORIS_MAX_RETRIES");
	if (value)
		config->max_retries = parse_int(value, config->max_retries);

	value = getenv("BORIS_TOOLS");
	if (value)
		config->tools_enabled = configuration_parse_tools_mask(value);

	value = getenv("BORIS_MEMORY_PERSIST");
	if (value)
		config->memory_persist = parse_bool(value);

	value = getenv("BORIS_SHOW_METRICS");
	if (value)
		config->show_metrics = parse_bool(value);
}

/* Free all heap strings owned by config. */
void configuration_destroy(struct agent_configuration *config)
{
	if (!config)
		return;

	FREE_AND_NULL(config->model_endpoint);
	FREE_AND_NULL(config->model_name);
	FREE_AND_NULL(config->api_key);
	FREE_AND_NULL(config->system_prompt);
	FREE_AND_NULL(config->system_prompt_file);
	FREE_AND_NULL(config->sandbox_root);
	FREE_AND_NULL(config->log_file);
	FREE_AND_NULL(config->log_format);
}
