/*
 * sandbox.h - Filesystem path sandboxing for Boris.
 *
 * All file-touching tools (read, write, list_dir) must call sandbox_resolve()
 * before any filesystem operation and use ONLY the returned path for all
 * subsequent syscalls.
 *
 * This prevents directory traversal attacks like "../../etc/passwd".
 */

#ifndef SANDBOX_H
#define SANDBOX_H

#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * sandbox_resolve - canonicalise path and verify it falls within root.
 *
 * root     : Sandbox root directory. If NULL or empty, sandboxing is
 *            disabled but path is still canonicalised.
 * path     : Path to validate (absolute or relative).
 * out      : Output buffer for canonical absolute path.
 * out_size : Capacity of out (must be >= PATH_MAX).
 *
 * Returns 0 on success, -1 on failure.
 */
int sandbox_resolve(const char *root, const char *path,
		    char *out, size_t out_size);

/*
 * sandbox_mkdirp - create directory tree within a resolved path.
 * dir_path must already be sandbox-resolved.
 */
int sandbox_mkdirp(const char *dir_path, mode_t mode);

/*
 * sandbox_contains - check whether a canonical path is inside the sandbox.
 * Both root and resolved_path must be canonical absolute paths.
 */
int sandbox_contains(const char *root, const char *resolved_path);

#ifdef __cplusplus
}
#endif

#endif /* SANDBOX_H */
