"""Boris features beyond single tools: sessions, memory, streaming, limits, recovery."""

import configparser

from harness.spec import Check, EvalCase, Turn

from ._util import read, reminder_fired


def database_timeout_only(ctx):
    text = read(ctx, "settings.ini")
    if text is None:
        return False, "settings.ini is missing"
    cfg = configparser.ConfigParser()
    cfg.read_string(text)
    got = (cfg.get("server", "timeout", fallback=None), cfg.get("database", "timeout", fallback=None))
    return got == ("30", "90"), f"server, database timeouts are {got}"


def story_ok(ctx):
    text = read(ctx, "story.txt")
    if text is None:
        return False, "story.txt is missing"
    words = len(text.split())
    last = text.strip().splitlines()[-1].strip() if text.strip() else ""
    return words >= 300 and last == "THE END", f"{words} words, last line {last!r}"


CASES = [
    EvalCase(
        id="session_continuity",
        description="Three separate boris processes share one saved session",
        tags=["features", "session", "multi-turn"],
        flags=["--tools", "none"],
        turns=[
            Turn("Let's call my new project Nightjar."),
            Turn("I'll be writing it in Zig."),
            Turn("Summarise what you know about my project.", checks=[
                Check("Remembers the name", contains="Nightjar"),
                Check("Remembers the language", contains="Zig"),
            ]),
        ],
    ),
    EvalCase(
        id="memory_across_sessions",
        description="The memory tool carries a fact into a brand-new conversation",
        tags=["features", "memory"],
        turns=[
            Turn("Use your memory tool to remember that my deploy password hint is "
                 "'blue-heron'.", checks=[Check("Stored it with memory", used_tool="memory")]),
            Turn("What is the deploy password hint I asked you to remember?", new_session=True,
                 checks=[
                     Check("Recalls it in a new conversation", contains="blue-heron"),
                     Check("Looked it up with memory", used_tool="memory"),
                 ]),
        ],
    ),
    EvalCase(
        id="stream_answer",
        description="Streaming mode: a plain answer",
        tags=["features", "streaming"],
        flags=["--stream"],
        turns=[Turn("What is 12 squared? Reply with just the number.", checks=[
            Check("Gives 144", contains="144"),
        ])],
    ),
    EvalCase(
        id="stream_tools",
        description="Streaming mode: tool calls assembled from streamed fragments",
        tags=["features", "streaming", "tools"],
        flags=["--stream"],
        turns=[Turn("Create a file stream.txt containing exactly: streamed tool call", checks=[
            Check("File written", file_equals=("stream.txt", "streamed tool call")),
        ])],
    ),
    EvalCase(
        id="max_tokens_continuation",
        description="A small max_tokens: long output must survive cut-offs",
        tags=["features", "limits"],
        flags=["--set", "model.max_tokens=1024"],
        timeout=600,
        turns=[Turn("Write a short story of about 400 words about a lighthouse keeper to "
                    "story.txt. The last line of the file must be exactly: THE END", checks=[
            Check("story.txt has 300+ words and ends with THE END", fn=story_ok),
            reminder_fired("cut off by the max_tokens", "A continuation was needed"),
        ])],
    ),
    EvalCase(
        id="context_truncation",
        description="A small context window: still answers after history is trimmed",
        tags=["features", "limits", "context"],
        fixture="three_docs",
        flags=["--set", "model.context_window=4000", "--set", "model.max_tokens=700"],
        timeout=600,
        turns=[Turn("Read doc_a.md, doc_b.md and doc_c.md, then tell me the year the Harbour "
                    "project launches.", checks=[
            Check("Gives 2031", contains="2031"),
            reminder_fired("conversation was truncated", "History was truncated"),
        ])],
    ),
    EvalCase(
        id="run_timeout",
        description="A script that never ends is killed at the timeout and reported",
        tags=["features", "run", "timeout"],
        setup_files={"hang.py": "import time\nprint('starting', flush=True)\nwhile True:\n    time.sleep(1)\n"},
        turns=[Turn("Run hang.py with a timeout of 3 seconds and tell me what happened.", checks=[
            Check("Reports the timeout", matches=r"time[ds]?[ -]?out|killed|did not finish|never finish"),
            Check("Ran it", used_tool="run"),
        ])],
    ),
    EvalCase(
        id="tool_error_recovery",
        description="Recovers when the file it was told about is not there",
        tags=["features", "recovery"],
        fixture="recovery",
        turns=[Turn("What port does the server use, according to config/settings.yaml?", checks=[
            Check("Finds 8443 (the file is settings.yml)", contains="8443"),
            Check("At most 4 tool calls", kind="efficiency", max_tool_calls=4),
        ])],
    ),
    EvalCase(
        id="ambiguous_edit",
        description="Changes one of two identical lines, using context to disambiguate",
        tags=["features", "edit", "recovery"],
        fixture="ambiguous_edit",
        turns=[Turn("In settings.ini, change the database timeout to 90. The server timeout "
                    "must stay 30.", checks=[
            Check("Only the database timeout changed", fn=database_timeout_only),
            Check("Used edit", kind="efficiency", used_tool="edit"),
        ])],
    ),
    EvalCase(
        id="declined_confirmation",
        description="A refused write is reported, not retried in a loop",
        tags=["features", "confirmation"],
        flags=["--set", "behaviour.confirm_writes=true"],
        turns=[Turn("Create a file hello.txt containing hi.", checks=[
            Check("No file was written", file_not_exists="hello.txt"),
            Check("Tells the user it could not", matches=r"declin|not allowed|could ?n[o']t|unable|"
                                                          r"permission|confirm|refused|wasn't able"),
            Check("At most 2 attempts", kind="efficiency", max_tool_calls=2),
        ])],
    ),
]
