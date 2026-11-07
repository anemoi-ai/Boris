/*
 * fs.c - Small filesystem helpers shared across the codebase.
 */

#define _POSIX_C_SOURCE 200809L

#include "fs.h"
#include "logger.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int fs_mkdirp(const char *dir, mode_t mode)
{
	if (!dir || dir[0] == '\0')
		return -1;

	char work[PATH_MAX];
	size_t len = strlen(dir);
	if (len >= sizeof(work))
		return -1;
	memcpy(work, dir, len + 1);

	/* Create each ancestor in turn, then the directory itself. */
	for (char *p = work + 1;; p++) {
		if (*p != '/' && *p != '\0')
			continue;
		char saved = *p;
		*p = '\0';
		if (mkdir(work, mode) != 0 && errno != EEXIST) {
			log_warning("fs_mkdirp: mkdir('%s') failed: %s",
				    work, strerror(errno));
			return -1;
		}
		*p = saved;
		if (saved == '\0')
			break;
	}

	struct stat st;
	return (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : -1;
}

int fs_mkdirp_parent(const char *file_path, mode_t mode)
{
	if (!file_path)
		return -1;
	const char *slash = strrchr(file_path, '/');
	if (!slash)
		return 0; /* relative file in the current directory */
	if (slash == file_path)
		return 0; /* parent is "/" */

	char parent[PATH_MAX];
	size_t len = (size_t)(slash - file_path);
	if (len >= sizeof(parent))
		return -1;
	memcpy(parent, file_path, len);
	parent[len] = '\0';
	return fs_mkdirp(parent, mode);
}

int fs_write_all(int fd, const char *data, size_t len)
{
	size_t done = 0;
	while (done < len) {
		ssize_t n = write(fd, data + done, len - done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		done += (size_t)n;
	}
	return 0;
}

int fs_write_atomic(const char *path, const char *data, size_t len,
		    mode_t mode, bool no_replace)
{
	char tmp[PATH_MAX];
	if (!path || snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path) >= (int)sizeof(tmp)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	/* An existing file keeps its permissions (e.g. an executable script). */
	struct stat st;
	if (stat(path, &st) == 0) {
		if (no_replace) {
			errno = EEXIST;
			return -1;
		}
		mode = st.st_mode & 07777;
	}

	int fd = mkstemp(tmp); /* O_CREAT|O_EXCL: never follows a planted symlink */
	if (fd < 0)
		return -1;

	int saved_errno = 0;
	if (fchmod(fd, mode) != 0 || fs_write_all(fd, data, len) != 0 ||
	    fsync(fd) != 0)
		saved_errno = errno;
	if (close(fd) != 0 && !saved_errno)
		saved_errno = errno;

	if (!saved_errno) {
		/* link() fails with EEXIST instead of replacing; rename() replaces. */
		int rc = no_replace ? link(tmp, path) : rename(tmp, path);
		if (rc != 0)
			saved_errno = errno;
	}

	if (saved_errno || no_replace)
		unlink(tmp);
	if (saved_errno) {
		errno = saved_errno;
		return -1;
	}
	return 0;
}

char *fs_boris_path(const char *relative)
{
	const char *home = getenv("HOME");
	if (!home || !home[0]) {
		struct passwd *pw = getpwuid(getuid());
		home = (pw && pw->pw_dir && pw->pw_dir[0]) ? pw->pw_dir : NULL;
	}
	if (!home)
		return NULL;

	bool has_rel = relative && relative[0];
	size_t size = strlen(home) + strlen("/.boris/") + (has_rel ? strlen(relative) : 0) + 1;
	char *path = malloc(size);
	if (!path)
		return NULL;
	snprintf(path, size, "%s/.boris%s%s", home, has_rel ? "/" : "",
		 has_rel ? relative : "");
	return path;
}
