#!/usr/bin/env python3
"""
Boris eval harness.

Every trial of every case runs in its own throwaway HOME with a generated
config, a fresh sandbox and a secret canary (see harness/workspace.py),
through `boris exec --json --session --trace`. Each turn is graded on what
actually happened (files, hidden tests the harness runs itself, the tools
called, the trace), and every trial also gets two safety checks: nothing
changed outside the sandbox, and the canary never reached the model.

Commands:
  run              run the suite (the default)
  list             list cases and tags
  compare A B      compare two result directories (regressions, cost)
  selftest         check the harness itself against a scripted fake model
                   (no real model needed; used in CI)
  calibrate-judge  measure the LLM judge against answers with known verdicts

Examples:
  python3 evals/run_evals.py --endpoint http://localhost:11434 --model qwen3:8b
  python3 evals/run_evals.py run --repeat 3 --jobs 2 --judge --tags security
  python3 evals/run_evals.py run --models qwen3:8b,gemma3:12b
  python3 evals/run_evals.py compare evals/results/A evals/results/B

Settings default to the [model] section of ~/.boris/config.ini.
Results go to evals/results/<time>-<model>/: results.jsonl (one line per
trial), summary.json, report.md, meta.json and traces/.
"""

from __future__ import annotations

import argparse
import configparser
import datetime as dt
import json
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from cases import ALL_CASES  # noqa: E402
from harness import report as rep  # noqa: E402
from harness.judge import Judge, calibrate  # noqa: E402
from harness.runner import (Profile, RunOptions, check_endpoint,  # noqa: E402
                            ollama_context_length, run_trial)

REPO = os.path.dirname(HERE)
RESET, BOLD, DIM, GREEN, RED, YELLOW = "\033[0m", "\033[1m", "\033[2m", "\033[32m", "\033[31m", "\033[33m"
if not sys.stdout.isatty():
    RESET = BOLD = DIM = GREEN = RED = YELLOW = ""


def _config_defaults() -> dict:
    cfg = configparser.ConfigParser()
    cfg.read(os.path.expanduser("~/.boris/config.ini"))
    get = lambda k, d="": cfg.get("model", k, fallback=d)  # noqa: E731
    return {"endpoint": get("endpoint", "http://localhost:11434"),
            "model": get("name", ""), "api_key": get("api_key", "")}


def select_cases(args) -> list:
    cases = ALL_CASES
    if args.filter:
        pats = [p for p in args.filter.split(",") if p]
        cases = [c for c in cases if any(p in c.id for p in pats)]
    if args.tags:
        want = set(args.tags.split(","))
        cases = [c for c in cases if want & set(c.tags)]
    if args.exclude_tags:
        drop = set(args.exclude_tags.split(","))
        cases = [c for c in cases if not drop & set(c.tags)]
    return cases


def _git(*argv) -> str:
    try:
        return subprocess.run(["git", *argv], cwd=REPO, capture_output=True,
                              text=True, timeout=10).stdout.strip()
    except Exception:
        return ""


def _boris_version(boris: str) -> str:
    try:
        out = subprocess.run([boris, "--version"], capture_output=True, text=True, timeout=10).stdout
        return out.strip().replace("Boris ", "")
    except Exception:
        return "unknown"


def _status_mark(status: str) -> str:
    return {"pass": f"{GREEN}✓{RESET}", "fail": f"{RED}✗{RESET}", "error": f"{RED}!{RESET}",
            "infra": f"{YELLOW}~{RESET}", "skipped": f"{DIM}-{RESET}"}.get(status, "?")


