"""Results on disk, statistics, the Markdown report and run comparison."""

from __future__ import annotations

import json
import math
import os
import statistics
from collections import defaultdict


def wilson(passes: int, n: int, z: float = 1.96) -> tuple:
    """95% Wilson score interval for a pass rate."""
    if n == 0:
        return (0.0, 0.0)
    p = passes / n
    denom = 1 + z * z / n
    centre = (p + z * z / (2 * n)) / denom
    half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / denom
    return (max(0.0, centre - half), min(1.0, centre + half))


def load(path: str) -> list:
    if os.path.isdir(path):
        path = os.path.join(path, "results.jsonl")
    with open(path, encoding="utf-8") as f:
        return [json.loads(line) for line in f if line.strip()]


def _median(values):
    values = [v for v in values if v is not None]
    return statistics.median(values) if values else None


def summarise(records: list) -> dict:
    by_case = defaultdict(list)
    for r in records:
        by_case[r["case"]].append(r)

    cases = {}
    for cid, rs in by_case.items():
        counted = [r for r in rs if r["status"] in ("pass", "fail", "error")]
        passes = sum(r["status"] == "pass" for r in counted)
        n = len(counted)
        eff = [c for r in counted for t in r["turns"] for c in t["checks"] if c["kind"] == "efficiency"]
        failed_checks = sorted({c["description"] for r in counted
                                for c in [*(c for t in r["turns"] for c in t["checks"]), *r["safety"]]
                                if c["kind"] in ("correctness", "safety") and c["status"] == "fail"})
        cases[cid] = {
            "tags": rs[0].get("tags", []),
            "description": rs[0].get("description", ""),
            "trials": len(rs), "counted": n, "passes": passes,
            "pass_rate": passes / n if n else None,
            "ci95": wilson(passes, n) if n else None,
            "flaky": 0 < passes < n,
            "infra": sum(r["status"] == "infra" for r in rs),
            "skipped": sum(r["status"] == "skipped" for r in rs),
            "skip_reason": next((r.get("reason") for r in rs if r["status"] == "skipped"), None),
            "safety_failures": sum(any(c["kind"] == "safety" and c["status"] == "fail"
                                       for c in [*r["safety"], *(c for t in r["turns"] for c in t["checks"])])
                                   for r in counted),
            "efficiency": {"passed": sum(c["status"] == "pass" for c in eff), "total": len(eff)},
            "failed_checks": failed_checks,
            "median": {k: _median([r["metrics"].get(k) for r in counted if r.get("metrics")])
                       for k in ("wall_s", "tool_calls", "tool_errors", "iterations",
                                 "prompt_tokens", "completion_tokens")},
            "reminders": _sum_dicts([r.get("metrics", {}).get("reminders", {}) for r in counted]),
            "prefix": (sum(r.get("metrics", {}).get("prefix_extends", 0) for r in counted),
                       sum(r.get("metrics", {}).get("prefix_pairs", 0) for r in counted)),
        }

    tags = defaultdict(lambda: [0, 0])
    for c in cases.values():
        for t in c["tags"]:
            tags[t][0] += c["passes"]
            tags[t][1] += c["counted"]

    counted = [c for c in cases.values() if c["counted"]]
    total_p = sum(c["passes"] for c in counted)
    total_n = sum(c["counted"] for c in counted)
    pe, pp = (sum(c["prefix"][0] for c in counted), sum(c["prefix"][1] for c in counted))
    return {
        "cases": cases,
        "tags": {t: {"passes": p, "trials": n, "pass_rate": p / n if n else None}
                 for t, (p, n) in sorted(tags.items())},
        "totals": {
            "cases": len(cases),
            "cases_all_pass": sum(c["passes"] == c["counted"] > 0 for c in counted),
            "trials": total_n, "passes": total_p,
            "pass_rate": total_p / total_n if total_n else None,
            "ci95": wilson(total_p, total_n),
            "flaky_cases": sorted(cid for cid, c in cases.items() if c["flaky"]),
            "safety_failures": sum(c["safety_failures"] for c in cases.values()),
            "infra_trials": sum(c["infra"] for c in cases.values()),
            "skipped_cases": sorted(cid for cid, c in cases.items() if c["skipped"] and not c["counted"]),
            "prefix_extends": pe, "prefix_pairs": pp,
            "tokens": sum(r.get("metrics", {}).get("prompt_tokens", 0) +
                          r.get("metrics", {}).get("completion_tokens", 0) for r in records),
            "wall_s": round(sum(r.get("metrics", {}).get("wall_s", 0) for r in records), 1),
        },
    }


def _sum_dicts(ds):
    out = defaultdict(int)
    for d in ds:
        for k, v in d.items():
            out[k] += v
    return dict(out)


def _pct(x):
    return "–" if x is None else f"{100 * x:.0f}%"


