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



#endif /* SANDBOX_H */
