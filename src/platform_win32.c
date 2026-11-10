/*
 * platform_win32.c - The subset of POSIX that Boris uses, over Win32.
 *
 * See platform.h for the contract. Everything here takes and returns UTF-8
 * and converts to UTF-16 at the system call. Paths handed back to callers
 * use forward slashes.
 *
 * On other systems this file compiles to nothing.
 */

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>

#include "platform.h"

#include <stdarg.h>

#define WPATH_MAX PATH_MAX /* UTF-16 units; a UTF-8 path never needs more */

/* ------------------------------------------------------------------------
 * Errors and text conversion
 * --------------------------------------------------------------------- */

static int errno_from_win32(DWORD code)
{
	switch (code) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_INVALID_NAME:
	case ERROR_INVALID_DRIVE:
	case ERROR_BAD_NETPATH:
	case ERROR_BAD_NET_NAME:
	case ERROR_BAD_PATHNAME:
	case ERROR_NO_MORE_FILES:
		return ENOENT;
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:
	case ERROR_WRITE_PROTECT:
	case ERROR_PRIVILEGE_NOT_HELD:
		return EACCES;
	case ERROR_FILE_EXISTS:
	case ERROR_ALREADY_EXISTS:
		return EEXIST;
	case ERROR_DIR_NOT_EMPTY:
		return ENOTEMPTY;
	case ERROR_DIRECTORY:
		return ENOTDIR;
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_OUTOFMEMORY:
		return ENOMEM;
	case ERROR_DISK_FULL:
	case ERROR_HANDLE_DISK_FULL:
		return ENOSPC;
	case ERROR_FILENAME_EXCED_RANGE:
		return ENAMETOOLONG;
	case ERROR_NOT_SAME_DEVICE:
		return EXDEV;
	case ERROR_BROKEN_PIPE:
	case ERROR_NO_DATA:
		return EPIPE;
	case ERROR_INVALID_HANDLE:
		return EBADF;
	case ERROR_CANT_RESOLVE_FILENAME:
		return ELOOP;
	default:
		return EINVAL;
	}
}

static int fail_win32(void)
{
	errno = errno_from_win32(GetLastError());
	return -1;
}

/* UTF-8 -> UTF-16 into a caller buffer. Returns false with errno set. */
static bool to_wide(const char *utf8, wchar_t *out, int cap)
{
	if (!utf8 || !utf8[0]) {
		errno = ENOENT;
		return false;
	}
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, cap) > 0)
		return true;
	errno = GetLastError() == ERROR_INSUFFICIENT_BUFFER ? ENAMETOOLONG : ENOENT;
	return false;
}

/* UTF-16 -> UTF-8 into a caller buffer. Returns false with errno set. */
static bool to_utf8(const wchar_t *wide, char *out, int cap)
{
	if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, cap, NULL, NULL) > 0)
		return true;
	errno = ENAMETOOLONG;
	return false;
}

static time_t time_from_filetime(FILETIME ft)
{
	ULARGE_INTEGER ticks;
	ticks.LowPart = ft.dwLowDateTime;
	ticks.HighPart = ft.dwHighDateTime;
	/* 100 ns intervals since 1601 -> seconds since 1970 */
	return (time_t)(ticks.QuadPart / 10000000ULL - 11644473600ULL);
}

/* ------------------------------------------------------------------------
 * stat, lstat
 * --------------------------------------------------------------------- */

static bool tag_is_link(DWORD attributes, DWORD reparse_tag)
{
	/* Other reparse points (cloud placeholders, deduplicated files) are
	 * ordinary files as far as a path check is concerned. */
	return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
	       (reparse_tag == IO_REPARSE_TAG_SYMLINK ||
		reparse_tag == IO_REPARSE_TAG_MOUNT_POINT);
}

static unsigned int mode_from_attributes(DWORD attributes, bool is_link)
{
	if (is_link)
		return S_IFLNK | 0777;
	if (attributes & FILE_ATTRIBUTE_DIRECTORY)
		return S_IFDIR | 0755;
	return S_IFREG | ((attributes & FILE_ATTRIBUTE_READONLY) ? 0444 : 0644);
}

