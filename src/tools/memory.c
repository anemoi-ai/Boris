/*
 * tools/memory.c - Persistent key-value memory tool.
 *
 * Stores entries in ~/.boris/memory/memory.json. Every action holds an
 * flock() on a sibling ".lock" file for its whole load -> modify -> save
 * cycle, so concurrent Boris processes cannot lose each other's updates.
 * Keys are case-sensitive. Actions: get, set, delete, list.
 */

#define _POSIX_C_SOURCE 200809L

#include "tools.h"
#include "arena.h"
#include "configuration.h"
#include "fs.h"
#include "json.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>

#define MEMORY_LIST_DEFAULT 50
#define MEMORY_LIST_MAX	    500
#define MEMORY_FILE_MAX	    (10 * 1024 * 1024)

/* Path of the store, or NULL if persistence is off or HOME is unknown. */
static const char *resolve_memory_path(const struct agent_configuration *cfg,
				       struct memory_arena *scratch)
{
	if (!cfg || !cfg->memory_persist)
		return NULL;
	char *path = fs_boris_path("memory/memory.json");
	const char *copy = path ? arena_duplicate_string(scratch, path) : NULL;
	free(path);
	return copy;
}

/* Take an exclusive (or shared) lock on "<path>.lock". Returns the fd, or -1. */
static int memory_lock(const char *path, bool exclusive)
{
	char lock_path[4096];
	if (snprintf(lock_path, sizeof(lock_path), "%s.lock", path) >= (int)sizeof(lock_path))
		return -1;
	if (fs_mkdirp_parent(lock_path, 0700) != 0)
		return -1;
	int fd = open(lock_path, O_RDWR | O_CREAT, 0600);
	if (fd < 0)
		return -1;
	while (flock(fd, exclusive ? LOCK_EX : LOCK_SH) != 0) {
		if (errno != EINTR) {
			close(fd);
			return -1;
		}
	}
	return fd;
}

static void memory_unlock(int fd)
{
	flock(fd, LOCK_UN);
	close(fd);
}

/*
 * Load the store. A missing or empty file is an empty store. A corrupt
 * file is moved aside to "<path>.corrupt" rather than silently discarded
 * by the next save. Returns NULL only if the file exists but cannot be read.
 */
static cJSON *memory_load(const char *path)
{
	struct stat st;
	if (stat(path, &st) != 0) {
		if (errno == ENOENT)
			return cJSON_CreateObject();
		return NULL;
	}
	if (st.st_size == 0)
		return cJSON_CreateObject();

	size_t len;
	char *buf = file_read_all(path, MEMORY_FILE_MAX, &len);
	if (!buf)
		return NULL;

	cJSON *root = json_parse(buf);
	free(buf);

	if (!root || !cJSON_IsObject(root)) {
		cJSON_Delete(root);
		char backup[4096];
		snprintf(backup, sizeof(backup), "%s.corrupt", path);
		log_warning("memory: %s is corrupted; moved to %s, starting fresh",
			    path, backup);
		rename(path, backup);
		return cJSON_CreateObject();
	}

	return root;
}

static int memory_save(const char *path, const cJSON *root)
{
	char *json = cJSON_PrintUnformatted(root);
	if (!json)
		return -1;
	int rc = fs_write_atomic(path, json, strlen(json), 0600, false);
	free(json);
	return rc;
}

/* Serialise a result object into a tool_result. Takes ownership of obj. */
static struct tool_result json_result(cJSON *obj)
{
	char *out = obj ? cJSON_PrintUnformatted(obj) : NULL;
	cJSON_Delete(obj);
	if (!out)
		return tool_result_errorf("memory", "Out of memory");
	return tool_result_ok("memory", out);
}

static struct tool_result action_get(cJSON *root, const char *key)
{
	if (!key || !key[0])
		return tool_result_errorf("memory", "\"key\" is required for action=get");

	const cJSON *entry = cJSON_GetObjectItemCaseSensitive(root, key);
	cJSON *result = cJSON_CreateObject();
	cJSON_AddStringToObject(result, "key", key);
	if (!cJSON_IsObject(entry)) {
		cJSON_AddBoolToObject(result, "found", false);
	} else {
		const char *value = json_get_str(entry, "value");
		cJSON_AddBoolToObject(result, "found", true);
		cJSON_AddStringToObject(result, "value", value ? value : "");
	}
	return json_result(result);
}

/* Sets *modified when root changes and must be saved. */
static struct tool_result action_set(cJSON *root, const cJSON *args, bool *modified)
{
	const char *key = json_get_str(args, "key");
	const char *value = json_get_str(args, "value");

	if (!key || !key[0])
		return tool_result_errorf("memory", "\"key\" is required for action=set");
	if (!value)
		return tool_result_errorf("memory", "\"value\" is required for action=set");
	if (strlen(key) > 255)
		return tool_result_errorf("memory", "Key too long (max 255 chars)");

