/*
 * tools/memory.c - Persistent key-value memory tool.
 *
 * Stores entries in a JSON file. Uses flock() for concurrent access safety.
 * Actions: get, set, delete, list.
 */

#define _POSIX_C_SOURCE 200809L

#include "tools.h"
#include "arena.h"
#include "configuration.h"
#include "json.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#include <libgen.h>

#define MEMORY_MAX_ENTRIES 500

static const char *resolve_memory_path(const struct agent_configuration *cfg,
				       struct memory_arena *scratch)
{
	if (cfg && cfg->memory_persist) {
		char buf[1024];
		const char *home = getenv("HOME");

		if (home && home[0]) {
			snprintf(buf, sizeof(buf),
				 "%s/.boris/memory/memory.json", home);
		} else {
			return NULL;
		}
		return arena_duplicate_string(scratch, buf);
	}
	return NULL;
}

static int ensure_dir(const char *filepath)
{
	/* Strip filename: find last separator */
	char path[1024];
	snprintf(path, sizeof(path), "%s", filepath);
	char *last = strrchr(path, '/');
	if (!last)
		return 0; /* no directory component */
	*last = '\0';

	struct stat st;
	if (stat(path, &st) == 0)
		return S_ISDIR(st.st_mode) ? 0 : -1;

	for (char *p = path + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(path, 0755) != 0 && errno != EEXIST)
				return -1;
			*p = '/';
		}
	}
	if (mkdir(path, 0755) != 0 && errno != EEXIST)
		return -1;

	return 0;
}

static cJSON *memory_load(const char *path)
{
	size_t len;
	char *buf = file_read_all(path, 10 * 1024 * 1024, &len);
	if (!buf) {
		if (errno == ENOENT)
			return cJSON_CreateObject();
		return NULL;
	}

	cJSON *root = json_parse(buf);
	free(buf);

	if (!root) {
		log_warning("memory: corrupted file, starting fresh");
		return cJSON_CreateObject();
	}

	if (!cJSON_IsObject(root)) {
		cJSON_Delete(root);
		return cJSON_CreateObject();
	}

	return root;
}

static int memory_save(const char *path, cJSON *root)
{
	if (ensure_dir(path) != 0)
		return -1;

	char *json = cJSON_PrintUnformatted(root);
	if (!json)
		return -1;

	char tmp[1024];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);

	FILE *f = fopen(tmp, "w");
	if (!f) {
		free(json);
		return -1;
	}

	int fd = fileno(f);
	if (flock(fd, LOCK_EX) != 0) {
		fclose(f);
		free(json);
		return -1;
	}

	size_t len = strlen(json);
	size_t written = fwrite(json, 1, len, f);
	if (written != len) {
		flock(fd, LOCK_UN);
		fclose(f);
		free(json);
		unlink(tmp);
		return -1;
	}

	fflush(f);
	fsync(fd);
	flock(fd, LOCK_UN);
	fclose(f);
	free(json);

	if (rename(tmp, path) != 0) {
		unlink(tmp);
		return -1;
	}

	return 0;
}

static struct tool_result action_get(const char *path, const char *key)
{
	if (!key || !key[0]) {
		return tool_result_errorf("memory",
					  "\"key\" is required for action=get");
	}

	cJSON *root = memory_load(path);
	if (!root) {
		return tool_result_errorf("memory", "Failed to load memory file");
	}

	cJSON *entry = cJSON_GetObjectItem(root, key);
	if (!entry || !cJSON_IsObject(entry)) {
		cJSON *result = cJSON_CreateObject();
		cJSON_AddBoolToObject(result, "found", false);
		cJSON_AddStringToObject(result, "key", key);
		char *out = cJSON_PrintUnformatted(result);
		cJSON_Delete(result);
		cJSON_Delete(root);
		if (!out)
			return tool_result_errorf("memory", "Out of memory");
		return tool_result_ok("memory", out);
	}

	cJSON *val = cJSON_GetObjectItem(entry, "value");
	const char *value = val && cJSON_IsString(val) ? val->valuestring : "";

	cJSON *result = cJSON_CreateObject();
	cJSON_AddBoolToObject(result, "found", true);
	cJSON_AddStringToObject(result, "key", key);
	cJSON_AddStringToObject(result, "value", value);
	char *out = cJSON_PrintUnformatted(result);
	cJSON_Delete(result);
	cJSON_Delete(root);
	if (!out)
		return tool_result_errorf("memory", "Out of memory");
	return tool_result_ok("memory", out);
}

static struct tool_result action_set(const char *path, const cJSON *args)
{
	const char *key = json_get_str(args, "key");
	const char *value = json_get_str(args, "value");

	if (!key || !key[0]) {
		return tool_result_errorf("memory",
					  "\"key\" is required for action=set");
	}
	if (!value) {
		return tool_result_errorf("memory",
					  "\"value\" is required for action=set");
	}
	if (strlen(key) > 255) {
		return tool_result_errorf("memory", "Key too long (max 255 chars)");
	}

	cJSON *root = memory_load(path);
	if (!root) {
		return tool_result_errorf("memory", "Failed to load memory file");
	}

	cJSON *entry = cJSON_GetObjectItem(root, key);
	int is_new = !entry || !cJSON_IsObject(entry);

	if (!entry) {
		entry = cJSON_CreateObject();
		cJSON_AddItemToObject(root, key, entry);
	}

	cJSON *old_val = cJSON_GetObjectItem(entry, "value");
	if (old_val) {
		cJSON_SetValuestring(old_val, value);
	} else {
		cJSON_AddStringToObject(entry, "value", value);
	}

	time_t now = time(NULL);
	struct tm *tm = gmtime(&now);
	char ts[32];
	strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", tm);

