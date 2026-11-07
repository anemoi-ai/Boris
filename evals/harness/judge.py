"""
LLM-as-judge.

The judge sees the user's task, the tool calls the agent made and its
reply, reasons briefly, then gives a verdict. With votes > 1 it is asked
several times and the majority wins (ties fail). Verdicts are cached by
the hash of everything the judge saw, so repeated runs cost nothing.

A judge that cannot answer returns status "error", which the report
counts separately from a failed check: an unreachable judge is not a
model failure.

Use a different model from the one under test where possible; a model
grading its own output is biased towards passing it. `calibrate()` runs
the judge over evals/judge_calibration.json (answers with known verdicts)
to measure how far to trust it.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import threading
import urllib.request

SYSTEM = (
    "You grade the work of an AI agent called Boris against one criterion. "
    "Judge only the criterion, strictly and literally. Think step by step in "
    "at most three sentences, then end with exactly one line of JSON: "
    '{"pass": true or false, "reason": "one sentence"}'
)


class Judge:
    def __init__(self, endpoint: str, model: str, api_key: str = "",
                 votes: int = 1, cache_path: str | None = None, timeout: int = 300):
        base = endpoint.rstrip("/")
        if not base.endswith("/v1/chat/completions"):
            base += "/v1/chat/completions"
        self.url = base
        self.model = model
        self.api_key = api_key
        self.votes = max(1, votes)
        self.timeout = timeout
        self.cache_path = cache_path
        self.lock = threading.Lock()
        self.cache = {}
        if cache_path and os.path.exists(cache_path):
            try:
                with open(cache_path) as f:
                    self.cache = json.load(f)
            except (OSError, json.JSONDecodeError):
                self.cache = {}

    def _ask(self, prompt: str) -> tuple:
        body = json.dumps({
            "model": self.model,
            "messages": [{"role": "system", "content": SYSTEM},
                         {"role": "user", "content": prompt}],
            "max_tokens": 4096,   # reasoning models think before answering
            "temperature": 0 if self.votes == 1 else 0.7,
        }).encode()
        req = urllib.request.Request(self.url, data=body, method="POST", headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {self.api_key or 'none'}",
        })
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                data = json.loads(resp.read())
            text = (data["choices"][0]["message"].get("content") or "")
        except Exception as e:  # network, HTTP or shape errors
            return None, f"judge error: {e}"
        text = re.sub(r"<think>.*?</think>", "", text, flags=re.DOTALL)
        objs = re.findall(r"\{[^{}]*\"pass\"[^{}]*\}", text)
        if not objs:
            return None, f"judge gave no verdict: {text.strip()[:120]!r}"
        try:
            verdict = json.loads(objs[-1])
        except json.JSONDecodeError:
            return None, f"unparseable verdict: {objs[-1][:120]!r}"
        return bool(verdict.get("pass")), str(verdict.get("reason", ""))

    def grade(self, criterion: str, task: str, response: str, tool_calls: list) -> tuple:
        """Returns (status, reason) with status 'pass', 'fail' or 'error'."""
        calls = "\n".join(f"- {c.get('name')} {c.get('summary', '')}"
                          f"{' (error)' if c.get('is_error') else ''}" for c in tool_calls) or "(none)"
        prompt = (f"Criterion: {criterion}\n\n"
                  f"The user's request:\n{task[:4000]}\n\n"
                  f"Tool calls Boris made:\n{calls[:3000]}\n\n"
                  f"Boris's final reply:\n{response[:8000]}")
        key = hashlib.sha256(f"{self.model}|{self.votes}|{prompt}".encode()).hexdigest()
        with self.lock:
            if key in self.cache:
                return tuple(self.cache[key])

        results = []
        for _ in range(self.votes):
            r = self._ask(prompt)
            if r[0] is None:  # empty or unparseable (reasoning ran out): ask once more
                r = self._ask(prompt)
            results.append(r)
        answered = [r for r in results if r[0] is not None]
        if not answered:
            outcome = ("error", results[0][1])
        else:
            yes = sum(1 for r in answered if r[0])
            passed = yes * 2 > len(answered)
            reason = next(r[1] for r in answered if r[0] == passed) if any(
                r[0] == passed for r in answered) else answered[0][1]
            if len(answered) > 1:
                reason = f"{yes}/{len(answered)} votes pass. {reason}"
            outcome = ("pass" if passed else "fail", reason)

        if outcome[0] != "error":
            with self.lock:
                self.cache[key] = list(outcome)
                self._save()
        return outcome

    def _save(self):
        if not self.cache_path:
            return
        tmp = self.cache_path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(self.cache, f)
        os.replace(tmp, self.cache_path)


def calibrate(judge: Judge, path: str) -> dict:
    """Run the judge on examples with known verdicts; return accuracy figures."""
    with open(path) as f:
        examples = json.load(f)
    rows, correct, errors = [], 0, 0
    for ex in examples:
        status, reason = judge.grade(ex["criterion"], ex["task"], ex["response"],
                                     ex.get("tool_calls", []))
        expected = "pass" if ex["expected"] else "fail"
        ok = status == expected
        correct += ok
        errors += status == "error"
        rows.append({"id": ex["id"], "expected": expected, "got": status,
                     "correct": ok, "reason": reason})
    n = len(examples)
    return {"examples": n, "correct": correct, "errors": errors,
            "accuracy": correct / n if n else 0.0, "rows": rows}