static int stat_common(const char *path, struct plat_stat *st, bool follow)
{
	wchar_t wpath[WPATH_MAX];
	if (!to_wide(path, wpath, WPATH_MAX))
		return -1;
	memset(st, 0, sizeof(*st));

	DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT);
	HANDLE h = CreateFileW(wpath, FILE_READ_ATTRIBUTES,
			       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
			       OPEN_EXISTING, flags, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		/* A file someone holds exclusively can still be described. */
		DWORD open_error = GetLastError();
		WIN32_FILE_ATTRIBUTE_DATA data;
		if (open_error != ERROR_SHARING_VIOLATION ||
		    !GetFileAttributesExW(wpath, GetFileExInfoStandard, &data)) {
			errno = errno_from_win32(open_error);
			return -1;
		}
		st->st_mode = mode_from_attributes(data.dwFileAttributes, false);
		st->st_size = ((long long)data.nFileSizeHigh << 32) | data.nFileSizeLow;
		st->st_mtime = time_from_filetime(data.ftLastWriteTime);
		return 0;
	}

	if (GetFileType(h) != FILE_TYPE_DISK) {
		CloseHandle(h);
		st->st_mode = 0x2000; /* a device such as NUL: not a file, not a directory */
		return 0;
	}

	BY_HANDLE_FILE_INFORMATION info;
	FILE_ATTRIBUTE_TAG_INFO tag = {0};
	if (!GetFileInformationByHandle(h, &info)) {
		fail_win32();
		CloseHandle(h);
		return -1;
	}
	GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof(tag));
	CloseHandle(h);

	bool is_link = !follow && tag_is_link(info.dwFileAttributes, tag.ReparseTag);
	st->st_mode = mode_from_attributes(info.dwFileAttributes, is_link);
	st->st_size = ((long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
	st->st_mtime = time_from_filetime(info.ftLastWriteTime);
	return 0;
}

int plat_stat(const char *path, struct plat_stat *st)
{
	return stat_common(path, st, true);
}

int plat_lstat(const char *path, struct plat_stat *st)
{
	return stat_common(path, st, false);
}

/* ------------------------------------------------------------------------
 * open and friends
 * --------------------------------------------------------------------- */

int plat_open(const char *path, int flags, ...)
{
	wchar_t wpath[WPATH_MAX];
	if (!to_wide(path, wpath, WPATH_MAX))
		return -1;

	DWORD access = (flags & O_RDWR)	    ? GENERIC_READ | GENERIC_WRITE
		       : (flags & O_WRONLY) ? GENERIC_WRITE
					    : GENERIC_READ;
	DWORD disposition = OPEN_EXISTING;
	if ((flags & O_CREAT) && (flags & O_EXCL))
		disposition = CREATE_NEW;
	else if ((flags & O_CREAT) && (flags & O_TRUNC))
		disposition = CREATE_ALWAYS;
	else if (flags & O_CREAT)
		disposition = OPEN_ALWAYS;
	else if (flags & O_TRUNC)
		disposition = TRUNCATE_EXISTING;

	/* Opening the reparse point itself is what lets us see, on the handle
	 * and therefore without a race, that the name is a link. */
	DWORD attributes = FILE_ATTRIBUTE_NORMAL |
			   ((flags & O_NOFOLLOW) ? FILE_FLAG_OPEN_REPARSE_POINT : 0);
	HANDLE h = CreateFileW(wpath, access,
			       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
			       disposition, attributes, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return fail_win32();

	if (flags & O_NOFOLLOW) {
		FILE_ATTRIBUTE_TAG_INFO tag = {0};
		if (GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof(tag)) &&
		    tag_is_link(tag.FileAttributes, tag.ReparseTag)) {
			CloseHandle(h);
			errno = ELOOP;
			return -1;
		}
	}

	int fd = _open_osfhandle((intptr_t)h, _O_BINARY | _O_NOINHERIT | (flags & O_APPEND));
	if (fd < 0) {
		CloseHandle(h);
		errno = EMFILE;
	}
	return fd;
}

int plat_mkdir(const char *path)
{
	wchar_t wpath[WPATH_MAX];
	if (!to_wide(path, wpath, WPATH_MAX))
		return -1;
	return CreateDirectoryW(wpath, NULL) ? 0 : fail_win32();
}

int plat_rmdir(const char *path)
{
	wchar_t wpath[WPATH_MAX];
	if (!to_wide(path, wpath, WPATH_MAX))
		return -1;
	return RemoveDirectoryW(wpath) ? 0 : fail_win32();
}

int plat_unlink(const char *path)
{
	wchar_t wpath[WPATH_MAX];
	if (!to_wide(path, wpath, WPATH_MAX))
		return -1;

	DWORD attributes = GetFileAttributesW(wpath);
	if (attributes == INVALID_FILE_ATTRIBUTES)
		return fail_win32();

	/* POSIX unlink() ignores the file's own permissions. */
	if (attributes & FILE_ATTRIBUTE_READONLY)
		SetFileAttributesW(wpath, attributes & ~(DWORD)FILE_ATTRIBUTE_READONLY);

	if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
		/* Only a link to a directory may be unlinked; removing it leaves
		 * the target alone. */
		if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
			errno = EISDIR;
			return -1;
		}
		return RemoveDirectoryW(wpath) ? 0 : fail_win32();
	}
	return DeleteFileW(wpath) ? 0 : fail_win32();
}

