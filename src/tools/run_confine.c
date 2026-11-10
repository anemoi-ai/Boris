/*
 * tools/run_confine.c - Confine scripts started by the run tool (Linux).
 *
 * The path sandbox decides which script may be started. It has no say in
 * what that script then does: it is ordinary code running as the user. So
 * a model that is refused "read ~/.ssh/id_rsa" can write a three-line
 * script that prints it, and run that instead.
 *
 * Landlock (Linux 5.13+) lets an unprivileged process give up filesystem
 * access for itself and everything it will ever start. The child does that
 * just before exec, keeping:
 *
 *   read, write, create, delete   the sandbox root, and a private temp dir
 *   read and execute              the system: /usr /bin /lib /etc ...,
 *                                 the interpreter's own installation, and
 *                                 anything named in tools.run_read_paths
 *   nothing                       everywhere else - notably the rest of the
 *                                 user's home directory
 *
 * The rules are prepared in the parent, where allocation and logging are
 * safe; the child only makes two system calls. Network access is not
 * restricted.
 *
 * Where Landlock is missing (older kernels, macOS) scripts run unconfined,
 * a warning is logged once, and behaviour.confirm_run remains the
 * safeguard. The Windows counterpart is in run_win32.c.
 */

#define _GNU_SOURCE

#ifndef _WIN32

#include "run_internal.h"
#include "fs.h"
#include "logger.h"
#include "platform.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__) && defined(__has_include)
#if __has_include(<linux/landlock.h>)
#define HAVE_LANDLOCK 1
#endif
#endif

#ifdef HAVE_LANDLOCK

#include <linux/landlock.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

/* Rights that make sense on a file as well as on a directory. */
#define ACCESS_FILE_READ (LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_EXECUTE)

#define ACCESS_READ (ACCESS_FILE_READ | LANDLOCK_ACCESS_FS_READ_DIR)

/* Everything Landlock v1 knows; newer rights are added by ABI version. */
#define ACCESS_ALL_V1                                                             \
	(LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE |             \
	 LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |             \
	 LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |         \
	 LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |             \
	 LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |             \
	 LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |           \
	 LANDLOCK_ACCESS_FS_MAKE_SYM)

/* Where interpreters, their libraries and their configuration live. */
static const char *const SYSTEM_DIRS[] = {
	"/usr", "/bin", "/sbin", "/lib", "/lib32", "/lib64", "/libx32", "/etc", "/opt",
	"/proc", "/sys", "/run", "/nix", "/snap", "/var/lib",
};

static __u64 all_rights(long abi)
{
	__u64 rights = ACCESS_ALL_V1;
#ifdef LANDLOCK_ACCESS_FS_REFER
	if (abi >= 2)
		rights |= LANDLOCK_ACCESS_FS_REFER;
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
	if (abi >= 3)
		rights |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
	if (abi >= 5)
		rights |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
#endif
	return rights;
}

/* Allow rights beneath path. A path that does not exist is skipped. */
static void allow(int ruleset, const char *path, __u64 rights)
{
	int fd = open(path, O_PATH | O_CLOEXEC);
	if (fd < 0)
		return;
	struct stat st;
	if (fstat(fd, &st) == 0 && !S_ISDIR(st.st_mode))
		rights &= ACCESS_FILE_READ | LANDLOCK_ACCESS_FS_WRITE_FILE;

	struct landlock_path_beneath_attr rule = {
		.allowed_access = rights,
		.parent_fd = fd,
	};
	if (syscall(SYS_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH, &rule, 0) != 0)
		log_debug("run: landlock: cannot add rule for %s: %s", path, strerror(errno));
	close(fd);
}

/*
 * Where is the interpreter installed? /home/me/.pyenv/versions/3.12/bin/python3
 * needs .../3.12/lib too, so the answer is the directory above "bin".
 * Nothing is added for interpreters already under a system directory.
 */
