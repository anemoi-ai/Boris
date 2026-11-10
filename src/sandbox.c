/*
 * sandbox.c - Filesystem path sandboxing for Boris.
 *
 * Provides sandbox_resolve(): canonicalise a path and verify it falls
 * within the configured sandbox root before any filesystem operation.
 *
 * Design goals:
 *   - Single, auditable code path for all path validation
 *   - No TOCTOU: resolve and check in one call
 *   - Correct behaviour for non-existent paths (needed by write tool)
 *   - Zero dynamic allocation: operates entirely on stack buffers
 */
#define _POSIX_C_SOURCE 200809L

#include "sandbox.h"
#include "fs.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include "platform.h"
#ifndef _WIN32
#include <libgen.h>
#endif

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static int path_is_too_long(const char *s, size_t max_len)
{
	return strnlen(s, max_len) >= max_len;
}

static int path_join(const char *base, const char *name,
		     char *buf, size_t buf_size)
{
	size_t base_len = strlen(base);
	size_t name_len = strlen(name);
	size_t need = base_len + 1 + name_len + 1;

	if (need > buf_size)
		return -1;

	memcpy(buf, base, base_len);
	size_t pos = base_len;
	if (pos > 0 && buf[pos - 1] != '/')
		buf[pos++] = '/';
	memcpy(buf + pos, name, name_len);
	pos += name_len;
	buf[pos] = '\0';
	return 0;
}

/*
 * safe_realpath - canonicalise path, handling non-existent final components.
 * Sets *exists_out to 1 if path exists, 0 if it doesn't but parent does.
 */
static int safe_realpath(const char *path, char *out, int *exists_out)
{
	if (realpath(path, out) != NULL) {
		if (exists_out)
			*exists_out = 1;
		return 0;
	}

	if (errno != ENOENT) {
		log_debug("sandbox: realpath('%s') failed: %s",
			  path, strerror(errno));
		return -1;
	}

	/* Path doesn't exist. Peel back to find existing ancestor. */
	char work[PATH_MAX];
	if (strlen(path) >= PATH_MAX) {
		log_warning("sandbox: path too long for safe_realpath");
		return -1;
	}
	strncpy(work, path, PATH_MAX - 1);
	work[PATH_MAX - 1] = '\0';

	char suffix[PATH_MAX];
	suffix[0] = '\0';
	size_t suffix_len = 0;
	char resolved_prefix[PATH_MAX];
	int found = 0;

	for (;;) {
		size_t work_len = strlen(work);
		if (work_len == 0)
			break;

		char *last_sep = strrchr(work, '/');
		if (!last_sep) {
			const char *component = work;
			size_t comp_len = work_len;

			if (strcmp(component, "..") == 0)
				return -1;

			if (suffix_len + 1 + comp_len + 1 > PATH_MAX)
				break;
			memmove(suffix + 1 + comp_len, suffix, suffix_len + 1);
			suffix[0] = '/';
			memcpy(suffix + 1, component, comp_len);
			suffix_len += 1 + comp_len;

			work[0] = '.';
			work[1] = '\0';
			if (realpath(work, resolved_prefix) != NULL)
				found = 1;
			break;
		}

		const char *component = last_sep + 1;
		size_t comp_len = strlen(component);

		if (comp_len == 0) {
			*last_sep = '\0';
			continue;
		}

		if (comp_len == 1 && component[0] == '.') {
			*last_sep = '\0';
			continue;
		}

		/*
		 * A ".." inside the non-existent tail cannot be resolved here:
		 * once the missing directories are created (write's create_dirs)
		 * the kernel would follow it, escaping any prefix check made on
		 * the unresolved string. Reject it outright.
		 */
		if (comp_len == 2 && component[0] == '.' && component[1] == '.') {
			log_warning("sandbox: '..' after non-existent component in '%s'",
				    path);
			return -1;
		}

		if (suffix_len + 1 + comp_len + 1 > PATH_MAX) {
			log_warning("sandbox: suffix overflow during realpath fallback");
			return -1;
		}
		memmove(suffix + 1 + comp_len, suffix, suffix_len + 1);
		suffix[0] = '/';
		memcpy(suffix + 1, component, comp_len);
		suffix_len += 1 + comp_len;

		/* Nothing left but the root: "/", or "C:/" on Windows. */
		size_t root_len = platform_path_root_length(work);
		if ((size_t)(last_sep - work) < root_len) {
			work[root_len] = '\0';
			if (realpath(work, resolved_prefix) != NULL)
				found = 1;
			break;
		}

		*last_sep = '\0';

		if (realpath(work, resolved_prefix) != NULL) {
			found = 1;
			break;
		}

		if (errno != ENOENT) {
			log_debug("sandbox: realpath('%s') failed during peel: %s",
				  work, strerror(errno));
			return -1;
		}
	}

	if (!found) {
		log_debug("sandbox: no existing ancestor for '%s'", path);
		return -1;
	}

	size_t prefix_len = strlen(resolved_prefix);
	if (prefix_len > 0 && resolved_prefix[prefix_len - 1] == '/')
		prefix_len--; /* a root ends in '/', and the suffix starts with one */
	size_t total = prefix_len + suffix_len + 1;
	if (total > PATH_MAX) {
		log_warning("sandbox: reconstructed path too long");
		return -1;
	}

	memcpy(out, resolved_prefix, prefix_len);
	memcpy(out + prefix_len, suffix, suffix_len);
	out[prefix_len + suffix_len] = '\0';

	if (exists_out)
		*exists_out = 0;
	return 0;
}

