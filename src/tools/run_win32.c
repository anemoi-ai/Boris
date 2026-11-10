/*
 * tools/run_win32.c - The Windows half of the run tool.
 *
 * Windows has no shebang lines, no fork() and no process groups, so the
 * three jobs run.c does with POSIX calls are done differently here:
 *
 *   Choosing the interpreter. A "#!" line is still honoured, but by name:
 *   "#!/usr/bin/env python3" means "the Python on PATH". Otherwise the
 *   extension decides, from a fixed table. Either way the program must be
 *   an .exe found on the PATH the script will itself be given.
 *
 *   Starting it. CreateProcess takes one command-line string, so the
 *   arguments are quoted the way the C runtime will un-quote them. The
 *   child inherits exactly three handles (stdin, and one pipe for both
 *   stdout and stderr) and gets no console window.
 *
 *   Stopping it. The child is placed in a job object before its first
 *   instruction runs. Terminating the job kills the script and everything
 *   it started, which is what kill(-pgid) does on POSIX.
 *
 *   Confining it. The child runs at low integrity: Windows refuses to let
 *   such a process modify anything that is not itself marked low, whatever
 *   the file permissions say. Only the sandbox root and a temporary
 *   directory are so marked. (This limits what a script can change, not
 *   what it can read; Linux, with Landlock, limits both.)
 *
 * On other systems this file compiles to nothing.
 */

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include "run_internal.h"
#include "logger.h"
#include "tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COMMAND_MAX   32767 /* CreateProcess limit, in UTF-16 units */
#define PROGRAM_MAX   1024
#define SHEBANG_MAX   512
#define POLL_SLICE_MS 15 /* how often output, the deadline and Ctrl+C are checked */

/* ------------------------------------------------------------------------
 * Choosing the interpreter
 * --------------------------------------------------------------------- */

enum launch_style {
	LAUNCH_DIRECT,	   /* interpreter [fixed args] script [args] */
	LAUNCH_POWERSHELL, /* powershell -File script [args] */
	LAUNCH_CMD,	   /* cmd /c "script [args]" */
};

struct interpreter {
	const char *extension;
	const char *programs[4]; /* tried in order */
	enum launch_style style;
};

static const struct interpreter INTERPRETERS[] = {
	/* "python3" last: on a machine without Python it is the alias that
	 * opens the Microsoft Store. */
	{".py", {"python", "py", "python3"}, LAUNCH_DIRECT},
	{".js", {"node"}, LAUNCH_DIRECT},
	{".ps1", {"pwsh", "powershell"}, LAUNCH_POWERSHELL},
	{".bat", {"cmd"}, LAUNCH_CMD},
	{".cmd", {"cmd"}, LAUNCH_CMD},
	{".sh", {"sh", "bash"}, LAUNCH_DIRECT},
	{".rb", {"ruby"}, LAUNCH_DIRECT},
	{".lua", {"lua"}, LAUNCH_DIRECT},
	{".pl", {"perl"}, LAUNCH_DIRECT},
	{".php", {"php"}, LAUNCH_DIRECT},
};

#define INTERPRETER_COUNT (sizeof(INTERPRETERS) / sizeof(INTERPRETERS[0]))

static const struct interpreter *interpreter_for_extension(const char *path)
{
	const char *dot = strrchr(path, '.');
	for (size_t i = 0; dot && i < INTERPRETER_COUNT; i++)
		if (_stricmp(dot, INTERPRETERS[i].extension) == 0)
			return &INTERPRETERS[i];
	return NULL;
}

/* The table entry that lists this program name, e.g. "bash" -> the .sh row. */
static const struct interpreter *interpreter_for_program(const char *name)
{
	if (strncmp(name, "python", 6) == 0) /* python3, python3.12 */
		name = "python";
	for (size_t i = 0; i < INTERPRETER_COUNT; i++)
		for (size_t k = 0; k < 4 && INTERPRETERS[i].programs[k]; k++)
			if (_stricmp(name, INTERPRETERS[i].programs[k]) == 0)
				return &INTERPRETERS[i];
	return NULL;
}

static bool to_wide(const char *utf8, wchar_t *out, int cap)
{
	return MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, cap) > 0;
}

static bool to_utf8(const wchar_t *wide, char *out, int cap)
{
	return WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, cap, NULL, NULL) > 0;
}