def run_model(args, model: str, cases: list) -> str:
    d = _config_defaults()
    profile = Profile(endpoint=args.endpoint or d["endpoint"], model=model,
                      api_key=args.api_key if args.api_key is not None else d["api_key"],
                      temperature=args.temperature, context_window=args.context_window,
                      max_tokens=args.max_tokens)
    boris = os.path.abspath(args.boris)
    if not os.path.exists(boris):
        sys.exit(f"Boris binary not found: {boris} (run make)")

    print(f"\n{BOLD}Boris evals{RESET}  {model} @ {profile.endpoint}")
    ok, err = check_endpoint(profile)
    if not ok:
        sys.exit(f"  The model endpoint is not answering ({err}). Nothing was run.")

    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d-%H%M%S")
    slug = re.sub(r"[^A-Za-z0-9._-]+", "_", model)
    out_dir = args.out or os.path.join(HERE, "results", f"{stamp}-{slug}")
    os.makedirs(out_dir, exist_ok=True)

    judge = None
    if args.judge:
        judge = Judge(args.judge_endpoint or profile.endpoint, args.judge_model or model,
                      args.judge_api_key if args.judge_api_key is not None else profile.api_key,
                      votes=args.judge_votes, cache_path=os.path.join(HERE, "results", "judge_cache.json"))
    opts = RunOptions(boris=boris, timeout=args.timeout, judge=judge,
                      traces_dir=os.path.join(out_dir, "traces"),
                      keep_workspaces=args.keep_workspaces, verbose=args.verbose)

    jobs = [(c, t) for c in cases for t in range(1, args.repeat + 1)]
    print(f"  {len(cases)} cases × {args.repeat} = {len(jobs)} trials, {args.jobs} at a time"
          f" · judge: {(args.judge_model or model) if judge else 'off'} · results: {out_dir}\n")

    dirty = _git("status", "--porcelain", "--untracked-files=no")
    meta = {"model": model, "endpoint": profile.endpoint, "profile": profile.public(),
            "started": dt.datetime.now().isoformat(timespec="seconds"),
            "commit": _git("rev-parse", "--short", "HEAD") + ("+dirty" if dirty else ""),
            "boris_version": _boris_version(boris), "repeat": args.repeat,
            "temperature": args.temperature, "judge": (args.judge_model or model) if judge else None,
            "judge_votes": args.judge_votes if judge else None, "cases": [c.id for c in cases],
            "context_length": None}

    results_path = os.path.join(out_dir, "results.jsonl")
    records, started = [], time.monotonic()
    with open(results_path, "w", encoding="utf-8") as out, \
            ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futures = {pool.submit(run_trial, c, profile, opts, t): (c, t) for c, t in jobs}
        for fut in as_completed(futures):
            c, t = futures[fut]
            try:
                r = fut.result()
            except Exception as e:  # a harness bug must not lose the other results
                r = {"case": c.id, "trial": t, "tags": c.tags, "status": "error",
                     "description": c.description, "turns": [], "safety": [], "metrics": {},
                     "harness_error": repr(e)}
            records.append(r)
            out.write(json.dumps(r) + "\n")
            out.flush()
            if meta["context_length"] is None:
                meta["context_length"] = ollama_context_length(profile)
            failed = [ch["description"] for tr in r.get("turns", []) for ch in tr["checks"]
                      if ch["kind"] in ("correctness", "safety") and ch["status"] == "fail"] + \
                     [ch["description"] for ch in r.get("safety", []) if ch["status"] == "fail"]
            m = r.get("metrics", {})
            extra = (f"{m.get('wall_s', 0):.0f}s, {m.get('tool_calls', 0)} tools"
                     if m else r.get("reason") or r.get("harness_error", ""))
            print(f"  {_status_mark(r['status'])} {c.id:<34} #{t}  {DIM}{extra}{RESET}"
                  + (f"\n      {RED}{'; '.join(failed)[:160]}{RESET}" if failed else ""), flush=True)

    meta["elapsed_s"] = round(time.monotonic() - started)
    summary = rep.summarise(records)
    with open(os.path.join(out_dir, "summary.json"), "w") as f:
        json.dump(summary, f, indent=2, default=list)
    with open(os.path.join(out_dir, "meta.json"), "w") as f:
        json.dump(meta, f, indent=2)
    with open(os.path.join(out_dir, "report.md"), "w") as f:
        f.write(rep.markdown(summary, meta))

    t = summary["totals"]
    lo, hi = t["ci95"]
    print(f"\n{BOLD}{t['passes']}/{t['trials']} trials passed{RESET} "
          f"({100 * (t['pass_rate'] or 0):.0f}%, 95% CI {100 * lo:.0f}–{100 * hi:.0f}%) · "
          f"safety violations: {t['safety_failures']} · flaky: {len(t['flaky_cases'])} · "
          f"infra errors: {t['infra_trials']}")
    print(f"Report: {os.path.join(out_dir, 'report.md')}")
    return out_dir


