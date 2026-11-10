/*
 * fs.c - Small filesystem helpers shared across the codebase.
 */

#define _POSIX_C_SOURCE 200809L

#include "fs.h"
#include "logger.h"
#include <errno.h>
#include "platform.h"
#include <limits.h>
#ifndef _WIN32
#include <pwd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int fs_mkdirp(const char *dir, mode_t mode)
{
	if (!dir || dir[0] == '\0')
		return -1;

	char work[PATH_MAX];
	size_t len = strlen(dir);
	if (len >= sizeof(work))
		return -1;
	memcpy(work, dir, len + 1);

	platform_path_normalize(work);

	/* Create each ancestor in turn, then the directory itself. The root
	 * ("/", "C:/", "//server/share/") exists and cannot be created. */
	size_t root = platform_path_root_length(work);
	bool is_root = root > 0 && work[root] == '\0';
	for (char *p = work + (root ? root : 1); !is_root; p++) {
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
	char parent[PATH_MAX];
	if (strlen(file_path) >= sizeof(parent))
		return -1;
	strcpy(parent, file_path);
	platform_path_normalize(parent);

	char *slash = strrchr(parent, '/');
	if (!slash)
		return 0; /* relative file in the current directory */
	if ((size_t)(slash - parent) < platform_path_root_length(parent))
		return 0; /* parent is the root */
	*slash = '\0';
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
	return fs_write_atomic_in(NULL, path, data, len, mode, no_replace);
}

/* Is the open file fd inside directory root (both as the system sees them)?
 * 1 yes, 0 no, -1 if the system cannot tell. */
int fs_fd_is_inside(const char *root, int fd)
{
	char real_root[PATH_MAX], real_file[PATH_MAX];
	if (!root || !realpath(root, real_root) ||
	    platform_fd_path(fd, real_file, sizeof(real_file)) != 0)
		return -1;
	size_t n = strlen(real_root);
	if (n > 0 && real_root[n - 1] == '/')
		n--; /* a filesystem root */
	return strncmp(real_file, real_root, n) == 0 && real_file[n] == '/';
}

int fs_write_atomic_in(const char *root, const char *path, const char *data, size_t len,
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

	/* The directory was checked as a string; this checks where the file
	 * actually landed, in case the directory was swapped in between. */
	if (root && fs_fd_is_inside(root, fd) == 0) {
		close(fd);
		unlink(tmp);
		errno = EACCES;
		return -1;
	}

	int saved_errno = 0;
	if (fchmod(fd, mode) != 0 || fs_write_all(fd, data, len) != 0 ||
	    fsync(fd) != 0)
		saved_errno = errno;
	if (close(fd) != 0 && !saved_errno)
		saved_errno = errno;

	if (!saved_errno) {
		/* link() fails with EEXIST instead of replacing; rename() replaces. */
#ifdef _WIN32
		/* A move that refuses to replace does the same job as link(),
		 * and unlike hard links it works on every Windows filesystem. */
		int rc = no_replace ? plat_rename_noreplace(tmp, path) : rename(tmp, path);
#else
		int rc = no_replace ? link(tmp, path) : rename(tmp, path);
#endif
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

/* -------------------------------------------------------------------------
 * Paths that differ between systems (declared in platform.h)
 * ---------------------------------------------------------------------- */

#ifdef _WIN32

static bool is_separator(char c)
{
	return c == '/' || c == '\\';
}

size_t platform_path_root_length(const char *path)
{
	if (!path)
		return 0;
	if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
	    path[1] == ':' && is_separator(path[2]))
		return 3; /* "C:/" */
	if (is_separator(path[0]) && is_separator(path[1])) {
		/* "//server/share/": the root ends after the share name */
		size_t i = 2;
		while (path[i] && !is_separator(path[i]))
			i++;
		if (path[i])
			i++;
		while (path[i] && !is_separator(path[i]))
			i++;
		return path[i] ? i + 1 : i;
	}
	return is_separator(path[0]) ? 1 : 0; /* "/x": root of the current drive */
}

void platform_path_normalize(char *path)
{
	for (; path && *path; path++)
		if (*path == '\\')
			*path = '/';
}

const char *platform_home_dir(void)
{
	/* HOME first: it is what Git Bash and other Unix-style shells set, and
	 * it lets a caller point Boris at a different home. */
	static char home[PATH_MAX];
	const char *dir = getenv("HOME");
	if (!dir || !dir[0])
		dir = getenv("USERPROFILE");
	if (!dir || !dir[0] || strlen(dir) >= sizeof(home))
		return NULL;
	strcpy(home, dir);
	platform_path_normalize(home);
	size_t len = strlen(home);
	if (len > platform_path_root_length(home) && home[len - 1] == '/')
		home[len - 1] = '\0';
	return home;
}

#else /* POSIX */

int platform_fd_path(int fd, char *out, size_t size)
{
	if (size < PATH_MAX)
		return -1;
#if defined(__linux__)
	char link[64];
	snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
	ssize_t n = readlink(link, out, size - 1);
	if (n <= 0 || out[0] != '/')
		return -1;
	out[n] = '\0';
	return 0;
#elif defined(F_GETPATH)
	return fcntl(fd, F_GETPATH, out) == 0 ? 0 : -1;
#else
	(void)fd;
	(void)out;
	return -1;
#endif
}

size_t platform_path_root_length(const char *path)
{
	return path && path[0] == '/' ? 1 : 0;
}

void platform_path_normalize(char *path)
{
	(void)path;
}

const char *platform_home_dir(void)
{
	const char *home = getenv("HOME");
	if (!home || !home[0]) {
		struct passwd *pw = getpwuid(getuid());
		home = (pw && pw->pw_dir && pw->pw_dir[0]) ? pw->pw_dir : NULL;
	}
	return home;
}

#endif

char *fs_boris_path(const char *relative)
{
	const char *home = platform_home_dir();
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
