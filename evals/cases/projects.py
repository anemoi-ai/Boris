"""
Realistic tasks on small projects. Success is decided by the harness's own
tests and checks, never by what the agent says happened.
"""

import csv
import io
import json
import random

from harness.spec import Check, EvalCase, Turn

from ._util import py, read, run_in_sandbox

UNITTEST = "python3 -m unittest discover -s tests -t . 2>&1"


# ── generated inputs ──────────────────────────────────────────────────────────

def make_big_tree(ws):
    """150 modules of plausible constants; one sets MAX_UPLOAD_MB."""
    rng = random.Random(7)
    areas = ["api", "auth", "billing", "jobs", "reports", "search", "services", "storage", "web"]
    names = ["TIMEOUT_S", "RETRIES", "PAGE_SIZE", "MAX_ITEMS", "CACHE_TTL", "BATCH", "WORKERS"]
    for i in range(150):
        area = areas[i % len(areas)]
        lines = [f"# {area} settings, part {i}"]
        lines += [f"{n} = {rng.randint(1, 900)}" for n in rng.sample(names, 3)]
        ws.write(f"app/{area}/mod_{i:03d}.py", "\n".join(lines) + "\n")
    ws.write("app/services/storage/limits.py",
             "# Upload limits\nMAX_UPLOAD_MB = 37\nMAX_FILES = 12\n")
    ws.write("app/README.md", "Configuration constants are spread across app/.\n")


def make_big_log(ws):
    """About 400 KB of log with one FATAL line near the end."""
    rng = random.Random(11)
    levels = ["INFO"] * 12 + ["DEBUG"] * 6 + ["WARN"]
    msgs = ["request served", "cache refreshed", "user login", "job queued", "job finished",
            "connection reused", "slow query 412ms", "retrying upstream"]
    lines = []
    for i in range(5200):
        lines.append(f"2026-11-07T{(i // 3600) % 24:02d}:{(i // 60) % 60:02d}:{i % 60:02d}Z "
                     f"{rng.choice(levels):5} worker-{rng.randint(1, 8)} {rng.choice(msgs)} id={i}")
        if i == 4700:
            lines.append("2026-11-07T01:18:20Z FATAL worker-3 disk controller fault code=E4471 "
                         "shutting down")
    ws.write("server.log", "\n".join(lines) + "\n")


# ── graders ───────────────────────────────────────────────────────────────────

CLEAN_EXPECTED = [
    ["id", "name", "email", "signup_date"],
    ["1", "Alice Smith", "alice@example.com", "2024-01-05"],
    ["2", "Bob Jones", "bob@example.com", "2024-02-05"],
    ["3", "Carol White", "carol@example.com", "2024-03-17"],
    ["4", "Dan Brown", "dan@example.com", "2024-04-17"],
]


def clean_csv_matches(ctx):
    text = read(ctx, "clean.csv")
    if text is None:
        return False, "clean.csv is missing"
    rows = [[c.strip() for c in r] for r in csv.reader(io.StringIO(text)) if r]
    return rows == CLEAN_EXPECTED, f"got {rows}"


TODO_TEXT = "[x] 1. Buy milk\n[ ] 2. Write report\n[ ] 3. Call Sam\n"
TODO_TASKS = [{"id": 1, "title": "Buy milk", "done": True},
              {"id": 2, "title": "Write report", "done": False},
              {"id": 3, "title": "Call Sam", "done": False}]


def todo_json_works(ctx):
    code, out, err = run_in_sandbox(ctx, ["python3", "todo.py", "list", "--json"])
    if code != 0:
        return False, f"exit {code}: {err.strip()[-200:]}"
    try:
        data = json.loads(out)
    except json.JSONDecodeError:
        return False, f"not JSON: {out[:200]!r}"
    got = [{k: t.get(k) for k in ("id", "title", "done")} for t in data] if isinstance(data, list) else data
    return got == TODO_TASKS, f"got {data}"


def todo_text_unchanged(ctx):
    code, out, err = run_in_sandbox(ctx, ["python3", "todo.py", "list"])
    return code == 0 and out == TODO_TEXT, f"exit {code}, output {out!r}"