static int rename_common(const char *from, const char *to, DWORD flags)
{
	wchar_t wfrom[WPATH_MAX], wto[WPATH_MAX];
	if (!to_wide(from, wfrom, WPATH_MAX) || !to_wide(to, wto, WPATH_MAX))
		return -1;
	return MoveFileExW(wfrom, wto, flags) ? 0 : fail_win32();
}

int plat_rename(const char *from, const char *to)
{
	return rename_common(from, to, MOVEFILE_REPLACE_EXISTING);
}

int plat_rename_noreplace(const char *from, const char *to)
{
	return rename_common(from, to, 0);
}

int plat_mkstemp(char *template_path)
{
	static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
	size_t len = strlen(template_path);
	if (len < 6 || strcmp(template_path + len - 6, "XXXXXX") != 0) {
		errno = EINVAL;
		return -1;
	}

	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	unsigned long long seed = (unsigned long long)counter.QuadPart ^
				  ((unsigned long long)GetCurrentProcessId() << 32) ^
				  GetTickCount64();

	for (int attempt = 0; attempt < 100; attempt++) {
		for (size_t i = len - 6; i < len; i++) {
			seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
			template_path[i] = alphabet[(seed >> 33) % (sizeof(alphabet) - 1)];
		}
		int fd = plat_open(template_path, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0 || errno != EEXIST)
			return fd;
	}
	errno = EEXIST;
	return -1;
}

/*
 * symlink(). Windows wants to know up front whether the target is a
 * directory, and only lets unprivileged users create links at all when
 * Developer Mode is on; otherwise this fails with EPERM.
 */
int plat_symlink(const char *target, const char *link_path)
{
	wchar_t wtarget[WPATH_MAX], wlink[WPATH_MAX], wfull[WPATH_MAX];
	if (!to_wide(target, wtarget, WPATH_MAX) || !to_wide(link_path, wlink, WPATH_MAX))
		return -1;
	for (wchar_t *p = wtarget; *p; p++)
		if (*p == L'/')
			*p = L'\\'; /* stored verbatim, and only this form resolves */

	/* A relative target is relative to the directory holding the link. */
	wcscpy_s(wfull, WPATH_MAX, wtarget);
	if (platform_path_root_length(target) == 0) {
		wcscpy_s(wfull, WPATH_MAX, wlink);
		wchar_t *slash = wcsrchr(wfull, L'/');
		wchar_t *backslash = wcsrchr(wfull, L'\\');
		if (backslash && (!slash || backslash > slash))
			slash = backslash;
		if (slash)
			slash[1] = L'\0';
		else
			wfull[0] = L'\0';
		if (wcslen(wfull) + wcslen(wtarget) + 1 > WPATH_MAX) {
			errno = ENAMETOOLONG;
			return -1;
		}
		wcscat_s(wfull, WPATH_MAX, wtarget);
	}
	DWORD attributes = GetFileAttributesW(wfull);
	DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
	if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))
		flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;

	if (CreateSymbolicLinkW(wlink, wtarget, flags))
		return 0;
	errno = GetLastError() == ERROR_PRIVILEGE_NOT_HELD ? EPERM
							    : errno_from_win32(GetLastError());
	return -1;
}

/* The path the system reports for an open handle, as "C:/dir/file" or
 * "//server/share/file". Returns false with errno set. */
