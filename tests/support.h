/*
 * support.h - What the tests need from the system, on POSIX and Windows.
 *
 * A temporary directory, a way to delete a tree, and links - the three
 * things the tests used to get from "/tmp", "rm -rf" and symlink().
 * Header-only (static inline) so each test_*.c stays a single translation
 * unit and may use only some of it.
 */

#ifndef TEST_SUPPORT_H
#define TEST_SUPPORT_H

#include "platform.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The temporary directory in the form sandbox_resolve() reports paths:
 * "/tmp" on POSIX; "C:/Users/me/AppData/Local/Temp" on Windows. */
static inline const char *test_tmp_dir(void)
{
#ifdef _WIN32
	static char dir[PATH_MAX];
	if (!dir[0]) {
		const char *temp = getenv("TEMP");
		if (!temp || !realpath(temp, dir)) {
			fprintf(stderr, "tests: cannot resolve %%TEMP%%\n");
			exit(1);
		}
	}
	return dir;
#else
	return "/tmp";
#endif
}

/* "<tmp>/<name>". The result is valid until eight more calls are made. */
static inline const char *test_tmp_path(const char *name)
{
	static char paths[8][PATH_MAX];
	static int next = 0;
	char *path = paths[next++ % 8];
	snprintf(path, PATH_MAX, "%s/%s", test_tmp_dir(), name);
	return path;
}

/* rm -rf, without following links. */
static inline void test_remove_tree(const char *path)
{
	struct stat st;
	if (lstat(path, &st) != 0)
		return;
	if (!S_ISDIR(st.st_mode)) {
		unlink(path);
		return;
	}
	DIR *dir = opendir(path);
	if (dir) {
		struct dirent *entry;
		while ((entry = readdir(dir)) != NULL) {
			if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
				continue;
			char child[PATH_MAX];
			snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
			test_remove_tree(child);
		}
		closedir(dir);
	}
	rmdir(path);
}

/*
 * symlink(), as far as the system allows. Windows only lets ordinary users
 * create symbolic links in Developer Mode, but anyone may create a junction
 * (a link to a directory), so that is the fallback for directory targets.
 * Returns 0, or -1 if no link could be made - the caller skips its checks.
 */
static inline int test_link(const char *target, const char *link_path)
{
#ifdef _WIN32
	if (plat_symlink(target, link_path) == 0)
		return 0;
	struct stat st;
	if (platform_path_root_length(target) == 0 || stat(target, &st) != 0 ||
	    !S_ISDIR(st.st_mode))
		return -1;
	char native_link[PATH_MAX], native_target[PATH_MAX], cmd[2 * PATH_MAX + 64];
	snprintf(native_link, sizeof(native_link), "%s", link_path);
	snprintf(native_target, sizeof(native_target), "%s", target);
	for (char *p = native_link; *p; p++)
		if (*p == '/')
			*p = '\\';
	for (char *p = native_target; *p; p++)
		if (*p == '/')
			*p = '\\';
	snprintf(cmd, sizeof(cmd), "mklink /J \"%s\" \"%s\" >NUL 2>&1", native_link,
		 native_target);
	return system(cmd) == 0 ? 0 : -1;
#else
	return symlink(target, link_path);
#endif
}

/* Can test_link() make a link to a file here? */
static inline bool test_file_links_work(void)
{
	static int cached = -1;
	if (cached < 0) {
		char target[PATH_MAX], link_path[PATH_MAX];
		snprintf(target, sizeof(target), "%s/boris_linkprobe_%d.txt", test_tmp_dir(),
			 (int)getpid());
		snprintf(link_path, sizeof(link_path), "%s/boris_linkprobe_%d.lnk",
			 test_tmp_dir(), (int)getpid());
		FILE *f = fopen(target, "w");
		if (f)
			fclose(f);
		cached = f && test_link(target, link_path) == 0;
		unlink(link_path);
		unlink(target);
		if (!cached)
			printf("  (note: this system does not let the tests create symbolic "
			       "links to files; those checks are skipped)\n");
	}
	return cached == 1;
}

#endif /* TEST_SUPPORT_H */
