/*
 * platform.h - The seam between Boris and the operating system.
 *
 * Boris is written against POSIX. On Linux and macOS this header simply
 * includes the system headers the code relies on. On Windows it declares the
 * small subset of POSIX that Boris actually uses - same names, same
 * contracts - implemented over Win32 in src/platform_win32.c, plus a few
 * console helpers that have no POSIX spelling.
 *
 * Conventions on Windows:
 *   - Every path and string is UTF-8. The shims convert to UTF-16 at the
 *     Win32 boundary; boris.exe carries a manifest that makes the C runtime
 *     (fopen, getenv, argv) UTF-8 as well.
 *   - Canonical paths use forward slashes ("C:/Users/me/project"), which
 *     Win32 accepts everywhere. That keeps the sandbox's prefix checks and
 *     every strrchr(path, '/') identical on all platforms.
 *   - Files are opened in binary mode: bytes on disk are bytes in memory.
 *
 * Include this instead of <unistd.h>, <sys/stat.h>, <dirent.h>, <fcntl.h>,
 * <strings.h>, <signal.h> or <sys/time.h>.
 */

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stddef.h>

#ifndef _WIN32
/* ------------------------------------------------------------------------
 * POSIX: the real thing
 * --------------------------------------------------------------------- */

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define PLATFORM_WINDOWS 0

static inline void platform_init(void)
{
}

#else /* _WIN32 */
/* ------------------------------------------------------------------------
 * Windows: the subset of POSIX that Boris uses
 * --------------------------------------------------------------------- */

#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <process.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#define PLATFORM_WINDOWS 1

/* Set the console to UTF-8 (restored at exit). Call once, early in main(). */
void platform_init(void);

/* ---- Types and constants ---------------------------------------------- */

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef ptrdiff_t ssize_t;
#endif
typedef int mode_t;
typedef int pid_t;

#undef PATH_MAX
#define PATH_MAX 4096

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#ifndef F_OK
#define F_OK 0
#endif

/* ---- Files ------------------------------------------------------------- */

/*
 * stat() and lstat(). Symbolic links and junctions are both reported as
 * S_IFLNK by lstat(), so code that refuses to follow links refuses both.
 */
struct plat_stat {
	unsigned int st_mode;
	long long st_size;
	time_t st_mtime;
};

#undef S_IFMT
#undef S_IFDIR
#undef S_IFREG
#undef S_IFLNK
#define S_IFMT	0xF000
#define S_IFDIR 0x4000
#define S_IFREG 0x8000
#define S_IFLNK 0xA000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)

int plat_stat(const char *path, struct plat_stat *st);
int plat_lstat(const char *path, struct plat_stat *st);

/* "struct stat" and "stat()" both become the plat_ versions. */
#define stat  plat_stat
#define lstat plat_lstat

/* open(): always binary, never inherited by child processes. With
 * O_NOFOLLOW a final component that is a link fails with ELOOP. */
#define O_NOFOLLOW 0x01000000
int plat_open(const char *path, int flags, ...);
#define open plat_open

int plat_mkdir(const char *path);
int plat_rmdir(const char *path);
int plat_unlink(const char *path); /* also removes read-only files and links */
int plat_rename(const char *from, const char *to); /* replaces, like POSIX */
int plat_rename_noreplace(const char *from, const char *to); /* EEXIST if to exists */
int plat_mkstemp(char *template_path);
int plat_symlink(const char *target, const char *link_path); /* EPERM unless allowed */
char *plat_realpath(const char *path, char *resolved);

#define mkdir(path, mode) plat_mkdir(path)
#define rmdir		  plat_rmdir
#define unlink		  plat_unlink
#define rename		  plat_rename
#define mkstemp		  plat_mkstemp
#define symlink		  plat_symlink
#define realpath	  plat_realpath
#define fsync		  _commit
#define fchmod(fd, mode)  0 /* permissions come from the directory's ACL */

/* True only for a real console, not for NUL or other character devices. */
int plat_isatty(int fd);
#define isatty plat_isatty

/* Advisory whole-file locks. */
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_UN 8
int plat_flock(int fd, int operation);
#define flock plat_flock

/* ---- Directories ------------------------------------------------------- */

struct dirent {
	char d_name[1024];
};
typedef struct plat_dir DIR;

DIR *plat_opendir(const char *path);
struct dirent *plat_readdir(DIR *dir);
int plat_closedir(DIR *dir);
int plat_scandir(const char *path, struct dirent ***namelist,
		 int (*filter)(const struct dirent *),
		 int (*compar)(const struct dirent **, const struct dirent **));

#define opendir	 plat_opendir
#define readdir	 plat_readdir
#define closedir plat_closedir
#define scandir	 plat_scandir

/* ---- Strings ----------------------------------------------------------- */

#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#define strtok_r    strtok_s

char *plat_strndup(const char *s, size_t n);
ssize_t plat_getline(char **line, size_t *cap, FILE *stream);
#define strndup plat_strndup
#define getline plat_getline

/* ---- Time -------------------------------------------------------------- */