static bool path_of_handle(HANDLE h, char *resolved)
{
	wchar_t final[WPATH_MAX];
	DWORD n = GetFinalPathNameByHandleW(h, final, WPATH_MAX,
					    FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	if (n == 0 || n >= WPATH_MAX) {
		/* Devices (NUL, CON, pipes) have no path on a volume. */
		DWORD error = GetLastError();
		errno = n ? ENAMETOOLONG : (error == ERROR_PATH_NOT_FOUND ? ENOENT : EINVAL);
		return false;
	}

	/* "\\?\C:\dir" -> "C:\dir";  "\\?\UNC\server\share" -> "\\server\share" */
	wchar_t *start = final;
	if (wcsncmp(start, L"\\\\?\\UNC\\", 8) == 0) {
		start += 6;
		start[0] = L'\\';
	} else if (wcsncmp(start, L"\\\\?\\", 4) == 0) {
		start += 4;
	}

	if (!to_utf8(start, resolved, PATH_MAX))
		return false;
	platform_path_normalize(resolved);

	size_t len = strlen(resolved);
	if (len > platform_path_root_length(resolved) && resolved[len - 1] == '/')
		resolved[len - 1] = '\0';
	return true;
}

/*
 * realpath(): the name the system itself reports for the opened object, so
 * symbolic links, junctions, 8.3 short names, "." and ".." and letter case
 * are all resolved.
 */
char *plat_realpath(const char *path, char *resolved)
{
	wchar_t wpath[WPATH_MAX];
	if (!resolved) {
		errno = EINVAL;
		return NULL;
	}
	if (!to_wide(path, wpath, WPATH_MAX))
		return NULL;

	HANDLE h = CreateFileW(wpath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			       NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		fail_win32();
		return NULL;
	}
	bool ok = path_of_handle(h, resolved);
	int saved = errno;
	CloseHandle(h);
	errno = saved;
	return ok ? resolved : NULL;
}

int platform_fd_path(int fd, char *out, size_t size)
{
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	if (h == INVALID_HANDLE_VALUE || size < PATH_MAX)
		return -1;
	return path_of_handle(h, out) ? 0 : -1;
}

int plat_isatty(int fd)
{
	if (!_isatty(fd))
		return 0;
	DWORD mode;
	return GetConsoleMode((HANDLE)_get_osfhandle(fd), &mode) ? 1 : 0;
}

int plat_flock(int fd, int operation)
{
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	if (h == INVALID_HANDLE_VALUE) {
		errno = EBADF;
		return -1;
	}
	OVERLAPPED whole_file = {0};
	BOOL ok;
	if (operation & LOCK_UN)
		ok = UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &whole_file);
	else
		ok = LockFileEx(h, (operation & LOCK_EX) ? LOCKFILE_EXCLUSIVE_LOCK : 0, 0,
				MAXDWORD, MAXDWORD, &whole_file);
	return ok ? 0 : fail_win32();
}

/* ------------------------------------------------------------------------
 * Directories
 * --------------------------------------------------------------------- */

struct plat_dir {
	HANDLE find;
	WIN32_FIND_DATAW data;
	bool have_data; /* data holds an entry not yet returned */
	struct dirent entry;
};

DIR *plat_opendir(const char *path)
{
	wchar_t pattern[WPATH_MAX];
	if (!to_wide(path, pattern, WPATH_MAX - 2))
		return NULL;
	size_t len = wcslen(pattern);
	if (pattern[len - 1] != L'/' && pattern[len - 1] != L'\\')
		pattern[len++] = L'\\';
	pattern[len++] = L'*';
	pattern[len] = L'\0';

	DIR *dir = calloc(1, sizeof(*dir));
	if (!dir) {
		errno = ENOMEM;
		return NULL;
	}
	dir->find = FindFirstFileW(pattern, &dir->data);
	if (dir->find == INVALID_HANDLE_VALUE) {
		DWORD error = GetLastError();
		free(dir);
		errno = error == ERROR_DIRECTORY ? ENOTDIR : errno_from_win32(error);
		return NULL;
	}
	dir->have_data = true;
	return dir;
}

struct dirent *plat_readdir(DIR *dir)
{
	for (;;) {
		if (!dir->have_data && !FindNextFileW(dir->find, &dir->data))
			return NULL;
		dir->have_data = false;
		/* A name that is not valid UTF-16 cannot be passed back in; skip it. */
		if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, dir->data.cFileName, -1,
					dir->entry.d_name, (int)sizeof(dir->entry.d_name), NULL,
					NULL) > 0)
			return &dir->entry;
	}
}

int plat_closedir(DIR *dir)
{
	if (!dir)
		return -1;
	FindClose(dir->find);
	free(dir);
	return 0;
}

int plat_scandir(const char *path, struct dirent ***namelist,
		 int (*filter)(const struct dirent *),
		 int (*compar)(const struct dirent **, const struct dirent **))
{
	DIR *dir = plat_opendir(path);
	if (!dir)
		return -1;

	struct dirent **list = NULL;
	int count = 0, cap = 0;
	struct dirent *entry;
	while ((entry = plat_readdir(dir)) != NULL) {
		if (filter && !filter(entry))
			continue;
		if (count == cap) {
			int grown = cap ? cap * 2 : 32;
			struct dirent **tmp = realloc(list, (size_t)grown * sizeof(*list));
			if (!tmp)
				goto oom;
			list = tmp;
			cap = grown;
		}
		/* Each entry is trimmed to its name, as scandir() callers expect
		 * to free() them one by one. */
		size_t size = offsetof(struct dirent, d_name) + strlen(entry->d_name) + 1;
		list[count] = malloc(size);
		if (!list[count])
			goto oom;
		memcpy(list[count], entry, size);
		count++;
	}
	plat_closedir(dir);

	if (compar && count > 1)
		qsort(list, (size_t)count, sizeof(*list),
		      (int (*)(const void *, const void *))compar);
	*namelist = list;
	return count;

oom:
	for (int i = 0; i < count; i++)
		free(list[i]);
	free(list);
	plat_closedir(dir);
	errno = ENOMEM;
	return -1;
}