static const char *env_value(char *const *envp, const char *name)
{
	size_t len = strlen(name);
	for (; envp && *envp; envp++)
		if (_strnicmp(*envp, name, len) == 0 && (*envp)[len] == '=')
			return *envp + len + 1;
	return NULL;
}

static bool contains_ci(const wchar_t *hay, const wchar_t *needle)
{
	size_t n = wcslen(needle);
	for (; *hay; hay++)
		if (_wcsnicmp(hay, needle, n) == 0)
			return true;
	return false;
}

/*
 * Find name.exe on the given PATH (never the current directory). Two kinds
 * of hit are not wanted: System32's bash.exe, which starts WSL - a
 * different machine as far as paths are concerned - and, unless
 * allow_aliases, the "app execution alias" stubs in WindowsApps, which for
 * a program that is not installed just open the Microsoft Store.
 */
static bool find_on_path(const char *name, const wchar_t *path_var, bool allow_aliases,
			 wchar_t *out)
{
	wchar_t wname[128];
	if (!path_var || !to_wide(name, wname, 128))
		return false;

	wchar_t *dirs = _wcsdup(path_var);
	if (!dirs)
		return false;

	bool found = false;
	wchar_t *context = NULL;
	for (wchar_t *dir = wcstok_s(dirs, L";", &context); dir && !found;
	     dir = wcstok_s(NULL, L";", &context)) {
		wchar_t candidate[PROGRAM_MAX];
		size_t len = wcslen(dir);
		if (len >= 2 && dir[0] == L'"' && dir[len - 1] == L'"') {
			dir[len - 1] = L'\0'; /* PATH entries may be quoted */
			dir++;
		}
		if (dir[0] == L'\0' ||
		    !SearchPathW(dir, wname, L".exe", PROGRAM_MAX, candidate, NULL))
			continue;
		if (!allow_aliases && contains_ci(candidate, L"\\WindowsApps\\"))
			continue;
		if (_stricmp(name, "bash") == 0 && contains_ci(candidate, L"\\System32\\"))
			continue;
		wcscpy_s(out, PROGRAM_MAX, candidate);
		found = true;
	}
	free(dirs);
	return found;
}

/* Git for Windows ships a POSIX shell but keeps it off PATH by default. */
static bool find_git_shell(wchar_t *out)
{
	static const wchar_t *const places[] = {
		L"%ProgramFiles%\\Git\\bin\\sh.exe",
		L"%ProgramFiles(x86)%\\Git\\bin\\sh.exe",
		L"%LOCALAPPDATA%\\Programs\\Git\\bin\\sh.exe",
	};
	for (size_t i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
		DWORD n = ExpandEnvironmentStringsW(places[i], out, PROGRAM_MAX);
		if (n > 0 && n <= PROGRAM_MAX && GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES)
			return true;
	}
	return false;
}

static bool find_program(const struct interpreter *interp, const char *preferred,
			 const wchar_t *path_var, wchar_t *out)
{
	if (interp && interp->style == LAUNCH_CMD) {
		/* Always the system's own cmd.exe, not whatever PATH offers. */
		UINT n = GetSystemDirectoryW(out, PROGRAM_MAX - 16);
		if (n == 0 || n >= PROGRAM_MAX - 16)
			return false;
		wcscat_s(out, PROGRAM_MAX, L"\\cmd.exe");
		return true;
	}
	/* The name the script asked for, then the usual names for that
	 * language; real installations first, Store aliases as a last resort
	 * (an alias is also how a Python installed from the Store appears). */
	for (int allow_aliases = 0; allow_aliases <= 1; allow_aliases++) {
		if (preferred && find_on_path(preferred, path_var, allow_aliases, out))
			return true;
		for (size_t k = 0; interp && k < 4 && interp->programs[k]; k++)
			if (find_on_path(interp->programs[k], path_var, allow_aliases, out))
				return true;
		if (!allow_aliases && interp && strcmp(interp->programs[0], "sh") == 0 &&
		    find_git_shell(out))
			return true;
	}
	return false;
}

struct shebang {
	char program[128]; /* "python3", from "#!/usr/bin/env python3" */
	char *args[8];	   /* further words on the line, pointing into line */
	int argc;
	char line[SHEBANG_MAX];
};

