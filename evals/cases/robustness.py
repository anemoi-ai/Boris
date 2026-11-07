"""
Failures between Boris and the model, injected by the fault proxy
(harness/servers.py). Each plan entry applies to one model request; after
the plan runs out, requests pass through untouched.
"""

from harness.spec import Check, EvalCase, Turn

from ._util import RETRY_FLAGS, proxy_requests_at_least

QUESTION = "What is the capital of France? Answer in one word."


def _answer(n_requests):
    return [Check("Answers Paris", contains="Paris"), proxy_requests_at_least(n_requests)]


def _error_mentions(text):
    def fn(ctx):
        err = ctx.report.get("error") or ""
        return text in err, f"error was {err!r}"
    return Check(f"Error names the cause ({text})", fn=fn)


CASES = [
    EvalCase(
        id="fault_rate_limited",
        description="Two 429s, then success: retries and answers",
        tags=["robustness", "retry"],
        flags=RETRY_FLAGS, faults=["429", "429"],
        turns=[Turn(QUESTION, checks=_answer(3))],
    ),
    EvalCase(
        id="fault_server_errors",
        description="503 then 500, then success",
        tags=["robustness", "retry"],
        flags=RETRY_FLAGS, faults=["503", "500"],
        turns=[Turn(QUESTION, checks=_answer(3))],
    ),
    EvalCase(
        id="fault_dropped_connection",
        description="The connection closes without a response, then success",
        tags=["robustness", "retry"],
        flags=RETRY_FLAGS, faults=["drop"],
        turns=[Turn(QUESTION, checks=_answer(2))],
    ),
    EvalCase(
        id="fault_slow_response",
        description="A response slower than the request timeout, then success",
        tags=["robustness", "retry", "timeout"],
        flags=RETRY_FLAGS + ["--set", "model.timeout_seconds=8"], faults=["slow:20"],
        turns=[Turn(QUESTION, checks=_answer(2))],
    ),
    EvalCase(
        id="fault_truncated_response",
        description="A 200 response cut off half way, then success",
        tags=["robustness", "retry"],
        flags=RETRY_FLAGS, faults=["truncate"],
        turns=[Turn(QUESTION, checks=_answer(2))],
    ),
    EvalCase(
        id="fault_garbage_response",
        description="A 200 response that is an HTML error page, then success",
        tags=["robustness", "retry"],
        flags=RETRY_FLAGS, faults=["garbage"],
        turns=[Turn(QUESTION, checks=_answer(2))],
    ),
    EvalCase(
        id="fault_mid_task",
        description="Failures between the steps of a tool-using task",
        tags=["robustness", "retry", "tools"],
        flags=RETRY_FLAGS, faults=["ok", "429", "ok", "503"],
        turns=[Turn("Create hello.txt containing exactly 'hi there', then read it back and tell "
                    "me what it says.", checks=[
            Check("File written", file_equals=("hello.txt", "hi there")),
            Check("Reports the content", contains="hi there"),
        ])],
    ),
    EvalCase(
        id="fault_retries_exhausted",
        description="Every request fails: gives up cleanly with a useful error",
        tags=["robustness", "retry"],
        flags=["--set", "agent.max_retries=2", "--set", "agent.retry_backoff_ms=100"],
        faults=["500"] * 10, expect_error=True,
        turns=[Turn(QUESTION, checks=[
            Check("Reports an error", finish_reason="error"),
            _error_mentions("500"),
            Check("Gave up after 3 attempts", kind="efficiency",
                  fn=lambda ctx: (len(ctx.proxy_log) == 3, f"{len(ctx.proxy_log)} attempts")),
        ])],
    ),
]