/* ------------------------------------------------------------------------
 * Strings
 * --------------------------------------------------------------------- */

char *plat_strndup(const char *s, size_t n)
{
	size_t len = strnlen(s, n);
	char *copy = malloc(len + 1);
	if (!copy)
		return NULL;
	memcpy(copy, s, len);
	copy[len] = '\0';
	return copy;
}

ssize_t plat_getline(char **line, size_t *cap, FILE *stream)
{
	if (!line || !cap || !stream) {
		errno = EINVAL;
		return -1;
	}
	size_t len = 0;
	int c;
	while ((c = fgetc(stream)) != EOF) {
		if (len + 2 > *cap) {
			size_t grown = *cap ? *cap * 2 : 256;
			char *tmp = realloc(*line, grown);
			if (!tmp) {
				errno = ENOMEM;
				return -1;
			}
			*line = tmp;
			*cap = grown;
		}
		(*line)[len++] = (char)c;
		if (c == '\n')
			break;
	}
	if (len == 0)
		return -1;
	(*line)[len] = '\0';
	return (ssize_t)len;
}

/* ------------------------------------------------------------------------
 * Time
 * --------------------------------------------------------------------- */

int plat_clock_gettime(int clock_id, struct timespec *ts)
{
	(void)clock_id;
	LARGE_INTEGER frequency, counter;
	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&counter);
	ts->tv_sec = (time_t)(counter.QuadPart / frequency.QuadPart);
	ts->tv_nsec = (long)((counter.QuadPart % frequency.QuadPart) * 1000000000LL /
			     frequency.QuadPart);
	return 0;
}

int plat_nanosleep(const struct timespec *req, struct timespec *rem)
{
	long long ms = (long long)req->tv_sec * 1000 + (req->tv_nsec + 999999) / 1000000;
	Sleep(ms > 0 ? (DWORD)ms : 0);
	if (rem) {
		rem->tv_sec = 0;
		rem->tv_nsec = 0;
	}
	return 0;
}

void platform_sleep_ms(unsigned ms)
{
	Sleep(ms);
}

/* ------------------------------------------------------------------------
 * Environment
 * --------------------------------------------------------------------- */

int plat_setenv(const char *name, const char *value, int overwrite)
{
	if (!overwrite && getenv(name))
		return 0;
	/* The C runtime treats an empty value as "remove"; POSIX does not, but
	 * nothing in Boris distinguishes an empty variable from a missing one. */
	return _putenv_s(name, value) == 0 ? 0 : -1;
}

