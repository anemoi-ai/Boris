/*
 * configuration.c - Layered configuration loader.
 *
 * Settings are applied in priority order:
 *   1. compiled-in defaults
 *   2. INI file (~/.boris/config.ini, then any --config FILE)
 *   3. environment variables
 *   (command-line flags are applied by the caller on top)
 *
 * Every layer funnels through configuration_set_key(), so parsing,
 * validation and path expansion live in one place.
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
#include "tools.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "platform.h"
#include <errno.h>
#include <limits.h>

#define FREE_AND_NULL(p)    \
	do {                \
		free(p);    \
		(p) = NULL; \
	} while (0)

/* Expand a leading "~/" (or a bare "~") to $HOME. Caller frees. */
static char *expand_tilde(const char *path)
{
	bool separator = path[1] == '/' || (PLATFORM_WINDOWS && path[1] == '\\');
	if (path[0] != '~' || (path[1] != '\0' && !separator))
		return strdup(path);

	const char *home = platform_home_dir();
	if (!home)
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

/* Replace *field with a copy of value (tilde-expanded when is_path). */
static enum boris_error set_string(char **field, const char *value, bool is_path)
{
	char *copy = is_path ? expand_tilde(value) : strdup(value);
	if (!copy)
		return BORIS_ERROR_OUT_OF_MEMORY;
	if (is_path)
		platform_path_normalize(copy); /* Windows: one kind of slash */
	free(*field);
	*field = copy;
	return BORIS_OK;
}

/* Parse a decimal int into *field; on bad input warn and keep the old value. */
static enum boris_error set_int(int *field, const char *key, const char *value)
{
	char *end = NULL;
	errno = 0;
	long val = strtol(value, &end, 10);
	if (end == value || *end != '\0' || errno != 0 ||
	    val > INT_MAX || val < INT_MIN) {
		log_warning("configuration: %s: '%s' is not an integer (keeping %d)",
			    key, value, *field);
		return BORIS_ERROR_BAD_ARGUMENT;
	}
	*field = (int)val;
	return BORIS_OK;
}

static enum boris_error set_float(float *field, const char *key, const char *value)
{
	char *end = NULL;
	errno = 0;
	float val = strtof(value, &end);
	if (end == value || *end != '\0' || errno != 0) {
		log_warning("configuration: %s: '%s' is not a number (keeping %g)",
			    key, value, (double)*field);
		return BORIS_ERROR_BAD_ARGUMENT;
	}
	*field = val;
	return BORIS_OK;
}

/* Accepts true/false, yes/no, on/off, 1/0. Anything else keeps the old value:
 * silently turning "verify_ssl = ture" into false would be a security bug. */
static enum boris_error set_bool(bool *field, const char *key, const char *value)
{
	static const char *const yes[] = {"true", "yes", "on", "1"};
	static const char *const no[] = {"false", "no", "off", "0"};
	for (size_t i = 0; i < sizeof(yes) / sizeof(yes[0]); i++) {
		if (strcmp(value, yes[i]) == 0) {
			*field = true;
			return BORIS_OK;
		}
		if (strcmp(value, no[i]) == 0) {
			*field = false;
			return BORIS_OK;
		}
	}
	log_warning("configuration: %s: '%s' is not a boolean (keeping %s)",
		    key, value, *field ? "true" : "false");
	return BORIS_ERROR_BAD_ARGUMENT;
}

/*
 * The API key, as written or - "dpapi:<hex>", which is how the setup wizard
 * stores it on Windows - encrypted for the current Windows user.
 */
static enum boris_error set_api_key(struct agent_configuration *config, const char *value)
{
	if (strncmp(value, "dpapi:", 6) != 0)
		return set_string(&config->api_key, value, false);
#ifdef _WIN32
	char *secret = platform_secret_unprotect(value + 6);
	if (!secret) {
		log_warning("configuration: model.api_key could not be decrypted. It can "
			    "only be read by the Windows user who ran 'boris init', on "
			    "that machine; run it again or set BORIS_API_KEY.");
		return BORIS_ERROR_BAD_ARGUMENT;
	}
	free(config->api_key);
	config->api_key = secret;
	return BORIS_OK;
#else
	log_warning("configuration: model.api_key is encrypted for a Windows user and "
		    "cannot be read here; set BORIS_API_KEY or replace it.");
	return BORIS_ERROR_BAD_ARGUMENT;
#endif
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
	for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
		tok = trim_whitespace(tok);
		if (strcmp(tok, "all") == 0) {
			mask = TOOL_ALL;
		} else if (strcmp(tok, "default") == 0) {
			mask |= TOOL_DEFAULT;
		} else if (strcmp(tok, "none") == 0) {
			mask = TOOL_NONE;
		} else {
			unsigned int flag = tools_flag_from_name(tok);
			if (flag)
				mask |= flag;
			else if (tok[0])
				log_warning("configuration: unknown tool '%s' ignored", tok);
		}
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
	config->request_timeout_seconds = 120;

	config->run_timeout_seconds = 30;
	config->max_result_bytes = TOOL_RESULT_MAX_DEFAULT;

	config->max_iterations = 16;
	config->text_tool_fallback = true; /* parse JSON tool calls embedded in plain text,
					      for models without native tool_calls support */
	config->native_tool_reminder = true;
	config->max_retries = 2;
	config->retry_backoff_ms = 1000;

	config->verify_ssl = true;

	config->stream_responses = false;
	config->confirm_writes = false;
	config->confirm_bash = true; /* run executes code: ask unless told otherwise */
	config->run_confine = true;
	config->confirm_fetch = true;
	config->fetch_allow_private = false;
	config->memory_persist = false;
	config->show_metrics = false;
	config->show_tool_calls = true;

	config->log_level = 1; /* LOG_LEVEL_INFO */
}

enum boris_error configuration_set_key(struct agent_configuration *config,
				       const char *key, const char *value)
{
	if (!config || !key || !value)
		return BORIS_ERROR_BAD_ARGUMENT;

	/* [model] */
	if (strcmp(key, "model.endpoint") == 0)
		return set_string(&config->model_endpoint, value, false);
	if (strcmp(key, "model.name") == 0)
		return set_string(&config->model_name, value, false);
	if (strcmp(key, "model.api_key") == 0)
		return set_api_key(config, value);
	if (strcmp(key, "model.temperature") == 0)
		return set_float(&config->temperature, key, value);
	if (strcmp(key, "model.max_tokens") == 0)
		return set_int(&config->max_tokens, key, value);
	if (strcmp(key, "model.context_window") == 0)
		return set_int(&config->context_window, key, value);
	if (strcmp(key, "model.timeout_seconds") == 0)
		return set_int(&config->request_timeout_seconds, key, value);
	if (strcmp(key, "model.stream") == 0) {
		config->stream_set = true;
		return set_bool(&config->stream_responses, key, value);
	}
	if (strcmp(key, "model.verify_ssl") == 0)
		return set_bool(&config->verify_ssl, key, value);

	/* [prompt] */
	if (strcmp(key, "prompt.system_prompt_file") == 0)
		return set_string(&config->system_prompt_file, value, true);
	if (strcmp(key, "prompt.system_prompt") == 0)
		return set_string(&config->system_prompt, value, false);
	if (strcmp(key, "prompt.instructions") == 0)
		return set_string(&config->instructions_file, value, true);

	/* [agent] */
	if (strcmp(key, "agent.max_iterations") == 0)
		return set_int(&config->max_iterations, key, value);
	if (strcmp(key, "agent.text_tool_fallback") == 0)
		return set_bool(&config->text_tool_fallback, key, value);
	if (strcmp(key, "agent.native_tool_reminder") == 0)
		return set_bool(&config->native_tool_reminder, key, value);
	if (strcmp(key, "agent.max_retries") == 0)
		return set_int(&config->max_retries, key, value);
	if (strcmp(key, "agent.retry_backoff_ms") == 0)
		return set_int(&config->retry_backoff_ms, key, value);

	/* [tools] */
	if (strcmp(key, "tools.enabled") == 0) {
		config->tools_enabled = configuration_parse_tools_mask(value);
		return BORIS_OK;
	}
	if (strcmp(key, "tools.sandbox_root") == 0)
		return set_string(&config->sandbox_root, value, true);
	if (strcmp(key, "tools.run_timeout") == 0)
		return set_int(&config->run_timeout_seconds, key, value);
	if (strcmp(key, "tools.run_env") == 0)
		return set_string(&config->run_env, value, false);
	if (strcmp(key, "tools.run_confine") == 0)
		return set_bool(&config->run_confine, key, value);
	if (strcmp(key, "tools.run_read_paths") == 0)
		return set_string(&config->run_read_paths, value, false);
	if (strcmp(key, "tools.max_result_bytes") == 0)
		return set_int(&config->max_result_bytes, key, value);
	if (strcmp(key, "tools.memory_persist") == 0)
		return set_bool(&config->memory_persist, key, value);
	if (strcmp(key, "tools.fetch_allow_private") == 0)
		return set_bool(&config->fetch_allow_private, key, value);

	/* [behaviour] */
	if (strcmp(key, "behaviour.confirm_writes") == 0)
		return set_bool(&config->confirm_writes, key, value);
	if (strcmp(key, "behaviour.confirm_run") == 0 ||
	    strcmp(key, "behaviour.confirm_bash") == 0) /* older name */
		return set_bool(&config->confirm_bash, key, value);
	if (strcmp(key, "behaviour.confirm_fetch") == 0)
		return set_bool(&config->confirm_fetch, key, value);

	/* [logging] */
	if (strcmp(key, "logging.level") == 0) {
		config->log_level = log_level_from_string(value, config->log_level);
		return BORIS_OK;
	}
	if (strcmp(key, "logging.file") == 0)
		return set_string(&config->log_file, value, true);
	if (strcmp(key, "logging.trace_file") == 0)
		return set_string(&config->trace_file, value, true);

	/* [output] */
	if (strcmp(key, "output.show_metrics") == 0)
		return set_bool(&config->show_metrics, key, value);
	if (strcmp(key, "output.show_tool_calls") == 0)
		return set_bool(&config->show_tool_calls, key, value);

	/* Accepted for compatibility with older config files; no effect. */
	if (strcmp(key, "model.model_path") == 0 ||
	    strcmp(key, "agent.max_turns") == 0 ||
	    strcmp(key, "logging.format") == 0 ||
	    strcmp(key, "output.json") == 0) {
		log_debug("configuration: '%s' is no longer used", key);
		return BORIS_OK;
	}

	log_warning("Unknown configuration key: %s", key);
	return BORIS_ERROR_BAD_ARGUMENT;
}

/*
 * Parse an INI file and apply each key-value pair to config.
 * Missing file is treated as empty (not an error).
 */
enum boris_error configuration_load_from_file(struct agent_configuration *config,
					      const char *file_path)
{
	FILE *file;
	char line[4096];
	char current_section[256];
	int line_number = 0;

	if (!config || !file_path)
		return BORIS_ERROR_BAD_ARGUMENT;

	file = fopen(file_path, "r");
	if (!file) {
		log_info("Configuration file not found: %s (using defaults)", file_path);
		return BORIS_OK; /* Not having a config file is OK */
	}

	current_section[0] = '\0'; /* Start with no section */
	const char *key_before = config->api_key;

	while (fgets(line, sizeof(line), file)) {
		char *trimmed;
		char *key;
		char *value;

		line_number++;

		if (!strchr(line, '\n') && !feof(file)) {
			log_warning("%s:%d: line too long, skipped", file_path, line_number);
			int c;
			while ((c = fgetc(file)) != EOF && c != '\n')
				;
			continue;
		}

		/* Remove the newline at the end */
		line[strcspn(line, "\r\n")] = '\0';

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
					 trim_whitespace(trimmed + 1));
			}
			continue;
		}

		/* Key-value pair: key = value */
		value = strchr(trimmed, '=');
		if (!value) {
			log_warning("%s:%d: malformed line skipped: %s",
				    file_path, line_number, trimmed);
			continue;
		}

		/* Split on the '=' sign */
		*value = '\0';
		value++;

		key = trim_whitespace(trimmed);
		value = trim_whitespace(value);

		/* Build the full key: "section.key" */
		char full_key[512];
		if (current_section[0] != '\0')
			snprintf(full_key, sizeof(full_key), "%s.%s",
				 current_section, key);
		else
			snprintf(full_key, sizeof(full_key), "%s", key);

		if (configuration_set_key(config, full_key, value) != BORIS_OK)
			log_warning("%s:%d: could not apply '%s'",
				    file_path, line_number, full_key);
	}

	fclose(file);