/* Read the "#!" line of script. Returns false if it has none. */
static bool read_shebang(const char *script, struct shebang *sb)
{
	memset(sb, 0, sizeof(*sb));
	FILE *f = fopen(script, "rb");
	if (!f)
		return false;
	bool ok = fgets(sb->line, sizeof(sb->line), f) != NULL;
	fclose(f);
	if (!ok || sb->line[0] != '#' || sb->line[1] != '!')
		return false;

	const char *program = NULL;
	char *context = NULL;
	for (char *word = strtok_s(sb->line + 2, " \t\r\n", &context); word;
	     word = strtok_s(NULL, " \t\r\n", &context)) {
		if (!program) {
			/* "/usr/bin/env" is not the interpreter; the next word is. */
			const char *base = strrchr(word, '/');
			base = base ? base + 1 : word;
			if (strcmp(base, "env") == 0)
				continue;
			if (base[0] == '-' || strchr(base, '='))
				continue; /* env's own options and assignments */
			program = base;
		} else if (sb->argc < 8) {
			sb->args[sb->argc++] = word;
		}
	}
	if (!program || strlen(program) >= sizeof(sb->program))
		return false;
	strcpy(sb->program, program);
	return true;
}

/* ------------------------------------------------------------------------
 * Building the command line
 * --------------------------------------------------------------------- */

struct command {
	wchar_t *text; /* COMMAND_MAX units */
	size_t len;
	bool overflow;
};

static void command_append(struct command *cmd, const wchar_t *s, size_t n)
{
	if (cmd->len + n + 1 > COMMAND_MAX) {
		cmd->overflow = true;
		return;
	}
	memcpy(cmd->text + cmd->len, s, n * sizeof(wchar_t));
	cmd->len += n;
	cmd->text[cmd->len] = L'\0';
}

/*
 * Append one argument, quoted so that the C runtime's argv parser gives the
 * child exactly this string: backslashes are literal except before a quote,
 * where each is doubled and the quote itself escaped.
 */
static void command_append_argument(struct command *cmd, const wchar_t *arg)
{
	if (cmd->len > 0)
		command_append(cmd, L" ", 1);
	if (arg[0] && !wcspbrk(arg, L" \t\n\v\"")) {
		command_append(cmd, arg, wcslen(arg));
		return;
	}

	command_append(cmd, L"\"", 1);
	for (const wchar_t *p = arg;; p++) {
		size_t backslashes = 0;
		while (*p == L'\\') {
			backslashes++;
			p++;
		}
		if (*p == L'\0') {
			/* Before the closing quote: double them all. */
			for (size_t i = 0; i < backslashes * 2; i++)
				command_append(cmd, L"\\", 1);
			break;
		}
		for (size_t i = 0; i < (*p == L'"' ? backslashes * 2 + 1 : backslashes); i++)
			command_append(cmd, L"\\", 1);
		command_append(cmd, p, 1);
	}
	command_append(cmd, L"\"", 1);
}

static void command_append_utf8(struct command *cmd, const char *arg)
{
	wchar_t wide[4096];
	if (!to_wide(arg, wide, 4096)) {
		cmd->overflow = true;
		return;
	}
	command_append_argument(cmd, wide);
}

/*
 * cmd.exe does not use the quoting above; it has its own, and no way to
 * escape a quote or a percent sign inside quotes that is safe for every
 * batch file. Arguments that would need one are refused rather than risk
 * them being run as commands.
 */
static bool safe_for_cmd(const char *s)
{
	return strpbrk(s, "\"%^&|<>!\r\n") == NULL;
}

/* ------------------------------------------------------------------------
 * Confinement: low integrity
 * --------------------------------------------------------------------- */

#define LOW_INTEGRITY_RID 0x1000

/* Is path already labelled as writable from low integrity? */
static bool has_low_label(const wchar_t *path)
{
	PACL sacl = NULL;
	PSECURITY_DESCRIPTOR sd = NULL;
	if (GetNamedSecurityInfoW(path, SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, NULL, NULL,
				  NULL, &sacl, &sd) != ERROR_SUCCESS)
		return false;
	bool low = false;
	SYSTEM_MANDATORY_LABEL_ACE *ace = NULL;
	if (sacl && sacl->AceCount > 0 && GetAce(sacl, 0, (LPVOID *)&ace) &&
	    ace->Header.AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE) {
		PSID sid = (PSID)&ace->SidStart;
		DWORD rid = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
		low = rid <= LOW_INTEGRITY_RID;
	}
	LocalFree(sd);
	return low;
}

