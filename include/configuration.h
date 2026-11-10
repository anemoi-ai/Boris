/*
 * configuration.h - Layered configuration loader interface.
 *
 * Settings are applied in priority order:
 *   1. compiled-in defaults
 *   2. INI file (~/.boris/config.ini, then --config FILE)
 *   3. environment variables
 *   4. command-line flags (applied by the caller)
 */

#ifndef CONFIGURATION_H
#define CONFIGURATION_H

#include "boris_types.h"

/* System prompt used when none is configured. */
#define BORIS_DEFAULT_SYSTEM_PROMPT                                  \
	"You are Boris, a helpful AI companion. You are patient, "   \
	"curious, and enjoy learning alongside your human. You "      \
	"explain things clearly and encourage exploration."

/*
 * Populate config with compiled-in defaults.
 * Must be called before any other configuration function.
 */
void configuration_set_defaults(struct agent_configuration *config);

/*
 * Load configuration from an INI file and apply it to config.
 *
 * A missing file is treated as empty (not an error).
 * Returns BORIS_OK on success or an error code on parse failure.
 */
enum boris_error configuration_load_from_file(struct agent_configuration *config,
					      const char *file_path);

/*
 * Override config fields from recognized environment variables.
 * See configuration.c for the full variable list.
 */
void configuration_apply_environment(struct agent_configuration *config);

/*
 * Set a single configuration key-value pair.
 *
 * Key format is "section.key" (e.g., "model.temperature").
 * Used by CLI --set and internally by the INI loader.
 * Returns BORIS_OK if the key is recognized and the value valid, otherwise
 * BORIS_ERROR_BAD_ARGUMENT (the field keeps its previous value). Path keys
 * (sandbox_root, system_prompt_file, logging.file) expand a leading ~.
 */
enum boris_error configuration_set_key(struct agent_configuration *config,
				       const char *key, const char *value);

/*
 * Free all heap strings owned by config.
 * The config struct itself is not freed (it is usually stack-allocated).
 */
void configuration_destroy(struct agent_configuration *config);

/*
 * Parse a comma-separated tool list into a tools_enabled bitmask.
 * Recognizes: read, write, list_dir, memory, run, all, none. Unknown
 * names are ignored with a warning.
 * Returns TOOL_NONE if value is NULL or empty.
 */
unsigned int configuration_parse_tools_mask(const char *value);

/*
 * Warn (through the logger) about settings that are valid one by one but
 * do not make sense together, such as max_tokens >= context_window.
 */
void configuration_check(const struct agent_configuration *config);

#endif /* CONFIGURATION_H */