#ifndef _WIN32
	/* A key in a file other users can read is not a secret. (ssh refuses
	 * such a file outright; a warning is enough here.) */
	struct stat st;
	if (config->api_key && config->api_key != key_before && stat(file_path, &st) == 0 &&
	    (st.st_mode & 077) != 0)
		log_warning("%s holds an API key and is readable by other users "
			    "(mode %03o). Run: chmod 600 %s",
			    file_path, (unsigned)(st.st_mode & 0777), file_path);
#else
	(void)key_before;
#endif
	log_info("Configuration loaded from: %s", file_path);

	return BORIS_OK;
}

void configuration_check(const struct agent_configuration *config)
{
	if (!config)
		return;
	/* The reply has to fit in the window together with the conversation.
	 * A max_tokens at or above the window leaves no room for either: the
	 * server truncates the prompt, or rejects the request outright. */
	if (config->context_window > 0 && config->max_tokens >= config->context_window)
		log_warning("configuration: model.max_tokens (%d) is not below "
			    "model.context_window (%d); the reply alone could fill the "
			    "window. Lower max_tokens, or raise context_window if the "
			    "model really has more.",
			    config->max_tokens, config->context_window);
	else if (config->context_window > 0 &&
		 config->max_tokens > config->context_window / 2)
		log_warning("configuration: model.max_tokens (%d) is more than half of "
			    "model.context_window (%d); little room is left for the "
			    "conversation.",
			    config->max_tokens, config->context_window);
}