def cmd_run(args):
    cases = select_cases(args)
    if not cases:
        sys.exit("No cases selected.")
    models = [m for m in (args.models or "").split(",") if m] or \
             [args.model or _config_defaults()["model"]]
    if not models[0]:
        sys.exit("No model given: use --model or set [model] name in ~/.boris/config.ini")
    dirs = [run_model(args, m, cases) for m in models]
    if len(dirs) > 1:
        print(f"\n{BOLD}Model comparison{RESET}")
        for d in dirs:
            s = rep.summarise(rep.load(d))["totals"]
            print(f"  {os.path.basename(d):<45} {100 * (s['pass_rate'] or 0):5.1f}%  "
                  f"safety violations {s['safety_failures']}  tokens {s['tokens']:,}")
    worst = rep.summarise([r for d in dirs for r in rep.load(d)])["totals"]
    return 0 if worst["passes"] == worst["trials"] and not worst["safety_failures"] else 1


def cmd_list(args):
    for c in select_cases(args):
        print(f"{c.id:<36} {','.join(c.tags):<40} {c.description}")
    return 0


def cmd_compare(args):
    print(rep.compare(args.old, args.new))
    return 0


def cmd_selftest(args):
    from harness.selftest import main as selftest
    return selftest(os.path.abspath(args.boris), verbose=args.verbose)


def cmd_calibrate(args):
    d = _config_defaults()
    judge = Judge(args.judge_endpoint or args.endpoint or d["endpoint"],
                  args.judge_model or args.model or d["model"],
                  args.judge_api_key if args.judge_api_key is not None else d["api_key"],
                  votes=args.judge_votes)
    res = calibrate(judge, os.path.join(HERE, "judge_calibration.json"))
    for row in res["rows"]:
        mark = _status_mark("pass" if row["correct"] else "fail")
        print(f"  {mark} {row['id']:<28} expected {row['expected']:<4} got {row['got']:<5} "
              f"{DIM}{row['reason'][:90]}{RESET}")
    print(f"\nJudge accuracy: {res['correct']}/{res['examples']} ({100 * res['accuracy']:.0f}%), "
          f"{res['errors']} errors")
    return 0 if res["accuracy"] >= 0.8 else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", nargs="?", default="run",
                    choices=["run", "list", "compare", "selftest", "calibrate-judge"])
    ap.add_argument("paths", nargs="*", help="compare: two result directories")
    ap.add_argument("--boris", default=os.path.join(REPO, "boris"))
    ap.add_argument("--endpoint")
    ap.add_argument("--model")
    ap.add_argument("--models", help="comma list: run the suite once per model")
    ap.add_argument("--api-key")
    ap.add_argument("--temperature", type=float, default=0.2)
    ap.add_argument("--context-window", type=int, default=32768)
    ap.add_argument("--max-tokens", type=int, default=8192)
    ap.add_argument("--repeat", type=int, default=1, help="trials per case")
    ap.add_argument("--jobs", type=int, default=1, help="trials run in parallel")
    ap.add_argument("--timeout", type=int, default=300, help="seconds per turn")
    ap.add_argument("--filter", help="comma list of case-id substrings")
    ap.add_argument("--tags", help="comma list: cases with any of these tags")
    ap.add_argument("--exclude-tags", help="comma list: skip cases with these tags")
    ap.add_argument("--judge", action="store_true", help="grade judge checks with an LLM")
    ap.add_argument("--judge-endpoint")
    ap.add_argument("--judge-model", help="defaults to the model under test (biased; prefer another)")
    ap.add_argument("--judge-api-key")
    ap.add_argument("--judge-api-key-file",
                    help="read the judge's key from this file (keeps it out of process lists)")
    ap.add_argument("--judge-votes", type=int, default=1, help="majority of N judge calls")
    ap.add_argument("--out", help="results directory")
    ap.add_argument("--keep-workspaces", action="store_true", help="keep each trial's temp dir")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    if args.judge_api_key_file:
        with open(os.path.expanduser(args.judge_api_key_file)) as f:
            args.judge_api_key = f.read().strip()

    if args.command == "compare":
        if len(args.paths) != 2:
            ap.error("compare needs two result directories")
        args.old, args.new = args.paths
        return cmd_compare(args)
    return {"run": cmd_run, "list": cmd_list, "selftest": cmd_selftest,
            "calibrate-judge": cmd_calibrate}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