#define CLOCK_MONOTONIC 1
int plat_clock_gettime(int clock_id, struct timespec *ts);
int plat_nanosleep(const struct timespec *req, struct timespec *rem);
#define clock_gettime plat_clock_gettime
#define nanosleep     plat_nanosleep

static inline struct tm *plat_gmtime_r(const time_t *t, struct tm *out)
{
	return gmtime_s(out, t) == 0 ? out : NULL;
}

static inline struct tm *plat_localtime_r(const time_t *t, struct tm *out)
{
	return localtime_s(out, t) == 0 ? out : NULL;
}
#define gmtime_r    plat_gmtime_r
#define localtime_r plat_localtime_r

/* ---- Environment ------------------------------------------------------- */

int plat_setenv(const char *name, const char *value, int overwrite);
int plat_unsetenv(const char *name);
#define setenv	 plat_setenv
#define unsetenv plat_unsetenv

/* ---- Signals ----------------------------------------------------------- */

/*
 * sigaction() for SIGINT only, delivered from the console's Ctrl+C event.
 * The handler runs on a separate thread, so (as on POSIX) it should do no
 * more than set a volatile sig_atomic_t flag. Blocking calls are not
 * interrupted; long waits poll the flag instead.
 */
struct plat_sigaction {
	void (*sa_handler)(int);
	int sa_flags;
	int sa_mask;
};
#define SA_RESTART	  0
#define sigemptyset(mask) (*(mask) = 0)
int plat_sigaction(int sig, const struct plat_sigaction *act, struct plat_sigaction *old);
#define sigaction plat_sigaction /* the struct and the function, as with stat */

/* ---- Command line ------------------------------------------------------ */

struct option {
	const char *name;
	int has_arg;
	int *flag;
	int val;
};
#define no_argument	  0
#define required_argument 1

extern char *optarg;
extern int optind;
extern int opterr; /* 0: do not print messages for bad options */
int getopt_long(int argc, char *const argv[], const char *optstring,
		const struct option *longopts, int *longindex);

/* ---- Pattern matching (src/platform_regex.c) --------------------------- */

#define FNM_NOMATCH 1
int fnmatch(const char *pattern, const char *string, int flags);

#define REG_EXTENDED 1
#define REG_ICASE    2
#define REG_NOSUB    4
#define REG_NOMATCH  1

typedef struct {
	void *program;
} regex_t;
typedef struct {
	int rm_so, rm_eo;
} regmatch_t;

/* POSIX extended regular expressions, match / no match only (REG_NOSUB). */
int regcomp(regex_t *preg, const char *pattern, int cflags);
int regexec(const regex_t *preg, const char *string, size_t nmatch,
	    regmatch_t pmatch[], int eflags);
size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size);
void regfree(regex_t *preg);

/* ---- Console (no POSIX equivalent) ------------------------------------- */

/* Is stdout a console that understands ANSI escape sequences? The first
 * call switches virtual-terminal processing on. */
bool platform_console_vt(void);

/* Can the console's font be trusted with glyphs beyond box drawing? */
bool platform_console_rich_glyphs(void);

int platform_console_width(void);

/* Raw mode: no echo, no line buffering, Ctrl+C read as byte 0x03.
 * Returns whether raw mode is now on. */
bool platform_console_raw(bool enable);

/* Next byte of keyboard input as UTF-8, keys as ANSI sequences; -1 on EOF. */
int platform_console_read_byte(void);

/* Read one line from the console as UTF-8, without the newline. Returns
 * false on EOF. With echo off the typed text is not shown. */
bool platform_console_read_line(char *buf, size_t size, bool echo);

/*
 * Encrypt a secret so that only the current Windows user can read it back
 * (DPAPI), as text safe to put in a config file. Both return a heap string
 * or NULL.
 */
char *platform_secret_protect(const char *secret);
char *platform_secret_unprotect(const char *text);

/* Call fn every interval_ms from a background thread until stopped. */
void platform_ticker_start(unsigned interval_ms, void (*fn)(void));
void platform_ticker_stop(void);

void platform_sleep_ms(unsigned ms);

#endif /* _WIN32 */

/* ------------------------------------------------------------------------
 * Paths (both platforms; see src/fs.c)
 * --------------------------------------------------------------------- */

/*
 * Length of the root at the start of an absolute path, or 0 if path is
 * relative: 1 for "/x"; on Windows also 3 for "C:/x" and the length of
 * "//server/share/" for a network path.
 */
size_t platform_path_root_length(const char *path);

/* Rewrite backslashes as forward slashes on Windows; nothing on POSIX,
 * where a backslash is an ordinary file name character. */
void platform_path_normalize(char *path);

/*
 * The canonical path of the file an open descriptor refers to - where it
 * really is now, whatever name it was opened by. size must be >= PATH_MAX.
 * Returns 0, or -1 where the system cannot say.
 */
int platform_fd_path(int fd, char *out, size_t size);

/* The user's home directory with forward slashes, or NULL. */
const char *platform_home_dir(void);

#endif /* PLATFORM_H */
