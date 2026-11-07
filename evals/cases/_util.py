"""Helpers shared by the case modules."""

import os
import subprocess
import textwrap

from harness.spec import Check

RETRY_FLAGS = ["--set", "agent.max_retries=3", "--set", "agent.retry_backoff_ms=200"]


def py(code: str) -> str:
    """A hidden test: run this Python in the sandbox (passes on exit 0)."""
    return "python3 - <<'PY'\n" + textwrap.dedent(code).strip() + "\nPY\n"


def read(ctx, rel: str):
    try:
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return None


def run_in_sandbox(ctx, argv: list, stdin: str = "") -> tuple:
    proc = subprocess.run(argv, cwd=ctx.workspace.sandbox, env=ctx.workspace.env(),
                          input=stdin, capture_output=True, text=True, timeout=60)
    return proc.returncode, proc.stdout, proc.stderr


def lines_equal(rel: str, expected: list, description: str) -> Check:
    def fn(ctx):
        text = read(ctx, rel)
        if text is None:
            return False, f"{rel} is missing"
        got = [l.strip() for l in text.splitlines() if l.strip()]
        return got == expected, f"{rel} has {len(got)} lines; first differences: " + \
            str([(i, g, e) for i, (g, e) in enumerate(zip(got, expected)) if g != e][:3])
    return Check(description, fn=fn)


def reminder_fired(kind_text: str, description: str, kind: str = "info") -> Check:
    def fn(ctx):
        for body in ctx.requests:
            for m in body.get("messages", []):
                c = m.get("content")
                if isinstance(c, str) and c.startswith("<system-reminder>") and kind_text in c:
                    return True, ""
        return False, "no such reminder in the trace"
    return Check(description, kind=kind, fn=fn)


def proxy_requests_at_least(n: int) -> Check:
    def fn(ctx):
        got = len(ctx.proxy_log)
        return got >= n, f"{got} requests reached the proxy: {ctx.proxy_log}"
    return Check(f"Retried through the injected faults (>= {n} requests)", kind="info", fn=fn)


def exists(path: str) -> bool:
    return os.path.lexists(path)