	char ts[32];
	time_t now = time(NULL);
	struct tm tm;
	gmtime_r(&now, &tm);
	strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);

	cJSON *old = cJSON_GetObjectItemCaseSensitive(root, key);
	bool is_new = !cJSON_IsObject(old);
	const char *created = is_new ? ts : json_get_str(old, "created");

	/* Build a fresh entry and swap it in: simpler than patching fields,
	 * and it also repairs entries that are not objects. */
	cJSON *entry = cJSON_CreateObject();
	if (!entry)
		return tool_result_errorf("memory", "Out of memory");
	cJSON_AddStringToObject(entry, "value", value);
	cJSON_AddStringToObject(entry, "created", created ? created : ts);
	cJSON_AddStringToObject(entry, "updated", ts);

	if (old)
		cJSON_ReplaceItemInObjectCaseSensitive(root, key, entry);
	else
		cJSON_AddItemToObject(root, key, entry);
	*modified = true;

	cJSON *result = cJSON_CreateObject();
	cJSON_AddBoolToObject(result, "ok", true);
	cJSON_AddStringToObject(result, "key", key);
	cJSON_AddStringToObject(result, "action", "set");
	cJSON_AddBoolToObject(result, "created", is_new);
	return json_result(result);
}

static struct tool_result action_delete(cJSON *root, const char *key, bool *modified)
{
	if (!key || !key[0])
		return tool_result_errorf("memory", "\"key\" is required for action=delete");

	bool found = cJSON_GetObjectItemCaseSensitive(root, key) != NULL;
	if (found) {
		cJSON_DeleteItemFromObjectCaseSensitive(root, key);
		*modified = true;
	}

	cJSON *result = cJSON_CreateObject();
	cJSON_AddBoolToObject(result, "ok", found);
	cJSON_AddStringToObject(result, "key", key);
	if (found)
		cJSON_AddStringToObject(result, "action", "delete");
	else
		cJSON_AddStringToObject(result, "reason", "not found");
	return json_result(result);
}

static struct tool_result action_list(const cJSON *root, const cJSON *args)
{
	int limit = json_get_int(args, "limit", MEMORY_LIST_DEFAULT);
	if (limit <= 0 || limit > MEMORY_LIST_MAX)
		limit = MEMORY_LIST_DEFAULT;

	const char *prefix = json_get_str(args, "prefix");
	size_t plen = prefix ? strlen(prefix) : 0;

	cJSON *result = cJSON_CreateObject();
	cJSON *entries = cJSON_CreateArray();
	int count = 0;

	const cJSON *item = NULL;
	cJSON_ArrayForEach(item, root)
	{
		if (count >= limit)
			break;
		if (!item->string || !cJSON_IsObject(item))
			continue;
		if (plen && strncmp(item->string, prefix, plen) != 0)
			continue;

		const char *value = json_get_str(item, "value");
		cJSON *entry = cJSON_CreateObject();
		cJSON_AddStringToObject(entry, "key", item->string);
		cJSON_AddStringToObject(entry, "value", value ? value : "");
		cJSON_AddItemToArray(entries, entry);
		count++;
	}

	cJSON_AddNumberToObject(result, "count", count);
	cJSON_AddItemToObject(result, "entries", entries);
	return json_result(result);
}

struct tool_result tool_memory_fn(const char *arguments_json,
				  const struct agent_configuration *cfg,
				  struct memory_arena *scratch)
{
	cJSON *args = json_parse(arguments_json);
	if (!args)
		return tool_result_errorf("memory", "Invalid JSON arguments");

	const char *action = json_get_str(args, "action");
	if (!action || !action[0]) {
		cJSON_Delete(args);
		return tool_result_errorf("memory",
					  "\"action\" is required: get, set, delete, list");
	}

	bool writes = strcmp(action, "set") == 0 || strcmp(action, "delete") == 0;
	bool reads = strcmp(action, "get") == 0 || strcmp(action, "list") == 0;
	if (!writes && !reads) {
		struct tool_result r = tool_result_errorf(
			"memory", "Unknown action '%s'. Valid: get, set, delete, list",
			action);
		cJSON_Delete(args);
		return r;
	}

	/* Path is arena-allocated; no free() needed */
	const char *path = resolve_memory_path(cfg, scratch);
	if (!path) {
		cJSON_Delete(args);
		return tool_result_errorf("memory",
					  "Memory persistence not enabled. "
					  "Set memory_persist in config.");
	}

	int lock_fd = memory_lock(path, writes);
	if (lock_fd < 0) {
		cJSON_Delete(args);
		return tool_result_errorf("memory", "Cannot lock memory store: %s",
					  strerror(errno));
	}

	struct tool_result result;
	cJSON *root = memory_load(path);
	bool modified = false;

	if (!root)
		result = tool_result_errorf("memory", "Failed to load memory file");
	else if (strcmp(action, "get") == 0)
		result = action_get(root, json_get_str(args, "key"));
	else if (strcmp(action, "set") == 0)
		result = action_set(root, args, &modified);
	else if (strcmp(action, "delete") == 0)
		result = action_delete(root, json_get_str(args, "key"), &modified);
	else
		result = action_list(root, args);

	if (modified && memory_save(path, root) != 0) {
		tool_result_free(&result);
		result = tool_result_errorf("memory", "Failed to save memory file");
	}

	memory_unlock(lock_fd);
	cJSON_Delete(root);
	cJSON_Delete(args);
	return result;
}

/* "set favourite_colour", "list", "list prefix=proj_". */
void tool_memory_format(const char *argument_json, char *buf, size_t size)
{
	cJSON *root = json_parse(argument_json);
	const char *action = json_get_str(root, "action");
	const char *key = json_get_str(root, "key");
	const char *prefix = json_get_str(root, "prefix");
	if (!action)
		snprintf(buf, size, "(invalid arguments)");
	else if (key && key[0])
		snprintf(buf, size, "%s %s", action, key);
	else if (prefix && prefix[0])
		snprintf(buf, size, "%s prefix=%s", action, prefix);
	else
		snprintf(buf, size, "%s", action);
	cJSON_Delete(root);
}
