# Boris on Windows

Boris is written against POSIX. The Windows port keeps it that way: the code
in `src/` still calls `stat()`, `realpath()`, `opendir()` and `sigaction()`,
and one header decides what those names mean.

## Building

Visual Studio 2022 (MSVC or clang-cl), CMake 3.20+, and libcurl from vcpkg.
Windows 10 version 1903 or later.

```powershell
vcpkg install curl:x64-windows
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The result is `build\Release\boris.exe`, with the libcurl and zlib DLLs
copied next to it. The code compiles clean under `/W4 /WX`. MinGW is not
supported: it brings half of POSIX with it, and that half would collide with
the shims.

The Makefile remains the build for Linux and macOS.

## How the port is put together

| File | What it is |
|---|---|
| `include/platform.h` | On POSIX, includes the system headers. On Windows, declares the subset of POSIX that Boris uses. |
| `src/platform_win32.c` | That subset over Win32: files, directories, `realpath`, `flock`, `getopt_long`, SIGINT, and the console. |
| `src/platform_regex.c` | `fnmatch()` and POSIX extended regular expressions for the `search` tool. A Thompson NFA: no backtracking, so no pattern can make it slow. |
| `src/tools/run_win32.c` | The `run` tool's launcher: interpreter lookup, command-line quoting, `CreateProcess`, job object. |
| `windows/boris.manifest` | Makes the C runtime UTF-8 and allows long paths. |

Three conventions hold everywhere:

- **Strings are UTF-8.** The shims convert to UTF-16 at each system call, and
  the manifest makes `fopen`, `getenv` and `argv` UTF-8 too.
- **Canonical paths use forward slashes** (`C:/Users/me/project`). Win32
  accepts them, and it means the sandbox's prefix check and every
  `strrchr(path, '/')` are the same code on every system. Backslashes in
  input are accepted and converted.
- **Files are binary.** `"\n"` written is `"\n"` on disk (the executables link
  `binmode.obj`).

## The sandbox

`realpath()` is `GetFinalPathNameByHandle`: the name the system reports for
the opened object, with symbolic links, junctions, 8.3 short names and letter
case all resolved. `lstat()` reports both symbolic links and junctions as
links, so everything that refuses to follow a link refuses both.

Win32 rewrites some names before the filesystem sees them, so
`sandbox_resolve()` refuses the ones that cannot be reasoned about as strings:

| Refused | Why |
|---|---|
| `C:file`, `file:stream` | drive-relative paths; alternate data streams |
| `\\.\x`, `\\?\x` | the device namespace |
| `NUL`, `CON`, `COM1.txt` | device names exist in every directory |
| `name.`, `name `, `.. ` | trailing dots and spaces are stripped, so `.. ` is `..` |

One difference from POSIX: Win32 removes `..` from a path lexically, before
any lookup. `missing/../x` is therefore simply `x` on Windows, where POSIX
refuses it. Nothing can be followed later, because the resolved path that
tools then use contains no `..`.

## The run tool

- **Interpreter.** A `#!` line is honoured by name (`#!/usr/bin/env python3`
  means "the Python on PATH"); otherwise the extension decides: `.py .js .ps1
  .bat .cmd .sh .rb .lua .pl .php`. Store "app execution alias" stubs and
  the WSL `bash.exe` are passed over; for `.sh`, the shell that comes with Git
  for Windows is found even though it is not on PATH.
- **Arguments** are quoted so the child's `argv` is byte-for-byte what the
  model sent. Batch files are the exception: `cmd.exe` has no safe quoting,
  so arguments containing `" % ^ & | < > !` or a line break are refused.
- **Isolation.** The child gets an allowlisted environment (never `BORIS_*`,
  matched without regard to case), exactly three inherited handles, and no
  console. It runs in a job object, so a timeout or Ctrl+C kills the script
  and everything it started.
- `PYTHONUTF8=1` is set so Python scripts read and write UTF-8, as they do
  on Linux.

## The console

Raw mode uses the console's virtual-terminal input, so keys arrive as the
same ANSI sequences `editor.c` already parses. Input is read as UTF-16 and
converted. Ctrl+C at the prompt is read as a key; during a model call it
arrives as a console control event and sets the same flag SIGINT does. The
spinner is drawn by a thread, there being no SIGALRM; in the classic console
host, whose fonts have no braille, it falls back to `|/-\`.

The mintty terminal of Git Bash is a pipe, not a console: there Boris reads
lines without editing and prints without colour. Use Windows Terminal or the
classic console, or run it as `winpty boris chat`.

## What is different

- `~` and `~/.boris` are under `%USERPROFILE%` (or `%HOME%` if set).
- There are no permission bits. `config.ini`, which may hold the API key, is
  protected by the ACL of your profile directory rather than mode 0600.
- Creating symbolic links needs Developer Mode; tests that need file links
  skip themselves without it. Junctions need nothing and are tested.
- The eval suite's cases use `sh` scripts and need Git for Windows.