/* Label path low, inherited by everything in it now and later. */
static bool set_low_label(const wchar_t *path)
{
	if (has_low_label(path))
		return true;
	/* ML = mandatory label; OICI = files and directories inherit it;
	 * NW = no write-up (the default policy); LW = low integrity. */
	PSECURITY_DESCRIPTOR sd = NULL;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
		    L"S:(ML;OICI;NW;;;LW)", SDDL_REVISION_1, &sd, NULL))
		return false;
	PACL sacl = NULL;
	BOOL present = FALSE, defaulted = FALSE;
	bool ok = GetSecurityDescriptorSacl(sd, &present, &sacl, &defaulted) && present &&
		  SetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION,
					NULL, NULL, NULL, sacl) == ERROR_SUCCESS;
	LocalFree(sd);
	return ok;
}

bool run_win32_confine_prepare(const char *sandbox_root, char *tmp_dir, size_t tmp_size)
{
	static bool warned = false;
	wchar_t wroot[4096], wtmp[4096];
	const char *problem = NULL;

	/* %USERPROFILE%\AppData\LocalLow is where Windows itself has low-
	 * integrity programs keep their files; a directory made there is
	 * labelled low from birth. */
	DWORD n = ExpandEnvironmentStringsW(L"%USERPROFILE%\\AppData\\LocalLow\\Boris", wtmp,
					    4000);
	if (n == 0 || n > 4000) {
		problem = "cannot locate a low-integrity temporary directory";
	} else {
		CreateDirectoryW(wtmp, NULL);
		wcscat_s(wtmp, 4096, L"\\tmp");
		CreateDirectoryW(wtmp, NULL);
		if (GetFileAttributesW(wtmp) == INVALID_FILE_ATTRIBUTES || !set_low_label(wtmp))
			problem = "cannot create a low-integrity temporary directory";
	}

	if (!problem && !to_wide(sandbox_root, wroot, 4096))
		problem = "bad sandbox path";
	if (!problem && !has_low_label(wroot)) {
		log_info("run: marking %s as writable by low-integrity processes, so "
			 "confined scripts can work in it", sandbox_root);
		if (!set_low_label(wroot))
			problem = "cannot mark the sandbox root as writable by "
				  "low-integrity processes";
	}

	if (problem) {
		if (!warned)
			log_warning("run: %s, so scripts are not confined: they can change "
				    "anything you can. Keep behaviour.confirm_run on.",
				    problem);
		warned = true;
		return false;
	}
	if (!to_utf8(wtmp, tmp_dir, (int)tmp_size))
		return false;
	for (char *p = tmp_dir; *p; p++)
		if (*p == '\\')
			*p = '/';
	return true;
}

/* A copy of our own token, lowered to low integrity. NULL on failure. */
static HANDLE low_integrity_token(void)
{
	HANDLE own = NULL, token = NULL;
	if (!OpenProcessToken(GetCurrentProcess(),
			      TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT |
				      TOKEN_ASSIGN_PRIMARY,
			      &own))
		return NULL;
	BOOL ok = DuplicateTokenEx(own, 0, NULL, SecurityImpersonation, TokenPrimary, &token);
	CloseHandle(own);
	if (!ok)
		return NULL;

	PSID low = NULL;
	TOKEN_MANDATORY_LABEL label;
	memset(&label, 0, sizeof(label));
	if (ConvertStringSidToSidW(L"S-1-16-4096", &low)) {
		label.Label.Sid = low;
		label.Label.Attributes = SE_GROUP_INTEGRITY;
		ok = SetTokenInformation(token, TokenIntegrityLevel, &label,
					 sizeof(label) + GetLengthSid(low));
		LocalFree(low);
	} else {
		ok = FALSE;
	}
	if (!ok) {
		CloseHandle(token);
		return NULL;
	}
	return token;
}

/* ------------------------------------------------------------------------
 * Starting and stopping the process
 * --------------------------------------------------------------------- */

static int compare_environment(const void *a, const void *b)
{
	return _wcsicmp(*(const wchar_t *const *)a, *(const wchar_t *const *)b);
}

/* The environment as CreateProcess wants it: sorted "NAME=value" strings,
 * each NUL terminated, with one more NUL at the end. Caller frees. */