#ifdef _WIN32
/*
 * Win32 rewrites some names before the filesystem sees them, so a path
 * that looks harmless as a string can mean something else entirely. Refuse
 * the ones that cannot be reasoned about (path already uses '/'):
 *
 *   C:file, file:stream   drive-relative paths and alternate data streams
 *   //./x, //?/x          the device namespace
 *   NUL, CON, COM1.txt    device names, which exist in every directory
 *   "name." and "name "   trailing dots and spaces are stripped - so ".. "
 *                         is "..", which the checks below would not notice
 */
static int windows_name_is_device(const char *name, size_t len)
{
	static const char *const devices[] = {"CON", "PRN", "AUX", "NUL", "CONIN$",
					      "CONOUT$", "COM#", "LPT#"};
	/* The device is chosen by the part before the first dot or colon,
	 * ignoring trailing spaces: "nul.txt" and "NUL " are both NUL. */
	size_t base = 0;
	while (base < len && name[base] != '.' && name[base] != ':')
		base++;
	while (base > 0 && name[base - 1] == ' ')
		base--;

	for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
		const char *d = devices[i];
		if (strlen(d) != base)
			continue;
		size_t k = 0;
		while (k < base && (d[k] == '#' ? (name[k] >= '0' && name[k] <= '9')
						: (d[k] == (name[k] & ~0x20) || d[k] == name[k])))
			k++;
		if (k == base)
			return 1;
	}
	return 0;
}

static int windows_path_is_unsafe(const char *path)
{
	size_t root_len = platform_path_root_length(path);
	if (strncmp(path, "//./", 4) == 0 || strncmp(path, "//?/", 4) == 0)
		return 1;
	if (strchr(path + (root_len == 3 ? 2 : 0), ':'))
		return 1;

	for (const char *p = path + root_len; *p;) {
		size_t len = strcspn(p, "/");
		int dots_only = (len == 1 && p[0] == '.') ||
				(len == 2 && p[0] == '.' && p[1] == '.');
		if (len > 0 && !dots_only &&
		    (p[len - 1] == '.' || p[len - 1] == ' ' ||
		     windows_name_is_device(p, len)))
			return 1;
		p += len;
		while (*p == '/')
			p++;
	}
	return 0;
}
#endif

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