	cJSON *updated = cJSON_GetObjectItem(entry, "updated");
	if (updated) {
		cJSON_SetValuestring(updated, ts);
	} else {
		cJSON_AddStringToObject(entry, "updated", ts);
	}

	if (is_new)
		cJSON_AddStringToObject(entry, "created", ts);

	int rc = memory_save(path, root);
	cJSON_Delete(root);

	if (rc != 0) {
		return tool_result_errorf("memory", "Failed to save memory file");
	}

	cJSON *result = cJSON_CreateObject();
	cJSON_AddBoolToObject(result, "ok", true);
	cJSON_AddStringToObject(result, "key", key);
	cJSON_AddStringToObject(result, "action", "set");
	cJSON_AddBoolToObject(result, "created", is_new);
	char *out = cJSON_PrintUnformatted(result);
	cJSON_Delete(result);
	if (!out)
		return tool_result_errorf("memory", "Out of memory");
	return tool_result_ok("memory", out);
}

static struct tool_result action_delete(const char *path, const char *key)
{
	if (!key || !key[0]) {
		return tool_result_errorf("memory",
					  "\"key\" is required for action=delete");
	}

	cJSON *root = memory_load(path);
	if (!root) {
		return tool_result_errorf("memory", "Failed to load memory file");
	}

	cJSON *entry = cJSON_GetObjectItem(root, key);
	if (!entry) {
		cJSON *result = cJSON_CreateObject();
		cJSON_AddBoolToObject(result, "ok", false);
		cJSON_AddStringToObject(result, "key", key);
		cJSON_AddStringToObject(result, "reason", "not found");
		char *out = cJSON_PrintUnformatted(result);
		cJSON_Delete(result);
		cJSON_Delete(root);
		if (!out)
			return tool_result_errorf("memory", "Out of memory");
		return tool_result_ok("memory", out);
	}

	cJSON_DeleteItemFromObject(root, key);
	int rc = memory_save(path, root);
	cJSON_Delete(root);

	if (rc != 0) {
		return tool_result_errorf("memory", "Failed to save memory file");
	}

	cJSON *result = cJSON_CreateObject();
	cJSON_AddBoolToObject(result, "ok", true);
	cJSON_AddStringToObject(result, "key", key);
	cJSON_AddStringToObject(result, "action", "delete");
	char *out = cJSON_PrintUnformatted(result);
	cJSON_Delete(result);
	if (!out)
		return tool_result_errorf("memory", "Out of memory");
	return tool_result_ok("memory", out);
}

static struct tool_result action_list(const char *path, const cJSON *args)
{
	int limit = json_get_int(args, "limit", 50);
	if (limit <= 0 || limit > MEMORY_MAX_ENTRIES)
		limit = 50;

	const char *prefix = json_get_str(args, "prefix");

	cJSON *root = memory_load(path);
	if (!root) {
		return tool_result_errorf("memory", "Failed to load memory file");
	}

	cJSON *result = cJSON_CreateObject();
	cJSON *entries = cJSON_CreateArray();
	int count = 0;

	cJSON *item = NULL;
	cJSON_ArrayForEach(item, root)
	{
		const char *key = item->string;
		if (!key || !cJSON_IsObject(item))
			continue;
		if (prefix && prefix[0]) {
			size_t plen = strlen(prefix);
			if (strlen(key) < plen ||
			    strncmp(key, prefix, plen) != 0)
				continue;
		}
		if (count >= limit)
			break;

		cJSON *entry = cJSON_CreateObject();
		cJSON_AddStringToObject(entry, "key", key);

		cJSON *val = cJSON_GetObjectItem(item, "value");
		if (val && cJSON_IsString(val))
			cJSON_AddStringToObject(entry, "value", val->valuestring);
		else
			cJSON_AddStringToObject(entry, "value", "");

		cJSON_AddItemToArray(entries, entry);
		count++;
	}

	cJSON_AddNumberToObject(result, "count", count);
	cJSON_AddItemToObject(result, "entries", entries);

	char *out = cJSON_PrintUnformatted(result);
	cJSON_Delete(result);
	cJSON_Delete(root);

	if (!out)
		return tool_result_errorf("memory", "Out of memory");
	return tool_result_ok("memory", out);
}

struct tool_result tool_memory_fn(const char *arguments_json,
				  const struct agent_configuration *cfg,
				  struct memory_arena *scratch)
{
	cJSON *args = json_parse(arguments_json);
	if (!args) {
		return tool_result_errorf("memory", "Invalid JSON arguments");
	}

	const char *action = json_get_str(args, "action");
	if (!action || !action[0]) {
		cJSON_Delete(args);
		return tool_result_errorf("memory",
					  "\"action\" is required: "
					  "get, set, delete, list");
	}

	/* Path is arena-allocated; no free() needed */
	const char *path = resolve_memory_path(cfg, scratch);
	if (!path) {
		cJSON_Delete(args);
		return tool_result_errorf("memory",
					  "Memory persistence not enabled. "
					  "Set memory_persist in config.");
	}

	struct tool_result result;

	if (strcmp(action, "get") == 0) {
		result = action_get(path, json_get_str(args, "key"));
	} else if (strcmp(action, "set") == 0) {
		result = action_set(path, args);
	} else if (strcmp(action, "delete") == 0) {
		result = action_delete(path, json_get_str(args, "key"));
	} else if (strcmp(action, "list") == 0) {
		result = action_list(path, args);
	} else {
		result = tool_result_errorf("memory",
					    "Unknown action '%s'. "
					    "Valid: get, set, delete, list",
					    action);
	}

	cJSON_Delete(args);
	return result;
}