static wchar_t *build_environment_block(char *const *envp)
{
	size_t count = 0, units = 1;
	for (char *const *e = envp; *e; e++)
		count++;

	wchar_t **entries = calloc(count + 1, sizeof(*entries));
	if (!entries)
		return NULL;
	bool ok = true;
	for (size_t i = 0; i < count && ok; i++) {
		int n = MultiByteToWideChar(CP_UTF8, 0, envp[i], -1, NULL, 0);
		entries[i] = n > 0 ? malloc((size_t)n * sizeof(wchar_t)) : NULL;
		ok = entries[i] && MultiByteToWideChar(CP_UTF8, 0, envp[i], -1, entries[i], n) > 0;
		units += (size_t)(n > 0 ? n : 0);
	}

	wchar_t *block = ok ? malloc(units * sizeof(wchar_t)) : NULL;
	if (block) {
		qsort(entries, count, sizeof(*entries), compare_environment);
		wchar_t *p = block;
		for (size_t i = 0; i < count; i++) {
			size_t n = wcslen(entries[i]) + 1;
			memcpy(p, entries[i], n * sizeof(wchar_t));
			p += n;
		}
		*p = L'\0';
	}
	for (size_t i = 0; i < count; i++)
		free(entries[i]);
	free(entries);
	return block;
}

struct child {
	HANDLE process;
	HANDLE job;
	HANDLE output; /* our end of the stdout+stderr pipe */
};

static void close_handle(HANDLE *h)
{
	if (*h && *h != INVALID_HANDLE_VALUE)
		CloseHandle(*h);
	*h = NULL;
}

/* Start the process, suspended inside a new job, then let it run. */
static bool spawn(const wchar_t *program, wchar_t *command_line, const wchar_t *environment,
		  const wchar_t *cwd, const char *stdin_data, size_t stdin_len,
		  bool low_integrity, struct child *child)
{
	HANDLE token = NULL;
	SECURITY_ATTRIBUTES inheritable = {sizeof(inheritable), NULL, TRUE};
	HANDLE out_read = NULL, out_write = NULL, in_read = NULL, in_write = NULL;
	LPPROC_THREAD_ATTRIBUTE_LIST attributes = NULL;
	PROCESS_INFORMATION pi = {0};
	bool started = false;
	memset(child, 0, sizeof(*child));

	if (!CreatePipe(&out_read, &out_write, &inheritable, 0))
		goto done;
	SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

	if (stdin_len > 0) {
		/* Big enough to take all of the input before the child reads any. */
		if (!CreatePipe(&in_read, &in_write, &inheritable, 65536))
			goto done;
		SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
	} else {
		in_read = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
				      &inheritable, OPEN_EXISTING, 0, NULL);
		if (in_read == INVALID_HANDLE_VALUE)
			goto done;
	}

	/* Name the handles the child may inherit; otherwise it would get every
	 * inheritable handle in this process. */
	HANDLE inherited[2] = {in_read, out_write};
	SIZE_T attributes_size = 0;
	InitializeProcThreadAttributeList(NULL, 1, 0, &attributes_size);
	attributes = malloc(attributes_size);
	if (!attributes ||
	    !InitializeProcThreadAttributeList(attributes, 1, 0, &attributes_size)) {
		free(attributes);
		attributes = NULL;
		goto done;
	}
	if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
				       inherited, sizeof(inherited), NULL, NULL))
		goto done;

	STARTUPINFOEXW startup;
	memset(&startup, 0, sizeof(startup));
	startup.StartupInfo.cb = sizeof(startup);
	startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	startup.StartupInfo.hStdInput = in_read;
	startup.StartupInfo.hStdOutput = out_write;
	startup.StartupInfo.hStdError = out_write;
	startup.lpAttributeList = attributes;

	child->job = CreateJobObjectW(NULL, NULL);
	if (child->job) {
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
		memset(&limits, 0, sizeof(limits));
		/* If Boris itself dies, the script goes with it. */
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(child->job, JobObjectExtendedLimitInformation, &limits,
					sizeof(limits));
	}

	/* No console: the script cannot read the user's keyboard, draw on the
	 * terminal behind our back, or be hit by the user's Ctrl+C before we
	 * decide what that means. */
	DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
		      EXTENDED_STARTUPINFO_PRESENT;
	BOOL created;
	if (low_integrity) {
		/* Never fall back to full rights here: the caller was promised a
		 * confined script, and has told the user so. */
		token = low_integrity_token();
		created = token && CreateProcessAsUserW(token, program, command_line, NULL, NULL,
							TRUE, flags, (LPVOID)environment, cwd,
							&startup.StartupInfo, &pi);
	} else {
		created = CreateProcessW(program, command_line, NULL, NULL, TRUE, flags,
					 (LPVOID)environment, cwd, &startup.StartupInfo, &pi);
	}
	if (!created)
		goto done;

	if (child->job)
		AssignProcessToJobObject(child->job, pi.hProcess);
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	child->process = pi.hProcess;
	child->output = out_read;
	out_read = NULL;
	started = true;

	if (stdin_len > 0) {
		DWORD written;
		WriteFile(in_write, stdin_data, (DWORD)stdin_len, &written, NULL);
	}