int plat_unsetenv(const char *name)
{
	return _putenv_s(name, "") == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------------
 * SIGINT
 * --------------------------------------------------------------------- */

static void (*volatile g_sigint_handler)(int) = SIG_DFL;
static bool g_ctrl_handler_installed = false;

/* Ctrl+C at the console. Windows calls this on a thread of its own. */
static BOOL WINAPI console_ctrl_handler(DWORD event)
{
	void (*handler)(int) = g_sigint_handler;
	if (event != CTRL_C_EVENT || handler == SIG_DFL)
		return FALSE; /* the default action: end the process */
	if (handler != SIG_IGN)
		handler(SIGINT);
	return TRUE;
}

/* raise(SIGINT), which goes through the C runtime instead. The runtime
 * resets the action to SIG_DFL before each call, so put it back first. */
static void runtime_sigint_handler(int sig)
{
	signal(SIGINT, runtime_sigint_handler);
	void (*handler)(int) = g_sigint_handler;
	if (handler != SIG_DFL && handler != SIG_IGN)
		handler(sig);
}

int plat_sigaction(int sig, const struct plat_sigaction *act, struct plat_sigaction *old)
{
	if (sig != SIGINT) {
		errno = EINVAL;
		return -1;
	}
	if (old) {
		memset(old, 0, sizeof(*old));
		old->sa_handler = g_sigint_handler;
	}
	if (act) {
		g_sigint_handler = act->sa_handler;
		signal(SIGINT, act->sa_handler == SIG_DFL || act->sa_handler == SIG_IGN
				       ? act->sa_handler
				       : runtime_sigint_handler);
		/* Registered after the runtime's own console handler, so ours is
		 * asked first and a real Ctrl+C never sees the SIG_DFL window. */
		if (!g_ctrl_handler_installed) {
			SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
			/* A parent that ignores Ctrl+C passes that on to its children
			 * (as with SIG_IGN on POSIX). Installing a handler means we
			 * want the event, so undo it - as sigaction() would. */
			SetConsoleCtrlHandler(NULL, FALSE);
			g_ctrl_handler_installed = true;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------
 * getopt_long
 *
 * GNU behaviour as far as Boris needs it: options may follow operands
 * (argv is permuted so the operands end up last) unless optstring starts
 * with '+', "--" ends the options, long options may be abbreviated and may
 * take their argument as "--name=value" or "--name value".
 * --------------------------------------------------------------------- */

char *optarg = NULL;
int optind = 1;
int opterr = 1;

static int g_opt_offset = 0; /* position inside a group of short options */
static int g_opt_next = 0;   /* argv index of that group's separate argument */

static bool is_option(const char *arg)
{
	return arg[0] == '-' && arg[1] != '\0';
}

/* Move argv[from] down to argv[to], shifting what is between up by one. */
static void move_argument(char **argv, int from, int to)
{
	char *moved = argv[from];
	for (int i = from; i > to; i--)
		argv[i] = argv[i - 1];
	argv[to] = moved;
}

/* Fetch the argument that follows the option at argv[optind - 1], which may
 * still sit beyond some operands at argv[next]. */
static bool take_argument(int argc, char **argv, int next)
{
	if (next >= argc)
		return false;
	move_argument(argv, next, optind);
	optarg = argv[optind++];
	return true;
}

static int parse_long(int argc, char **argv, int next, const char *text,
		      const struct option *longopts, int *longindex)
{
	const char *equals = strchr(text, '=');
	size_t name_len = equals ? (size_t)(equals - text) : strlen(text);

	int match = -1;
	bool ambiguous = false;
	for (int i = 0; longopts && longopts[i].name; i++) {
		if (strncmp(longopts[i].name, text, name_len) != 0)
			continue;
		if (strlen(longopts[i].name) == name_len) {
			match = i;
			ambiguous = false;
			break;
		}
		if (match >= 0)
			ambiguous = true;
		match = i;
	}
	if (match < 0 || ambiguous) {
		if (opterr)
			fprintf(stderr, "%s: %s option '--%.*s'\n", argv[0],
			ambiguous ? "ambiguous" : "unrecognized", (int)name_len, text);
		return '?';
	}

	const struct option *o = &longopts[match];
	if (o->has_arg == no_argument) {
		if (equals) {
			if (opterr)
				fprintf(stderr, "%s: option '--%s' doesn't allow an argument\n",
				argv[0], o->name);
			return '?';
		}
	} else if (equals) {
		optarg = (char *)equals + 1;
	} else if (!take_argument(argc, argv, next)) {
		if (opterr)
			fprintf(stderr, "%s: option '--%s' requires an argument\n", argv[0], o->name);
		return '?';
	}

	if (longindex)
		*longindex = match;
	if (o->flag) {
		*o->flag = o->val;
		return 0;
	}
	return o->val;
}

int getopt_long(int argc, char *const argv_const[], const char *optstring,
		const struct option *longopts, int *longindex)
{
	char **argv = (char **)argv_const;
	bool stop_at_operand = optstring[0] == '+';
	if (stop_at_operand)
		optstring++;
	optarg = NULL;
	if (optind < 1) {
		optind = 1;
		g_opt_offset = 0;
	}

	if (g_opt_offset == 0) {
		/* Find the next option, stepping over operands. */
		int at = optind;
		while (at < argc && !is_option(argv[at])) {
			if (stop_at_operand)
				return -1;
			at++;
		}
		if (at >= argc)
			return -1;
		move_argument(argv, at, optind);
		int next = at + 1; /* where this option's argument would be */

		const char *arg = argv[optind];
		if (strcmp(arg, "--") == 0) {
			optind++;
			return -1;
		}
		if (arg[1] == '-') {
			optind++;
			return parse_long(argc, argv, next, arg + 2, longopts, longindex);
		}
		g_opt_offset = 1;
		g_opt_next = next;
	}

	/* A short option, possibly one of a group ("-qj"). The group's own
	 * argv entry is at optind; operands it jumped over come after it. */
	const char *group = argv[optind];
	char c = group[g_opt_offset++];
	bool last_in_group = group[g_opt_offset] == '\0';
	const char *spec = c == ':' ? NULL : strchr(optstring, c);

	if (!spec) {
		if (opterr)
			fprintf(stderr, "%s: invalid option -- '%c'\n", argv[0], c);
		if (last_in_group) {
			optind++;
			g_opt_offset = 0;
		}
		return '?';
	}

	if (spec[1] != ':') {
		if (last_in_group) {
			optind++;
			g_opt_offset = 0;
		}
		return c;
	}

	/* Takes an argument: the rest of the group, or the next argv entry. */
	const char *attached = group + g_opt_offset;
	optind++;
	g_opt_offset = 0;
	if (!last_in_group) {
		optarg = (char *)attached;
		return c;
	}
	if (!take_argument(argc, argv, g_opt_next)) {
		if (opterr)
			fprintf(stderr, "%s: option requires an argument -- '%c'\n", argv[0], c);
		return '?';
	}
	return c;
}

/* ------------------------------------------------------------------------
 * Console
 * --------------------------------------------------------------------- */

static UINT g_saved_output_cp = 0;
static DWORD g_saved_output_mode = 0;
static bool g_output_mode_changed = false;
static int g_vt_state = -1; /* -1 = not yet probed */

static DWORD g_saved_input_mode = 0;
static bool g_raw_active = false;

static void console_restore(void)
{
	platform_ticker_stop();
	platform_console_raw(false);
	if (g_output_mode_changed)
		SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), g_saved_output_mode);
	if (g_saved_output_cp)
		SetConsoleOutputCP(g_saved_output_cp);
}

void platform_init(void)
{
	static bool done = false;
	if (done)
		return;
	done = true;

	/* Boris writes UTF-8; tell the console so, and put it back on exit. */
	if (plat_isatty(STDOUT_FILENO) || plat_isatty(STDERR_FILENO)) {
		g_saved_output_cp = GetConsoleOutputCP();
		SetConsoleOutputCP(CP_UTF8);
	}
	atexit(console_restore);
}

bool platform_console_vt(void)
{
	if (g_vt_state >= 0)
		return g_vt_state == 1;
	g_vt_state = 0;

	HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD mode;
	if (!plat_isatty(STDOUT_FILENO) || !GetConsoleMode(out, &mode))
		return false;
	platform_init();
	if (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) {
		g_vt_state = 1;
		return true;
	}
	if (SetConsoleMode(out, mode | ENABLE_PROCESSED_OUTPUT |
					ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
		g_saved_output_mode = mode;
		g_output_mode_changed = true;
		g_vt_state = 1;
	}
	return g_vt_state == 1;
}

bool platform_console_rich_glyphs(void)
{
	/* The classic console host draws from a single font with no fallback,
	 * and its default fonts stop at box drawing. Windows Terminal and the
	 * editors' terminals substitute fonts as needed. */
	const char *wt = getenv("WT_SESSION");
	const char *program = getenv("TERM_PROGRAM");
	return (wt && wt[0]) || (program && program[0]);
}

int platform_console_width(void)
{
	CONSOLE_SCREEN_BUFFER_INFO info;
	if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
		int width = info.srWindow.Right - info.srWindow.Left + 1;
		if (width > 0)
			return width;
	}
	return 80;
}

bool platform_console_raw(bool enable)
{
	HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
	if (!enable) {
		if (g_raw_active)
			SetConsoleMode(in, g_saved_input_mode);
		g_raw_active = false;
		return false;
	}
	if (g_raw_active)
		return true;
	if (!GetConsoleMode(in, &g_saved_input_mode))
		return false;

	/* Keep the user's mouse-selection settings; drop line editing, echo
	 * and Ctrl+C processing; ask for keys as ANSI escape sequences. */
	DWORD mode = g_saved_input_mode &
		     (ENABLE_EXTENDED_FLAGS | ENABLE_QUICK_EDIT_MODE | ENABLE_INSERT_MODE);
	if (SetConsoleMode(in, mode | ENABLE_VIRTUAL_TERMINAL_INPUT))
		g_raw_active = true;
	return g_raw_active;
}

int platform_console_read_byte(void)
{
	static unsigned char pending[8];
	static int pending_len = 0, pending_pos = 0;

	if (pending_pos < pending_len)
		return pending[pending_pos++];

	HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
	wchar_t units[2];
	DWORD got = 0;
	if (!ReadConsoleW(in, &units[0], 1, &got, NULL) || got == 0)
		return -1;
	int count = 1;
	if (IS_HIGH_SURROGATE(units[0])) {
		if (!ReadConsoleW(in, &units[1], 1, &got, NULL) || got == 0)
			return -1;
		count = 2;
	}

	pending_len = WideCharToMultiByte(CP_UTF8, 0, units, count, (char *)pending,
					  (int)sizeof(pending), NULL, NULL);
	if (pending_len <= 0) {
		pending_len = 0;
		return '?';
	}
	pending_pos = 1;
	return pending[0];
}

bool platform_console_read_line(char *buf, size_t size, bool echo)
{
	HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
	DWORD saved;
	if (size == 0 || !GetConsoleMode(in, &saved))
		return false;
	buf[0] = '\0';

	DWORD mode = (saved | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT) &
		     ~(DWORD)ENABLE_VIRTUAL_TERMINAL_INPUT;
	mode = echo ? (mode | ENABLE_ECHO_INPUT) : (mode & ~(DWORD)ENABLE_ECHO_INPUT);
	SetConsoleMode(in, mode);

	wchar_t wide[2048];
	DWORD got = 0;
	SetLastError(0);
	BOOL ok = ReadConsoleW(in, wide, (DWORD)(sizeof(wide) / sizeof(wide[0])) - 1, &got, NULL);
	bool aborted = GetLastError() == ERROR_OPERATION_ABORTED; /* Ctrl+C */
	SetConsoleMode(in, saved);

	/* Ctrl+Z at the start of a line is the console's end-of-file. */
	if (!ok || aborted || got == 0 || wide[0] == 0x1A)
		return false;
	while (got > 0 && (wide[got - 1] == L'\n' || wide[got - 1] == L'\r'))
		got--;
	wide[got] = L'\0';

	/* Convert in full, then cut at a character boundary if buf is small. */
	char utf8[sizeof(wide) / sizeof(wide[0]) * 3 + 1];
	if (!to_utf8(wide, utf8, (int)sizeof(utf8)))
		return false;
	size_t len = strlen(utf8);
	if (len >= size) {
		len = size - 1;
		while (len > 0 && ((unsigned char)utf8[len] & 0xC0) == 0x80)
			len--;
	}
	memcpy(buf, utf8, len);
	buf[len] = '\0';
	return true;
}

/* ------------------------------------------------------------------------
 * Secrets (DPAPI)
 *
 * The key that encrypts is derived from the user's logon credentials and
 * never leaves the system, so the text is useless to another user or on
 * another machine. Written as hex.
 * --------------------------------------------------------------------- */

char *platform_secret_protect(const char *secret)
{
	DATA_BLOB in = {(DWORD)strlen(secret), (BYTE *)secret}, out = {0, NULL};
	if (!CryptProtectData(&in, L"Boris API key", NULL, NULL, NULL,
			      CRYPTPROTECT_UI_FORBIDDEN, &out))
		return NULL;
	char *text = malloc((size_t)out.cbData * 2 + 1);
	for (DWORD i = 0; text && i < out.cbData; i++)
		snprintf(text + i * 2, 3, "%02x", out.pbData[i]);
	if (text)
		text[(size_t)out.cbData * 2] = '\0';
	LocalFree(out.pbData);
	return text;
}

char *platform_secret_unprotect(const char *text)
{
	size_t len = strlen(text);
	if (len == 0 || len % 2 != 0)
		return NULL;
	BYTE *raw = malloc(len / 2);
	if (!raw)
		return NULL;
	for (size_t i = 0; i < len / 2; i++) {
		unsigned int byte;
		if (sscanf(text + i * 2, "%2x", &byte) != 1) {
			free(raw);
			return NULL;
		}
		raw[i] = (BYTE)byte;
	}
	DATA_BLOB in = {(DWORD)(len / 2), raw}, out = {0, NULL};
	BOOL ok = CryptUnprotectData(&in, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN,
				     &out);
	free(raw);
	if (!ok)
		return NULL;
	char *secret = malloc((size_t)out.cbData + 1);
	if (secret) {
		memcpy(secret, out.pbData, out.cbData);
		secret[out.cbData] = '\0';
	}
	SecureZeroMemory(out.pbData, out.cbData);
	LocalFree(out.pbData);
	return secret;
}

/* ------------------------------------------------------------------------
 * Ticker: the stand-in for setitimer() + SIGALRM
 * --------------------------------------------------------------------- */

static HANDLE g_ticker_thread = NULL;
static HANDLE g_ticker_stop = NULL;
static unsigned g_ticker_interval = 0;
static void (*g_ticker_fn)(void) = NULL;

static DWORD WINAPI ticker_main(LPVOID unused)
{
	(void)unused;
	while (WaitForSingleObject(g_ticker_stop, g_ticker_interval) == WAIT_TIMEOUT)
		g_ticker_fn();
	return 0;
}

void platform_ticker_start(unsigned interval_ms, void (*fn)(void))
{
	platform_ticker_stop();
	g_ticker_interval = interval_ms;
	g_ticker_fn = fn;
	g_ticker_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!g_ticker_stop)
		return;
	g_ticker_thread = CreateThread(NULL, 0, ticker_main, NULL, 0, NULL);
	if (!g_ticker_thread) {
		CloseHandle(g_ticker_stop);
		g_ticker_stop = NULL;
	}
}

/* Returns only once the thread has ended: fn will not run again. */
void platform_ticker_stop(void)
{
	if (!g_ticker_thread)
		return;
	SetEvent(g_ticker_stop);
	WaitForSingleObject(g_ticker_thread, INFINITE);
	CloseHandle(g_ticker_thread);
	CloseHandle(g_ticker_stop);
	g_ticker_thread = NULL;
	g_ticker_stop = NULL;
}

#endif /* _WIN32 */