def markdown(summary: dict, meta: dict) -> str:
    t = summary["totals"]
    lo, hi = t["ci95"]
    lines = [
        f"# Boris eval report: {meta.get('model')}",
        "",
        f"- **When:** {meta.get('started')} · **Boris:** {meta.get('boris_version')} "
        f"(commit {meta.get('commit')})",
        f"- **Endpoint:** {meta.get('endpoint')} · **context length:** {meta.get('context_length') or 'unknown'}"
        f" · **judge:** {meta.get('judge') or 'off'}",
        f"- **Repeats:** {meta.get('repeat')} · **temperature:** {meta.get('temperature')}",
        "",
        "## Summary",
        "",
        f"- **Pass rate:** {t['passes']}/{t['trials']} trials ({_pct(t['pass_rate'])}, "
        f"95% CI {_pct(lo)}–{_pct(hi)}); {t['cases_all_pass']}/{t['cases']} cases passed every trial",
        f"- **Safety violations:** {t['safety_failures']}",
        f"- **Flaky cases:** {', '.join(t['flaky_cases']) or 'none'}",
        f"- **Infrastructure errors (excluded):** {t['infra_trials']} trials",
        f"- **Skipped:** {', '.join(t['skipped_cases']) or 'none'}",
        f"- **Prompt prefix:** {t['prefix_extends']}/{t['prefix_pairs']} requests extended the previous one",
        f"- **Totals:** {t['tokens']:,} tokens, {t['wall_s'] / 60:.0f} minutes",
        "",
        "## By tag",
        "",
        "| Tag | Pass rate | Trials |",
        "|---|---|---|",
    ]
    for tag, v in summary["tags"].items():
        lines.append(f"| {tag} | {_pct(v['pass_rate'])} | {v['passes']}/{v['trials']} |")
    lines += ["", "## Cases", "",
              "| Case | Pass | Tools (median) | Tokens (median) | Time (median) | Efficiency | Notes |",
              "|---|---|---|---|---|---|---|"]
    for cid, c in sorted(summary["cases"].items()):
        if not c["counted"]:
            lines.append(f"| {cid} | skipped | | | | | {c['skip_reason'] or 'infrastructure error'} |")
            continue
        m = c["median"]
        tok = (m["prompt_tokens"] or 0) + (m["completion_tokens"] or 0)
        eff = c["efficiency"]
        notes = []
        if c["flaky"]:
            notes.append("flaky")
        if c["safety_failures"]:
            notes.append(f"**{c['safety_failures']} safety failure(s)**")
        if c["failed_checks"]:
            notes.append("failed: " + "; ".join(c["failed_checks"][:3]))
        if c["reminders"]:
            notes.append("reminders: " + ", ".join(f"{k}×{v}" for k, v in c["reminders"].items()))
        tools = "–" if m["tool_calls"] is None else m["tool_calls"]
        eff_text = "%d/%d" % (eff["passed"], eff["total"]) if eff["total"] else "–"
        lines.append(
            f"| {cid} | {c['passes']}/{c['counted']} | {tools} | {tok:,} | "
            f"{m['wall_s'] or 0:.0f}s | {eff_text} | {' · '.join(notes)} |")
    return "\n".join(lines) + "\n"


def compare(old_path: str, new_path: str) -> str:
    """What changed between two runs: per-case pass rates, cost and speed."""
    old, new = summarise(load(old_path)), summarise(load(new_path))
    rows, regressions, improvements = [], [], []
    for cid in sorted(set(old["cases"]) | set(new["cases"])):
        a, b = old["cases"].get(cid), new["cases"].get(cid)
        if not a or not b or not a["counted"] or not b["counted"]:
            rows.append(f"| {cid} | {_pct(a and a['pass_rate'])} | {_pct(b and b['pass_rate'])} | | | only in one run |")
            continue
        d = b["pass_rate"] - a["pass_rate"]
        note = ""
        # Call it a regression only when the intervals do not overlap, or it
        # went from always passing to failing at all.
        if b["ci95"][1] < a["ci95"][0] or (a["pass_rate"] == 1 and b["pass_rate"] < 1):
            note = "REGRESSION"
            regressions.append(cid)
        elif a["ci95"][1] < b["ci95"][0] or (b["pass_rate"] == 1 and a["pass_rate"] < 1):
            note = "improved"
            improvements.append(cid)
        ta = (a["median"]["prompt_tokens"] or 0) + (a["median"]["completion_tokens"] or 0)
        tb = (b["median"]["prompt_tokens"] or 0) + (b["median"]["completion_tokens"] or 0)
        rows.append(f"| {cid} | {_pct(a['pass_rate'])} | {_pct(b['pass_rate'])} | {d:+.0%} | "
                    f"{tb - ta:+,} | {note} |")
    ot, nt = old["totals"], new["totals"]
    head = [f"Overall: {_pct(ot['pass_rate'])} -> {_pct(nt['pass_rate'])}; safety violations "
            f"{ot['safety_failures']} -> {nt['safety_failures']}; tokens {ot['tokens']:,} -> {nt['tokens']:,}",
            f"Regressions: {', '.join(regressions) or 'none'}",
            f"Improvements: {', '.join(improvements) or 'none'}", "",
            "| Case | Before | After | Δ pass | Δ tokens (median) | |", "|---|---|---|---|---|---|"]
    return "\n".join(head + rows) + "\n"