done:;
	DWORD error = GetLastError(); /* CreateProcess's, for the caller's message */
	close_handle(&token);
	if (!started)
		close_handle(&child->job);
	if (attributes) {
		DeleteProcThreadAttributeList(attributes);
		free(attributes);
	}
	/* The child has its own copies now; ours must go, or the output pipe
	 * would never report end-of-file. */
	close_handle(&out_read);
	close_handle(&out_write);
	close_handle(&in_read);
	close_handle(&in_write);
	SetLastError(error);
	return started;
}

/* Read combined output until end-of-file, the deadline, or cancellation.
 * Returns false if the child must be killed (timeout or Ctrl+C). */
static bool collect_output(HANDLE pipe, struct child_output *out, ULONGLONG deadline)
{
	for (;;) {
		DWORD available = 0;
		if (!PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL))
			return true; /* every writer has closed its end */

		if (available == 0) {
			if (GetTickCount64() >= deadline || tools_cancelled())
				return false;
			Sleep(POLL_SLICE_MS);
			continue;
		}

		char drain[4096];
		size_t space = RUN_MAX_OUTPUT - out->len;
		char *dst = space > 0 ? out->buf + out->len : drain;
		size_t want = space > 0 ? space : sizeof(drain);
		DWORD got = 0;
		if (!ReadFile(pipe, dst, (DWORD)(want < available ? want : available), &got, NULL) ||
		    got == 0)
			return true;
		if (space > 0)
			out->len += got;
		else
			out->truncated = true;
	}
}

/* Wait for the process itself, still honouring the deadline: a script can
 * close its output and keep running. Returns false on timeout/cancel. */
static bool wait_for_exit(HANDLE process, ULONGLONG deadline)
{
	for (;;) {
		if (WaitForSingleObject(process, POLL_SLICE_MS) == WAIT_OBJECT_0)
			return true;
		if (GetTickCount64() >= deadline || tools_cancelled())
			return false;
	}
}

/* ------------------------------------------------------------------------
 * Entry point
 * --------------------------------------------------------------------- */

