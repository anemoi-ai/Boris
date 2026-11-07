"""Plain answers and conversation memory."""

from harness.spec import Check, EvalCase, Turn

CASES = [
    EvalCase(
        id="basic_math",
        description="Answers simple arithmetic without tools",
        tags=["basic", "reasoning"],
        turns=[Turn("What is 17 multiplied by 6?", checks=[
            Check("Gives 102", contains="102"),
            Check("Needs no tools", kind="efficiency", max_tool_calls=0),
        ])],
    ),
    EvalCase(
        id="basic_identity",
        description="Knows its name from the default system prompt",
        tags=["basic"],
        turns=[Turn("What is your name?", checks=[
            Check("Says it is Boris", contains="Boris"),
        ])],
    ),
    EvalCase(
        id="no_thinking_traces",
        description="Reasoning is stripped: the user never sees <think> blocks",
        tags=["basic", "quality"],
        turns=[Turn("Explain what a pointer is in C in one sentence.", checks=[
            Check("No <think> tags", not_matches=r"</?think>"),
            Check("Mentions memory addresses", matches=r"address"),
            Check("Clear one-sentence explanation", kind="info",
                  judge="Is the reply a correct, clear explanation of a C pointer in "
                        "one or two sentences?"),
        ])],
    ),
    EvalCase(
        id="multi_turn_context",
        description="Remembers a fact from the previous turn (session restored)",
        tags=["context", "multi-turn"],
        turns=[
            Turn("My favourite number is 42. Please remember that."),
            Turn("What is my favourite number?", checks=[Check("Recalls 42", contains="42")]),
        ],
    ),
    EvalCase(
        id="deep_multi_turn_recall",
        description="Recalls five facts given one per turn, without the memory tool",
        tags=["context", "multi-turn"],
        flags=["--tools", "none"],
        turns=[
            Turn("I'll give you five pieces of information, one at a time. "
                 "Piece 1: the secret code is ZEPHYR-7."),
            Turn("Piece 2: my favourite programming language is Rust."),
            Turn("Piece 3: the answer to everything is 42."),
            Turn("Piece 4: the project codename is Prometheus."),
            Turn("Piece 5: my lucky prime is 97."),
            Turn("Now list all five pieces of information, in order.", checks=[
                Check("Recalls ZEPHYR-7", contains="ZEPHYR-7"),
                Check("Recalls Rust", contains="rust"),
                Check("Recalls 42", contains="42"),
                Check("Recalls Prometheus", contains="prometheus"),
                Check("Recalls 97", contains="97"),
            ]),
        ],
    ),
]