static void allow_interpreter(int ruleset, const char *interpreter, const char *path_var)
{
	char found[PATH_MAX] = "", real[PATH_MAX];
	if (strchr(interpreter, '/')) {
		snprintf(found, sizeof(found), "%s", interpreter);
	} else {
		char dirs[8192];
		snprintf(dirs, sizeof(dirs), "%s", path_var ? path_var : "");
		char *save = NULL;
		for (char *dir = strtok_r(dirs, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
			char candidate[PATH_MAX];
			int n = snprintf(candidate, sizeof(candidate), "%s/%s", dir, interpreter);
			if (n > 0 && (size_t)n < sizeof(candidate) && access(candidate, X_OK) == 0) {
				snprintf(found, sizeof(found), "%s", candidate);
				break;
			}
		}
	}
	if (!found[0] || !realpath(found, real))
		return;

	for (size_t i = 0; i < sizeof(SYSTEM_DIRS) / sizeof(SYSTEM_DIRS[0]); i++) {
		size_t n = strlen(SYSTEM_DIRS[i]);
		if (strncmp(real, SYSTEM_DIRS[i], n) == 0 && real[n] == '/')
			return;
	}

	char *slash = strrchr(real, '/');
	if (!slash || slash == real)
		return;
	*slash = '\0'; /* .../bin */
	slash = strrchr(real, '/');
	if (slash && slash != real && strcmp(slash, "/bin") == 0)
		*slash = '\0'; /* the installation prefix */
	log_debug("run: confinement: interpreter files readable under %s", real);
	allow(ruleset, real, ACCESS_READ);
}

void run_confine_prepare(const struct agent_configuration *cfg, const char *sandbox_root,
			 const char *interpreter, const char *path_var,
			 struct run_confinement *out)
{
	memset(out, 0, sizeof(*out));
	out->ruleset_fd = -1;
	if (!cfg->run_confine)
		return;

	long abi = syscall(SYS_landlock_create_ruleset, NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
	if (abi < 1) {
		static bool warned = false;
		if (!warned)
			log_warning("run: this kernel has no Landlock, so scripts are not "
				    "confined to the sandbox: they can read and change "
				    "anything you can. Keep behaviour.confirm_run on.");
		warned = true;
		return;
	}

	/* A temp directory of the script's own, in place of the shared /tmp. */
	const char *tmp = getenv("TMPDIR");
	int n = snprintf(out->tmp_dir, sizeof(out->tmp_dir), "%s/boris-run-%u",
			 tmp && tmp[0] ? tmp : "/tmp", (unsigned)getuid());
	if (n < 0 || (size_t)n >= sizeof(out->tmp_dir) || fs_mkdirp(out->tmp_dir, 0700) != 0) {
		log_warning("run: cannot create %s; scripts are not confined", out->tmp_dir);
		return;
	}

	__u64 rights = all_rights(abi);
	struct landlock_ruleset_attr attr = {.handled_access_fs = rights};
	int ruleset = (int)syscall(SYS_landlock_create_ruleset, &attr, sizeof(attr), 0);
	if (ruleset < 0) {
		log_warning("run: landlock: %s; scripts are not confined", strerror(errno));
		return;
	}

	allow(ruleset, sandbox_root, rights);
	allow(ruleset, out->tmp_dir, rights);
	for (size_t i = 0; i < sizeof(SYSTEM_DIRS) / sizeof(SYSTEM_DIRS[0]); i++)
		allow(ruleset, SYSTEM_DIRS[i], ACCESS_READ);
	/* /dev/null, /dev/tty, /dev/urandom: read and write, but create nothing */
	allow(ruleset, "/dev",
	      (ACCESS_READ | LANDLOCK_ACCESS_FS_WRITE_FILE | (rights & ~ACCESS_ALL_V1)) &
		      ~(__u64)LANDLOCK_ACCESS_FS_EXECUTE);
	allow_interpreter(ruleset, interpreter, path_var);

	if (cfg->run_read_paths && cfg->run_read_paths[0]) {
		char paths[4096];
		snprintf(paths, sizeof(paths), "%s", cfg->run_read_paths);
		char *save = NULL;
		for (char *p = strtok_r(paths, ",", &save); p; p = strtok_r(NULL, ",", &save)) {
			while (*p == ' ' || *p == '\t')
				p++;
			size_t len = strlen(p);
			while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t'))
				p[--len] = '\0';
			if (p[0] == '~' && (p[1] == '/' || p[1] == '\0') && platform_home_dir()) {
				char expanded[PATH_MAX];
				snprintf(expanded, sizeof(expanded), "%s%s", platform_home_dir(),
					 p + 1);
				allow(ruleset, expanded, ACCESS_READ);
			} else if (p[0]) {
				allow(ruleset, p, ACCESS_READ);
			}
		}
	}

	out->ruleset_fd = ruleset;
	out->active = true;
}

/* In the child, between fork and exec: async-signal-safe calls only. */
int run_confine_child(const struct run_confinement *c)
{
	if (!c || !c->active)
		return 0;
	/* Required before an unprivileged process may restrict itself; it
	 * also means a setuid program cannot be used to get the rights back. */
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
	    syscall(SYS_landlock_restrict_self, c->ruleset_fd, 0) != 0)
		return -1;
	close(c->ruleset_fd);
	return 0;
}

void run_confine_release(struct run_confinement *c)
{
	if (c && c->ruleset_fd >= 0)
		close(c->ruleset_fd);
	if (c)
		c->ruleset_fd = -1;
}

#else /* no Landlock on this system */

void run_confine_prepare(const struct agent_configuration *cfg, const char *sandbox_root,
			 const char *interpreter, const char *path_var,
			 struct run_confinement *out)
{
	(void)sandbox_root;
	(void)interpreter;
	(void)path_var;
	memset(out, 0, sizeof(*out));
	out->ruleset_fd = -1;

	static bool warned = false;
	if (cfg->run_confine && !warned)
		log_warning("run: this system has no way to confine scripts to the "
			    "sandbox: they can read and change anything you can. Keep "
			    "behaviour.confirm_run on.");
	warned = true;
}

int run_confine_child(const struct run_confinement *c)
{
	(void)c;
	return 0;
}

void run_confine_release(struct run_confinement *c)
{
	(void)c;
}

#endif /* HAVE_LANDLOCK */

#endif /* !_WIN32 */
