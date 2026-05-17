/*
 * configuration.h - Layered configuration loader interface.
 *
 * Settings are applied in priority order:
 *   1. compiled-in defaults
 *   2. INI file (~/.boris/config.ini)
 *   3. environment variables
 *   4. command-line flags (applied by the caller)
 */

#ifndef CONFIGURATION_H
#define CONFIGURATION_H

#include "boris_types.h"

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
 * Returns BORIS_OK if the key is recognized, BORIS_ERROR_BAD_ARGUMENT otherwise.
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
 * Recognizes: read, write, bash, web, list_dir, memory, run, all, none.
 * Returns TOOL_NONE if value is NULL or empty.
 */
unsigned int configuration_parse_tools_mask(const char *value);

#endif /* CONFIGURATION_H */
