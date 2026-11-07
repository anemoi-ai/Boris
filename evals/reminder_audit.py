#!/usr/bin/env python3
"""
Audit the self-correction reminders in eval runs: which ones fired, whether
they were justified, and what they cost.

Usage:
    python3 evals/reminder_audit.py RESULTS_DIR                # one run
    python3 evals/reminder_audit.py BEFORE_DIR AFTER_DIR       # compare two
    options: --cases a,b,c   only these case ids
             --json FILE      write the full audit as JSON

It reads each trial's trace (every request and response) and its exec
reports (which tool calls failed), rebuilds the iterations, and for every
reminder Boris injected records:

  window     the 3 iterations before it. An iteration made progress if any
             call succeeded and either changed something (write, edit, move,
             delete, memory, plan) or was new in this trial (a call with
             arguments not seen before).
  verdict    drift and repeat reminders fired while the agent was still
             making progress are "waste"; after 3 iterations without
             progress they are "justified". Error, truncation and
             continuation reminders are judged by their own triggers.
  reaction   what the model did next: kept using tools, or replied with
             text, which ends the run in Boris's loop ("stopped").
  overhead   the reminder's tokens times the requests that carried it.
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import sys
from collections import Counter, defaultdict

MUTATING = {"write", "edit", "move", "delete", "memory", "plan"}
WINDOW = 3

KINDS = [
    ("tool_errors", "consecutive tool calls have failed"),
    ("repeated_calls", "same tool call"),
    ("stalled", "have not changed anything or found anything new"),
    ("drift", "without summarising"),
    ("budget", "iterations left"),
    ("truncation", "conversation was truncated"),
    ("text_fallback", "tool calls as text"),
    ("continuation", "cut off by the max_tokens"),
]
JUDGED_BY_PROGRESS = {"drift", "stalled", "repeated_calls"}


def kind_of(text: str) -> str:
    return next((k for k, s in KINDS if s in text), "other")


def load_results(path: str) -> list:
    if os.path.isdir(path):
        path = os.path.join(path, "results.jsonl")
    with open(path, encoding="utf-8") as f:
        return [json.loads(line) for line in f if line.strip()]


def load_trace(path: str) -> list:
    """[(request_body, response_body)] in order."""
    pairs, pending = [], None
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                try:
                    e = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if e.get("kind") == "request":
                    pending = e.get("body") if isinstance(e.get("body"), dict) else {}
                elif e.get("kind") == "response" and pending is not None:
                    pairs.append((pending, e.get("body")))
                    pending = None
    except OSError:
        pass
    return pairs


def reply_of(body) -> dict:
    """The assistant message and usage from a response body."""
    if not isinstance(body, dict):
        return {"tool_calls": [], "content": "", "completion_tokens": 0}
    choice = (body.get("choices") or [{}])[0]
    msg = choice.get("message") or {}
    calls = [(c.get("function", {}).get("name", ""), c.get("function", {}).get("arguments", ""))
             for c in msg.get("tool_calls") or []]
    usage = body.get("usage") or {}
    return {"tool_calls": calls, "content": msg.get("content") or "",
            "completion_tokens": usage.get("completion_tokens", 0) or 0}


def audit_trial(record: dict) -> dict:
    pairs = load_trace(record.get("trace", ""))
    errors = [bool(c.get("is_error")) for t in record.get("turns", []) for c in t.get("tool_calls", [])]
    err_iter = iter(errors)
    seen = set()
    iterations = []      # per response: {"progress": bool, "calls": n}
    events = []
    prev_messages = []

    for i, (req, resp) in enumerate(pairs):
        msgs = req.get("messages", [])
        # Reminders that appeared in this request but not in the previous one
        fresh = msgs[len(prev_messages):] if msgs[:len(prev_messages)] == prev_messages else msgs[-3:]
        for m in fresh:
            c = m.get("content")
            if m.get("role") == "user" and isinstance(c, str) and c.startswith("<system-reminder>"):
                kind = kind_of(c)
                window = iterations[-WINDOW:]
                progress = any(it["progress"] for it in window)
                remaining = sum(1 for (r, _) in pairs[i:]
                                if any(m2.get("content") == c for m2 in r.get("messages", [])))
                events.append({
                    "kind": kind, "iteration": i + 1,
                    "window_progress": progress,
                    "verdict": ("waste" if progress else "justified") if kind in JUDGED_BY_PROGRESS else "trigger",
                    "reaction": None,  # filled from this request's response below
                    "overhead_tokens": len(c) // 4 * remaining,
                })
        prev_messages = msgs

        r = reply_of(resp)
        for ev in events:
            if ev["iteration"] == i + 1 and ev["reaction"] is None:
                ev["reaction"] = "continued" if r["tool_calls"] else "stopped"
                ev["reaction_tokens"] = r["completion_tokens"]

        made_progress = False
        for name, args in r["tool_calls"]:
            failed = next(err_iter, False)
            fp = (name, args)
            new = fp not in seen
            seen.add(fp)
            if not failed and (name in MUTATING or new):
                made_progress = True
        iterations.append({"progress": made_progress, "calls": len(r["tool_calls"])})

    m = record.get("metrics", {})
    return {"case": record["case"], "trial": record["trial"], "status": record["status"],
            "events": events, "iterations": len(pairs),
            "tool_calls": m.get("tool_calls", 0),
            "tokens": (m.get("prompt_tokens", 0) or 0) + (m.get("completion_tokens", 0) or 0),
            "wall_s": m.get("wall_s", 0),
            "hit_max_iterations": any(t.get("finish_reason") == "max_iterations"
                                      for t in record.get("turns", []))}


def summarise(trials: list) -> dict:
    events = [e for t in trials for e in t["events"]]
    by_kind = defaultdict(Counter)
    for e in events:
        by_kind[e["kind"]]["fired"] += 1
        by_kind[e["kind"]][e["verdict"]] += 1
        by_kind[e["kind"]][e["reaction"] or "unknown"] += 1
        by_kind[e["kind"]]["overhead_tokens"] += e["overhead_tokens"]
    stopped_failed = sum(1 for t in trials for e in t["events"]
                         if e["reaction"] == "stopped" and t["status"] != "pass")
    med = lambda k: statistics.median([t[k] for t in trials]) if trials else 0  # noqa: E731
    return {
        "trials": len(trials),
        "passed": sum(t["status"] == "pass" for t in trials),
        "reminders": len(events),
        "reminders_per_trial": len(events) / len(trials) if trials else 0,
        "waste": sum(e["verdict"] == "waste" for e in events),
        "justified": sum(e["verdict"] == "justified" for e in events),
        "stopped_after_reminder": sum(e["reaction"] == "stopped" for e in events),
        "stopped_and_trial_failed": stopped_failed,
        "overhead_tokens": sum(e["overhead_tokens"] for e in events),
        "hit_max_iterations": sum(t["hit_max_iterations"] for t in trials),
        "median_tool_calls": med("tool_calls"),
        "median_iterations": med("iterations"),
        "median_tokens": med("tokens"),
        "median_wall_s": med("wall_s"),
        "by_kind": {k: dict(v) for k, v in by_kind.items()},
    }


def audit_dir(path: str, cases: set | None) -> tuple:
    records = [r for r in load_results(path) if r.get("trace") and r["status"] in ("pass", "fail")]
    if cases:
        records = [r for r in records if r["case"] in cases]
    trials = [audit_trial(r) for r in records]
    return trials, summarise(trials)


def print_summary(label: str, s: dict):
    print(f"\n== {label}")
    print(f"  {s['trials']} trials, {s['passed']} passed")
    print(f"  reminders: {s['reminders']} ({s['reminders_per_trial']:.2f} per trial); "
          f"progress-judged: {s['waste']} waste, {s['justified']} justified")
    print(f"  model stopped right after a reminder: {s['stopped_after_reminder']} "
          f"(of which the trial failed: {s['stopped_and_trial_failed']})")
    print(f"  reminder context overhead: {s['overhead_tokens']:,} tokens; "
          f"runs that hit max_iterations: {s['hit_max_iterations']}")
    print(f"  medians: {s['median_tool_calls']} tool calls, {s['median_iterations']} iterations, "
          f"{s['median_tokens']:,} tokens, {s['median_wall_s']:.0f}s")
    for k, c in sorted(s["by_kind"].items()):
        detail = ", ".join(f"{n} {v}" for n, v in c.items() if n not in ("fired", "overhead_tokens"))
        print(f"    {k:<15} fired {c['fired']:>3}  ({detail})")


def print_compare(a: dict, b: dict):
    rows = [("reminders per trial", "reminders_per_trial", "{:.2f}"),
            ("wasted (fired during progress)", "waste", "{}"),
            ("justified", "justified", "{}"),
            ("stopped right after a reminder", "stopped_after_reminder", "{}"),
            ("reminder overhead (tokens)", "overhead_tokens", "{:,}"),
            ("runs hitting max_iterations", "hit_max_iterations", "{}"),
            ("trials passed", "passed", "{}"),
            ("median tool calls", "median_tool_calls", "{}"),
            ("median iterations", "median_iterations", "{}"),
            ("median tokens", "median_tokens", "{:,}"),
            ("median wall time (s)", "median_wall_s", "{:.0f}")]
    print(f"\n{'':34} {'before':>12} {'after':>12}")
    for label, key, fmt in rows:
        print(f"  {label:<32} {fmt.format(a[key]):>12} {fmt.format(b[key]):>12}")
    print(f"  {'trials':<32} {a['trials']:>12} {b['trials']:>12}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("dirs", nargs="+", help="one results dir, or two to compare")
    ap.add_argument("--cases", help="comma list of case ids")
    ap.add_argument("--json", help="write the audit to this file")
    args = ap.parse_args()
    if len(args.dirs) > 2:
        ap.error("give one or two result directories")
    cases = set(args.cases.split(",")) if args.cases else None

    audits = [audit_dir(d, cases) for d in args.dirs]
    for d, (_, s) in zip(args.dirs, audits):
        print_summary(os.path.basename(os.path.normpath(d)), s)
    if len(audits) == 2:
        print_compare(audits[0][1], audits[1][1])
    if args.json:
        with open(args.json, "w") as f:
            json.dump([{"dir": d, "summary": s, "trials": t} for d, (t, s) in zip(args.dirs, audits)],
                      f, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
