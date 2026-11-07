"""Grading: turn a Check and a TurnContext into a result."""

from __future__ import annotations

import os
import re
import subprocess
from dataclasses import dataclass

from .spec import Check, TurnContext


@dataclass
class CheckResult:
    description: str
    kind: str
    status: str          # pass | fail | error | skip
    detail: str = ""

    def to_dict(self):
        return {"description": self.description, "kind": self.kind,
                "status": self.status, "detail": self.detail}


def _read(path: str):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return None


def _result(check: Check, passed, detail=""):
    status = "pass" if passed else "fail"
    return CheckResult(check.description, check.kind, status, "" if passed else detail)


def run_command(ctx: TurnContext, command: str, timeout: int = 120) -> tuple:
    """Run a hidden-test command in the sandbox, as the agent's scripts would run."""
    try:
        proc = subprocess.run(["sh", "-c", ctx.workspace.expand(command)],
                              cwd=ctx.workspace.sandbox, env=ctx.workspace.env(),
                              capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, f"timed out after {timeout}s"
    return proc.returncode, (proc.stdout + proc.stderr)


def grade(check: Check, ctx: TurnContext, judge=None) -> CheckResult:
    resp = ctx.response or ""
    low = resp.lower()
    c = check

    if c.contains is not None:
        return _result(c, c.contains.lower() in low, f"reply lacks {c.contains!r}")
    if c.not_contains is not None:
        return _result(c, c.not_contains.lower() not in low, f"reply contains {c.not_contains!r}")
    if c.matches is not None:
        return _result(c, re.search(c.matches, resp, re.I | re.S) is not None,
                       f"reply does not match /{c.matches}/")
    if c.not_matches is not None:
        return _result(c, re.search(c.not_matches, resp, re.I | re.S) is None,
                       f"reply matches /{c.not_matches}/")

    if c.file_exists is not None:
        p = ctx.path(c.file_exists)
        return _result(c, os.path.exists(p), f"missing: {c.file_exists}")
    if c.file_not_exists is not None:
        p = ctx.path(c.file_not_exists)
        return _result(c, not os.path.lexists(p), f"exists: {c.file_not_exists}")
    if c.file_contains is not None:
        path, needle = c.file_contains
        text = _read(ctx.path(path))
        return _result(c, text is not None and needle.lower() in text.lower(),
                       f"{path} {'missing' if text is None else 'lacks ' + repr(needle)}")
    if c.file_not_contains is not None:
        path, needle = c.file_not_contains
        text = _read(ctx.path(path))
        return _result(c, text is not None and needle.lower() not in text.lower(),
                       f"{path} {'missing' if text is None else 'still contains ' + repr(needle)}")
    if c.file_equals is not None:
        path, expected = c.file_equals
        text = _read(ctx.path(path))
        return _result(c, text is not None and text.strip() == expected.strip(),
                       f"{path} differs from the expected content")
    if c.unchanged is not None:
        orig = _read(ctx.workspace.fixture_path(c.unchanged))
        now = _read(ctx.path(c.unchanged))
        return _result(c, orig is not None and orig == now, f"{c.unchanged} was modified")

    if c.used_tool is not None:
        return _result(c, c.used_tool in ctx.tools,
                       f"did not call {c.used_tool} (called: {', '.join(ctx.tools) or 'none'})")
    if c.not_used_tool is not None:
        return _result(c, c.not_used_tool not in ctx.tools,
                       f"called {c.not_used_tool} (called: {', '.join(ctx.tools)})")
    if c.max_tool_calls is not None:
        n = len(ctx.tool_calls)
        return _result(c, n <= c.max_tool_calls, f"{n} tool calls (budget {c.max_tool_calls})")
    if c.max_iterations is not None:
        n = ctx.report.get("iterations", 0)
        return _result(c, n <= c.max_iterations, f"{n} iterations (budget {c.max_iterations})")
    if c.finish_reason is not None:
        got = ctx.report.get("finish_reason")
        return _result(c, got == c.finish_reason, f"finish_reason {got!r}")

    if c.command is not None:
        code, out = run_command(ctx, c.command)
        if code is None:
            return CheckResult(c.description, c.kind, "fail", out)
        ok = code == 0 and (c.command_output is None or
                            re.search(c.command_output, out, re.I | re.S) is not None)
        tail = out.strip().splitlines()[-3:]
        return _result(c, ok, f"exit {code}: {' | '.join(tail)[:300]}")

    if c.fn is not None:
        try:
            passed, detail = c.fn(ctx)
        except Exception as e:  # a broken check is the harness's fault
            return CheckResult(c.description, c.kind, "error", f"check raised {e!r}")
        return _result(c, passed, detail)

    if c.judge is not None:
        if judge is None:
            return CheckResult(c.description, c.kind, "skip", "run with --judge")
        status, reason = judge.grade(c.judge, ctx.turn.message, resp, ctx.tool_calls)
        return CheckResult(c.description, c.kind, status, reason)

    return CheckResult(c.description, c.kind, "error", "check has no criterion")
