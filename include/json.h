/*
 * json.h - Thin wrapper around cJSON for uniform error handling.
 *
 * Uses cJSON_ParseWithLength for bounded parsing and provides safe
 * accessors that return defaults on missing or wrong-typed keys.
 */

#ifndef JSON_H
#define JSON_H

#include "cJSON.h"

/*
 * Parse a NUL-terminated JSON string.
 *
 * On failure, logs an error and returns NULL.
 * The caller must cJSON_Delete() the result when done.
 */
cJSON *json_parse(const char *str);

/*
 * Parse a JSON string with a known length.
 *
 * Safer than json_parse() when you know the buffer size, since it
 * won't read past the specified length even if the string isn't
 * NUL-terminated.
 */
cJSON *json_parse_with_len(const char *str, size_t len);

/*
 * Return the string value of key in obj, or NULL.
 *
 * The returned pointer is owned by obj - do not free it.
 */
const char *json_get_str(const cJSON *obj, const char *key);

/*
 * Return the integer value of key, or def on failure.
 */
int json_get_int(const cJSON *obj, const char *key, int def);

/*
 * Return the double value of key, or def on failure.
 */
double json_get_double(const cJSON *obj, const char *key, double def);

/*
 * Return 1/0 for a boolean key, or def on failure.
 */
int json_get_bool(const cJSON *obj, const char *key, int def);

/*
 * Serialise obj to a compact (unformatted) JSON string.
 *
 * The caller must free() the returned string.
 * Returns NULL on OOM.
 */
char *json_to_str(const cJSON *obj);

/*
 * Serialise obj to a pretty-printed JSON string.
 *
 * The caller must free() the returned string.
 * Returns NULL on OOM.
 */
char *json_to_str_pretty(const cJSON *obj);

/*
 * Read an entire file into a heap-allocated, NUL-terminated buffer.
 *
 * Returns the buffer on success (caller must free()), or NULL on failure.
 * If out_len is non-NULL, the byte count (excluding NUL) is written there.
 * Files larger than max_bytes are rejected.
 */
char *file_read_all(const char *path, size_t max_bytes, size_t *out_len);

#endif /* JSON_H */