int sandbox_resolve(const char *root, const char *path,
		    char *out, size_t out_size)
{
	if (out && out_size > 0)
		memset(out, 0, out_size);

	if (!out || out_size < PATH_MAX) {
		log_error("sandbox_resolve: output buffer too small");
		return -1;
	}

	if (!path || path[0] == '\0') {
		log_warning("sandbox_resolve: empty path");
		return -1;
	}

	if (path_is_too_long(path, PATH_MAX)) {
		log_warning("sandbox_resolve: path too long");
		return -1;
	}

#ifdef _WIN32
	/* Work on a copy with one kind of separator. */
	char normalized[PATH_MAX];
	strcpy(normalized, path);
	platform_path_normalize(normalized);
	path = normalized;
	if (windows_path_is_unsafe(path)) {
		log_warning("sandbox_resolve: refusing special Windows path '%s'", path);
		return -1;
	}
#endif

	/* Sandboxing disabled */
	if (!root || root[0] == '\0') {
		log_debug("sandbox_resolve: sandboxing disabled, resolving '%s'", path);
		int exists;
		if (safe_realpath(path, out, &exists) != 0) {
			log_warning("sandbox_resolve: (unsandboxed) could not resolve '%s'", path);
			memset(out, 0, out_size);
			return -1;
		}
		return 0;
	}

	if (path_is_too_long(root, PATH_MAX)) {
		log_error("sandbox_resolve: sandbox root path too long");
		return -1;
	}

	/* Step 1: Canonicalise sandbox root, creating it if absent */
	struct stat root_stat;
	if (stat(root, &root_stat) != 0) {
		if (errno != ENOENT) {
			log_error("sandbox_resolve: cannot stat sandbox root '%s': %s",
				  root, strerror(errno));
			return -1;
		}
		if (fs_mkdirp(root, 0755) != 0) {
			log_error("sandbox_resolve: cannot create sandbox root '%s'",
				  root);
			return -1;
		}
		log_info("sandbox_resolve: created sandbox root '%s'", root);
	} else if (!S_ISDIR(root_stat.st_mode)) {
		log_error("sandbox_resolve: sandbox root '%s' is not a directory", root);
		return -1;
	}

	char abs_root[PATH_MAX];
	if (realpath(root, abs_root) == NULL) {
		log_error("sandbox_resolve: cannot resolve sandbox root '%s': %s",
			  root, strerror(errno));
		return -1;
	}

	/* Step 2: Construct candidate path */
	char candidate[PATH_MAX];
	int path_was_relative = 0;

	if (platform_path_root_length(path) > 0) {
		/*
		 * Absolute path: use as-is.
		 * The prefix check below will catch any escape.
		 */
		if (strlen(path) >= PATH_MAX) {
			log_warning("sandbox_resolve: absolute path too long");
			return -1;
		}
		strncpy(candidate, path, PATH_MAX - 1);
		candidate[PATH_MAX - 1] = '\0';
	} else {
		/*
		 * Relative path: join with the sandbox root so that relative paths
		 * are always interpreted as relative to the sandbox, not to the
		 * process CWD.
		 */
		path_was_relative = 1;
		if (path_join(abs_root, path, candidate, sizeof(candidate)) != 0) {
			log_warning("sandbox_resolve: path_join failed");
			return -1;
		}
	}

	/* Step 3: Canonicalise candidate */
	char abs_candidate[PATH_MAX];
	int path_exists = 0;

	if (safe_realpath(candidate, abs_candidate, &path_exists) != 0) {
		log_warning("sandbox_resolve: cannot resolve candidate '%s': %s",
			    candidate, strerror(errno));
		return -1;
	}

	/* Step 4: Prefix check */
	size_t root_len = strlen(abs_root);
	char required_prefix[PATH_MAX + 1];

	if (root_len + 1 >= sizeof(required_prefix)) {
		log_error("sandbox_resolve: root too long for prefix");
		return -1;
	}
	memcpy(required_prefix, abs_root, root_len);
	required_prefix[root_len] = '/';
	required_prefix[root_len + 1] = '\0';

	if (strlen(abs_candidate) < root_len + 1 || strncmp(abs_candidate, required_prefix, root_len + 1) != 0) {
		/*
		 * Special case: if the path was relative (e.g. ".") and resolved
		 * to the sandbox root itself, allow it. This is safe because a
		 * relative path cannot escape the sandbox.
		 */
		if (path_was_relative && strcmp(abs_candidate, abs_root) == 0) {
			/* Allow - fall through to success */
		} else {
			log_warning("sandbox_resolve: ESCAPE DETECTED: root='%s' resolved='%s'",
				    abs_root, abs_candidate);
			return -1;
		}
	}

	/*
	 * Reject absolute paths that resolve to the sandbox root itself
	 * (e.g. someone passing the root path directly). Relative paths
	 * that resolve to root are allowed above.
	 */
	if (!path_was_relative && strcmp(abs_candidate, abs_root) == 0) {
		log_warning("sandbox_resolve: path resolves to sandbox root itself");
		return -1;
	}

	if (path_is_too_long(abs_candidate, PATH_MAX)) {
		log_warning("sandbox_resolve: resolved path too long");
		return -1;
	}

	size_t resolved_len = strlen(abs_candidate);
	if (resolved_len + 1 > out_size) {
		log_warning("sandbox_resolve: resolved path too long for output buffer");
		return -1;
	}

	memcpy(out, abs_candidate, resolved_len + 1);
	log_debug("sandbox_resolve: OK '%s' -> '%s'", path, out);
	return 0;
}