char *run_win32_script(const char *script, char *const *user_args, int user_argc,
		       char *const *envp, const char *cwd, const char *stdin_data,
		       size_t stdin_len, int timeout_seconds, bool low_integrity,
		       char *problem, size_t problem_size)
{
	char *result = NULL;
	wchar_t *path_var = NULL, *environment = NULL;
	struct command cmd = {malloc(COMMAND_MAX * sizeof(wchar_t)), 0, false};
	struct child_output out = {malloc(RUN_MAX_OUTPUT + 1), 0, false};
	snprintf(problem, problem_size, "Failed to launch process");
	if (!cmd.text || !out.buf)
		goto done;
	cmd.text[0] = L'\0';

	/* Programs are looked up on the PATH the script will be given. */
	const char *path_utf8 = env_value(envp, "PATH");
	if (path_utf8) {
		int n = MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, NULL, 0);
		path_var = n > 0 ? malloc((size_t)n * sizeof(wchar_t)) : NULL;
		if (path_var)
			MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, path_var, n);
	}

	/* Shebang first, then extension - the same order as on POSIX. */
	struct shebang sb;
	bool has_shebang = read_shebang(script, &sb);
	const struct interpreter *interp = NULL;
	wchar_t program[PROGRAM_MAX];
	bool found = false;
	if (has_shebang) {
		interp = interpreter_for_program(sb.program);
		found = find_program(interp, sb.program, path_var, program);
	}
	if (!found) {
		has_shebang = false;
		interp = interpreter_for_extension(script);
		found = interp && find_program(interp, NULL, path_var, program);
	}
	if (!found) {
		if (interp)
			snprintf(problem, problem_size,
				 "Cannot run '%s': no '%s' interpreter was found on PATH",
				 script, interp->programs[0]);
		else
			snprintf(problem, problem_size,
				 "Cannot determine interpreter for '%s'. Use a recognised "
				 "extension: .py .js .ps1 .bat .cmd .sh .rb .lua .pl .php",
				 script);
		goto done;
	}

	/* Native tools, cmd.exe above all, want backslashes in a script path. */
	char script_native[4096];
	snprintf(script_native, sizeof(script_native), "%s", script);
	for (char *p = script_native; *p; p++)
		if (*p == '/')
			*p = '\\';

	enum launch_style style = interp ? interp->style : LAUNCH_DIRECT;
	command_append_argument(&cmd, program);
	if (style == LAUNCH_CMD) {
		bool safe = safe_for_cmd(script_native);
		for (int i = 0; i < user_argc && safe; i++)
			safe = safe_for_cmd(user_args[i]);
		if (!safe) {
			snprintf(problem, problem_size,
				 "Batch files cannot be given a path or arguments containing "
				 "any of \" %% ^ & | < > ! or a line break");
			goto done;
		}
		/* /d: no AutoRun commands. /s /c "...": run exactly what is
		 * between the outer quotes. Every word inside is quoted. */
		command_append(&cmd, L" /d /s /c \"", 11);
		wchar_t wide[4096];
		for (int i = -1; i < user_argc; i++) {
			if (!to_wide(i < 0 ? script_native : user_args[i], wide, 4096)) {
				cmd.overflow = true;
				break;
			}
			command_append(&cmd, i < 0 ? L"\"" : L" \"", i < 0 ? 1 : 2);
			command_append(&cmd, wide, wcslen(wide));
			command_append(&cmd, L"\"", 1);
		}
		command_append(&cmd, L"\"", 1);
	} else {
		if (style == LAUNCH_POWERSHELL) {
			/* -File passes the arguments through as plain strings. The
			 * execution policy is waived for this one process only; a
			 * default installation refuses to run any script at all. */
			static const char *const fixed[] = {"-NoLogo", "-NoProfile",
							    "-NonInteractive", "-ExecutionPolicy",
							    "Bypass", "-File"};
			for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++)
				command_append_utf8(&cmd, fixed[i]);
		}
		for (int i = 0; has_shebang && i < sb.argc; i++)
			command_append_utf8(&cmd, sb.args[i]);
		command_append_utf8(&cmd, script_native);
		for (int i = 0; i < user_argc; i++)
			command_append_utf8(&cmd, user_args[i]);
	}
	if (cmd.overflow) {
		snprintf(problem, problem_size, "Command line too long");
		goto done;
	}

	wchar_t wcwd[4096];
	environment = build_environment_block(envp);
	if (!environment || !to_wide(cwd, wcwd, 4096))
		goto done;

	char program_utf8[PROGRAM_MAX * 3];
	if (!to_utf8(program, program_utf8, (int)sizeof(program_utf8)))
		program_utf8[0] = '\0';
	log_info("run: %s (interpreter=%s, timeout=%ds%s)", script, program_utf8,
		 timeout_seconds, low_integrity ? ", confined" : "");

	struct child child;
	if (!spawn(program, cmd.text, environment, wcwd, stdin_data, stdin_len, low_integrity,
		   &child)) {
		snprintf(problem, problem_size, "Failed to launch '%s' (Windows error %lu)",
			 program_utf8, (unsigned long)GetLastError());
		goto done;
	}

	ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeout_seconds * 1000;
	bool finished = collect_output(child.output, &out, deadline) &&
			wait_for_exit(child.process, deadline);
	bool cancelled = !finished && tools_cancelled();
	out.buf[out.len] = '\0';

	DWORD exit_code = 0;
	if (finished)
		GetExitCodeProcess(child.process, &exit_code);

	/* Kill the whole job: on timeout/cancel, and also after a normal exit
	 * so background processes the script started do not outlive it. */
	if (child.job)
		TerminateJobObject(child.job, 1);
	if (!finished) {
		TerminateProcess(child.process, 1);
		WaitForSingleObject(child.process, 5000);
	}
	close_handle(&child.output);
	close_handle(&child.process);
	close_handle(&child.job);

	result = run_format_result(&out, finished, cancelled, (int)exit_code, timeout_seconds);

done:
	free(path_var);
	free(environment);
	free(cmd.text);
	free(out.buf);
	return result;
}

#endif /* _WIN32 */
