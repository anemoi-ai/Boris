/*
 * fs.h - Small filesystem helpers shared across the codebase.
 *
 * Directory creation, crash-safe file replacement, and the location of
 * Boris's per-user data directory (~/.boris).
 */

#ifndef FS_H
#define FS_H

#include <stdbool.h>
#include <stddef.h>
#include "platform.h"

/* Create dir and any missing parents (like mkdir -p). Returns 0 or -1. */
int fs_mkdirp(const char *dir, mode_t mode);

/* Create the directory that will contain file_path. Returns 0 or -1. */
int fs_mkdirp_parent(const char *file_path, mode_t mode);

/* Write all len bytes to fd, retrying short writes. Returns 0 or -1. */
int fs_write_all(int fd, const char *data, size_t len);

/*
 * Replace path with data atomically: write a uniquely named temp file in
 * the same directory, fsync it, then rename it over path. Readers see the
 * old or the new content, never a partial file.
 *
 * mode is used for a new file; an existing file keeps its permission bits.
 * With no_replace, an existing path is left untouched and the call fails
 * with errno == EEXIST (the check and the creation are a single step).
 *
 * Returns 0, or -1 with errno set.
 */
int fs_write_atomic(const char *path, const char *data, size_t len,
		    mode_t mode, bool no_replace);

/*
 * fs_write_atomic(), refusing (errno EACCES, nothing written) if the file
 * turns out not to be inside directory root once it is open. A path is
 * checked as a string before it is used; between the two, a directory in it
 * could be replaced by a link. Asking the open file where it is closes that
 * gap for the file's contents. root == NULL skips the check.
 */
int fs_write_atomic_in(const char *root, const char *path, const char *data, size_t len,
		       mode_t mode, bool no_replace);

/* Is the open file fd inside directory root? 1 yes, 0 no, -1 if the system
 * cannot tell (treat as "unknown", not as "no"). */
int fs_fd_is_inside(const char *root, int fd);

/*
 * Return "$HOME/.boris/<relative>" (or "$HOME/.boris" when relative is
 * NULL or empty) as a heap string. Falls back to the passwd entry when
 * HOME is unset (on Windows, to %USERPROFILE%). Returns NULL if no home
 * directory can be found.
 */
char *fs_boris_path(const char *relative);

#endif /* FS_H */