/* Environment variable -> configuration key. */
static const struct {
	const char *env;
	const char *key;
} ENV_KEYS[] = {
	{"BORIS_MODEL_ENDPOINT", "model.endpoint"},
	{"BORIS_MODEL_NAME", "model.name"},
	{"BORIS_API_KEY", "model.api_key"},
	{"BORIS_MODEL_VERIFY_SSL", "model.verify_ssl"},
	{"BORIS_LOG_LEVEL", "logging.level"},
	{"BORIS_LOG_FILE", "logging.file"},
	{"BORIS_TRACE_FILE", "logging.trace_file"},
	{"BORIS_SANDBOX_ROOT", "tools.sandbox_root"},
	{"BORIS_TOOLS", "tools.enabled"},
	{"BORIS_MEMORY_PERSIST", "tools.memory_persist"},
	{"BORIS_MAX_ITERATIONS", "agent.max_iterations"},
	{"BORIS_MAX_RETRIES", "agent.max_retries"},
	{"BORIS_SHOW_METRICS", "output.show_metrics"},
};

void configuration_apply_environment(struct agent_configuration *config)
{
	if (!config)
		return;

	for (size_t i = 0; i < sizeof(ENV_KEYS) / sizeof(ENV_KEYS[0]); i++) {
		const char *value = getenv(ENV_KEYS[i].env);
		if (value)
			configuration_set_key(config, ENV_KEYS[i].key, value);
	}
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
	FREE_AND_NULL(config->instructions_file);
	FREE_AND_NULL(config->sandbox_root);
	FREE_AND_NULL(config->run_env);
	FREE_AND_NULL(config->run_read_paths);
	FREE_AND_NULL(config->log_file);
	FREE_AND_NULL(config->trace_file);
}