CASES = [
    EvalCase(
        id="project_fix_python_bug",
        description="Fixes a bug so a failing test suite passes, without touching the tests",
        tags=["project", "debugging", "python"],
        fixture="py_inventory",
        timeout=500,
        turns=[Turn("The tests in this project fail. Find and fix the bug in the code (not the "
                    "tests), then run the tests with a script to confirm they pass. The tests run "
                    "with: python3 -m unittest discover -s tests -t .", checks=[
            Check("Test suite passes", command=UNITTEST),
            Check("Tests not modified", unchanged="tests/test_store.py"),
            Check("Hidden test: removing all stock removes the item", command=py("""
                from inventory import Store
                s = Store(); s.add("x", 3); s.remove("x", 3)
                assert s.names() == [] and s.count("x") == 0 and s.total() == 0
                s.add("y", 2); s.remove("y")
                assert s.names() == ["y"] and s.count("y") == 1
            """)),
            Check("Fixed with edit", kind="efficiency", used_tool="edit"),
        ])],
    ),
    EvalCase(
        id="project_fix_c_bug",
        description="Fixes a C word-count bug, building and testing through scripts",
        tags=["project", "debugging", "c"],
        fixture="c_wordcount",
        requires=["gcc"],
        timeout=500,
        turns=[Turn("Fix the word-count bug described in README.md. You cannot call gcc "
                    "directly: write a shell script that compiles wc.c and tests it on a few "
                    "inputs, and run it with the run tool.", checks=[
            Check("Hidden tests: counts match wc", command=(
                "gcc -Wall -o .wc_hidden wc.c && "
                "[ \"$(printf 'one two three' | ./.wc_hidden)\" = '0 3 13' ] && "
                "[ \"$(printf 'a b\\nc\\n' | ./.wc_hidden)\" = '2 3 6' ] && "
                "[ \"$(printf '  lead\\ttrail  ' | ./.wc_hidden)\" = '0 2 14' ] && "
                "[ \"$(printf '' | ./.wc_hidden)\" = '0 0 0' ]")),
            Check("Built and tested it", used_tool="run"),
        ])],
    ),
    EvalCase(
        id="project_csv_cleanup",
        description="Cleans a messy CSV to a precise specification",
        tags=["project", "data"],
        fixture="csv_cleanup",
        timeout=500,
        turns=[Turn("customers.csv is messy. Write a cleaned copy to clean.csv: trim spaces "
                    "around every field and header, lowercase the emails, convert dates written "
                    "DD/MM/YYYY to YYYY-MM-DD, remove duplicate rows and sort by id. Keep the same "
                    "four columns.", checks=[
            Check("clean.csv matches the specification", fn=clean_csv_matches),
            Check("Original left alone", unchanged="customers.csv"),
        ])],
    ),
    EvalCase(
        id="project_rename_symbol",
        description="Renames a function across a package and its tests",
        tags=["project", "refactor", "search", "edit"],
        fixture="refactor_rename",
        timeout=500,
        turns=[Turn("Rename the function calc_total to compute_total everywhere in this project, "
                    "including the tests, then run the tests (python3 -m unittest discover -s "
                    "tests -t .).", checks=[
            Check("No calc_total left", command="! grep -rn --include='*.py' calc_total ."),
            Check("Tests pass", command=UNITTEST),
            Check("compute_total works", command=py("""
                from pricing import compute_total
                assert compute_total([10, 10], 0.1) == 22.0
            """)),
            Check("Found the uses with search", kind="efficiency", used_tool="search"),
            Check("Changed files with edit, not rewrites", kind="efficiency", not_used_tool="write"),
        ])],
    ),
    EvalCase(
        id="project_add_feature",
        description="Adds a --json option to a CLI without changing its existing output",
        tags=["project", "feature", "python"],
        fixture="cli_feature",
        timeout=500,
        turns=[Turn("Add a --json option to the list command of todo.py: it should print the "
                    "tasks as a JSON array of objects with the keys id, title and done. Without "
                    "--json, list must print exactly what it prints now. Test it.", checks=[
            Check("list --json prints the tasks", fn=todo_json_works),
            Check("Plain list output unchanged", fn=todo_text_unchanged),
            Check("tasks.json untouched", unchanged="tasks.json"),
        ])],
    ),
    EvalCase(
        id="project_find_in_big_tree",
        description="Finds one constant among 150 modules",
        tags=["project", "search", "large-input"],
        setup=make_big_tree,
        turns=[Turn("What is the value of MAX_UPLOAD_MB in this project, and which file sets it?",
                    checks=[
            Check("Gives 37", matches=r"\b37\b"),
            Check("Names limits.py", contains="limits.py"),
            Check("Used search", kind="efficiency", used_tool="search"),
            Check("At most 4 tool calls", kind="efficiency", max_tool_calls=4),
        ])],
    ),
    EvalCase(
        id="project_big_log",
        description="Finds the one FATAL line in a 400 KB log (beyond one read)",
        tags=["project", "search", "large-input"],
        setup=make_big_log,
        turns=[Turn("server.log is large. Find the FATAL error in it and tell me its error code.",
                    checks=[
            Check("Gives the error code", contains="E4471"),
            Check("At most 4 tool calls", kind="efficiency", max_tool_calls=4),
        ])],
    ),
    EvalCase(
        id="project_shapes_package",
        description="Builds a small package with tests (a task long enough to plan)",
        tags=["project", "python", "plan"],
        timeout=600,
        turns=[Turn("Create a Python package called shapes with circle.py (area(radius)), "
                    "square.py (area(side)) and triangle.py (area(base, height)), and an "
                    "__init__.py that exposes them as circle_area, square_area and triangle_area. "
                    "Add test_shapes.py with unittest tests for all three, and run them.", checks=[
            Check("Hidden tests pass", command=py("""
                import math, shapes
                assert abs(shapes.circle_area(2) - math.pi * 4) < 1e-9
                assert shapes.square_area(3) == 9
                assert shapes.triangle_area(4, 5) == 10
            """)),
            Check("Its own tests pass", command="python3 -m unittest test_shapes 2>&1"),
            Check("Tracked the work with plan", kind="info", used_tool="plan"),
        ])],
    ),
]
