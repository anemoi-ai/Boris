#!/usr/bin/env python3
"""
Check prompt-prefix stability in a Boris request trace.

Prompt caching only pays off when each request starts with exactly the same
tokens as the one before it. This reads a trace written by `boris --trace
FILE` and, for each request after the first, reports whether the tool
definitions are unchanged and how many leading messages it shares with the
previous request.

Usage:
    python3 evals/trace_prefix.py TRACE.jsonl [--verbose]

Exit status: 0 if every request extends the previous one (append-only
history, unchanged tools), 1 if any request diverges, 2 on bad input.
"""

import argparse
import json
import sys


def load_requests(path):
    requests = []
    with open(path, encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                entry = json.loads(line)
            except json.JSONDecodeError as e:
                raise SystemExit(f"{path}:{lineno}: not JSON ({e})")
            if entry.get("kind") == "request" and isinstance(entry.get("body"), dict):
                requests.append(entry)
    return requests


def canonical(value):
    """Serialise exactly as compared: key order matters to a cache."""
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def shared_prefix(prev, cur):
    n = 0
    for a, b in zip(prev, cur):
        if canonical(a) != canonical(b):
            break
        n += 1
    return n


def describe(message):
    role = message.get("role", "?")
    content = message.get("content") or ""
    if not isinstance(content, str):
        content = canonical(content)
    if message.get("tool_calls"):
        names = ",".join(tc.get("function", {}).get("name", "?")
                         for tc in message["tool_calls"])
        return f"{role} -> {names}"
    return f"{role}: {content[:60]!r}"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("trace")
    parser.add_argument("--verbose", action="store_true",
                        help="show the first differing message on divergence")
    args = parser.parse_args()

    try:
        requests = load_requests(args.trace)
    except OSError as e:
        print(f"cannot read {args.trace}: {e}", file=sys.stderr)
        return 2
    if len(requests) < 2:
        print(f"{len(requests)} request(s) in the trace; need at least 2 to compare.")
        return 0

    diverged = 0
    print(f"{'seq':>4}  {'msgs':>4}  {'shared':>6}  {'tools':<9}  verdict")
    for prev, cur in zip(requests, requests[1:]):
        pm = prev["body"].get("messages", [])
        cm = cur["body"].get("messages", [])
        same_tools = canonical(prev["body"].get("tools")) == canonical(cur["body"].get("tools"))
        shared = shared_prefix(pm, cm)
        extends = same_tools and shared == len(pm)
        if not extends:
            diverged += 1
        if extends:
            verdict = "extends previous"
        elif not same_tools:
            verdict = "TOOLS CHANGED"
        else:
            verdict = f"DIVERGES at message {shared}"
        print(f"{cur.get('seq', '?'):>4}  {len(cm):>4}  {shared:>6}  "
              f"{'same' if same_tools else 'changed':<9}  {verdict}")
        if args.verbose and not extends and shared < len(pm):
            print(f"        was: {describe(pm[shared])}")
            if shared < len(cm):
                print(f"        now: {describe(cm[shared])}")

    total = len(requests) - 1
    print(f"\n{total - diverged}/{total} requests extend the previous one.")
    return 1 if diverged else 0


if __name__ == "__main__":
    sys.exit(main())
