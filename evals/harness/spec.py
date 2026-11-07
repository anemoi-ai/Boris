"""
The shape of an eval: cases, turns and checks.

A case runs in its own throwaway HOME (see workspace.py). Each turn is one
`boris exec --json --session ... --trace ...` invocation, so every turn has
a JSON report and every request is in the trace. Checks grade a turn; the
harness also adds safety checks of its own to every case (no changes
outside the sandbox, no canary leaks).

Check kinds:
  correctness  the task was done (default)
  safety       nothing harmful happened
  efficiency   it was done economically (reported, never fails a case)
  info         recorded for the report only
A case passes when every correctness and safety check passes.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Callable, Optional

KINDS = ("correctness", "safety", "efficiency", "info")
GATING_KINDS = ("correctness", "safety")


@dataclass
class Check:
    description: str
    kind: str = "correctness"

    # Reply text (case-insensitive substring / regex)
    contains: Optional[str] = None
    not_contains: Optional[str] = None
    matches: Optional[str] = None
    not_matches: Optional[str] = None

    # Files. Relative paths are inside the sandbox; {SANDBOX}, {OUTSIDE}
    # and {HOME} placeholders are expanded.
    file_exists: Optional[str] = None
    file_not_exists: Optional[str] = None
    file_contains: Optional[tuple] = None       # (path, substring)
    file_not_contains: Optional[tuple] = None   # (path, substring)
    file_equals: Optional[tuple] = None         # (path, exact text)
    unchanged: Optional[str] = None             # fixture file left as it was

    # What the run did (from the exec report)
    used_tool: Optional[str] = None
    not_used_tool: Optional[str] = None
    max_tool_calls: Optional[int] = None
    max_iterations: Optional[int] = None
    finish_reason: Optional[str] = None

    # Hidden test: a shell command the harness runs in the sandbox after the
    # turn. Passes on exit status 0 (and, if given, stdout matching the regex).
    command: Optional[str] = None
    command_output: Optional[str] = None

    # Anything else: fn(ctx) -> (passed, detail)
    fn: Optional[Callable[["TurnContext"], tuple]] = None

    # LLM judge (run with --judge; otherwise skipped)
    judge: Optional[str] = None


@dataclass
class Turn:
    message: str
    checks: list = field(default_factory=list)
    new_session: bool = False      # start a fresh conversation for this turn
    flags: list = field(default_factory=list)
    timeout: Optional[int] = None  # seconds; defaults to the case/run timeout


@dataclass
class EvalCase:
    id: str
    description: str
    turns: list
    tags: list = field(default_factory=list)
    flags: list = field(default_factory=list)        # extra boris options
    timeout: Optional[int] = None                    # per turn
    fixture: Optional[str] = None                    # evals/fixtures/<name> -> sandbox
    setup_files: dict = field(default_factory=dict)  # path -> content
    outside_files: dict = field(default_factory=dict)  # {OUTSIDE}/path -> content
    setup: Optional[Callable[[Any], None]] = None    # setup(workspace)
    requires: list = field(default_factory=list)     # commands that must exist
    faults: list = field(default_factory=list)       # fault-proxy plan, see servers.py
    serve: dict = field(default_factory=dict)        # static server: path -> content
    allow_changes: list = field(default_factory=list)  # globs (relative to the workspace root)
    expect_error: bool = False  # the run is meant to fail (e.g. retries exhausted)


@dataclass
class TurnContext:
    """What a check can look at."""
    case: EvalCase
    turn: Turn
    index: int
    workspace: Any
    response: str
    report: dict                 # boris exec --json report ({} if none)
    tools: list                  # names of tools called this turn
    tool_calls: list             # [{name, summary, is_error}]
    requests: list               # trace request bodies for this turn
    exit_code: Optional[int]
    stderr: str
    proxy_log: list = field(default_factory=list)

    def path(self, p: str) -> str:
        return self.workspace.resolve(p)
