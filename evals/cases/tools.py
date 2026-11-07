"""Single tools and short tool chains, graded on what ended up on disk."""

from harness.spec import Check, EvalCase, Turn

from ._util import lines_equal, py

PRIMES_BELOW_100 = [str(p) for p in range(2, 100) if all(p % d for d in range(2, int(p ** 0.5) + 1))]

CASES = [
    EvalCase(
        id="tool_write_read",
        description="Writes a file with exact content, then reads it back",
        tags=["tools", "write", "read", "multi-turn"],
        turns=[
            Turn("Write a file called eval_test.txt containing exactly the text: "
                 "hello from boris eval", checks=[
                Check("File has exactly that text",
                      file_equals=("eval_test.txt", "hello from boris eval")),
                Check("Used write", kind="info", used_tool="write"),
            ]),
            Turn("Now read eval_test.txt and tell me exactly what it says.", checks=[
                Check("Reports the content", contains="hello from boris eval"),
                Check("Read the file rather than remembering", kind="info", used_tool="read"),
            ]),
        ],
    ),
    EvalCase(
        id="tool_write_python",
        description="Writes and runs a Python script; the harness runs it too",
        tags=["tools", "write", "run"],
        turns=[Turn("Write a Python script called add.py that prints the result of 99 + 1, "
                    "then run it.", checks=[
            Check("Reports 100", contains="100"),
            Check("add.py really prints 100", command="python3 add.py",
                  command_output=r"^\s*100\s*$"),
            Check("Ran it", used_tool="run"),
        ])],
    ),
    EvalCase(
        id="tool_list_dir",
        description="Lists a known directory accurately",
        tags=["tools", "list_dir"],
        fixture="listing",
        turns=[Turn("List the files and folders at the top of your sandbox.", checks=[
            Check("Mentions README.md", contains="README.md"),
            Check("Mentions notes.txt", contains="notes.txt"),
            Check("Mentions the data folder", contains="data"),
            Check("Mentions the src folder", contains="src"),
            Check("Used list_dir", kind="info", used_tool="list_dir"),
        ])],
    ),
    EvalCase(
        id="tool_write_chunked",
        description="Writes 50 numbered lines correctly",
        tags=["tools", "write", "robustness"],
        turns=[Turn("Write a file called numbers.txt containing the numbers 1 through 50, "
                    "one per line. Then tell me how many lines it has.", checks=[
            lines_equal("numbers.txt", [str(i) for i in range(1, 51)], "numbers.txt is exactly 1..50"),
            Check("Reports 50", matches=r"\b50\b"),
        ])],
    ),
    EvalCase(
        id="multi_file_project",
        description="Writes a module and its tests; hidden tests check the module",
        tags=["tools", "write", "run", "multi-file"],
        timeout=400,
        turns=[Turn(
            "Create two Python files:\n"
            "1) stats.py with mean(numbers) and median(numbers).\n"
            "2) test_stats.py that imports stats and asserts mean([1, 2, 3, 4, 5]) == 3.0, "
            "median([3, 1, 2]) == 2 and median([1, 2, 3, 4]) == 2.5, then prints "
            "'ALL TESTS PASSED'.\n"
            "Then run test_stats.py and report the output.", checks=[
                Check("Its own tests pass when the harness runs them",
                      command="python3 test_stats.py", command_output="ALL TESTS PASSED"),
                Check("Hidden tests pass", command=py("""
                    import stats
                    assert stats.mean([1, 2, 3, 4, 5]) == 3.0
                    assert stats.mean([2.5]) == 2.5
                    assert stats.median([3, 1, 2]) == 2
                    assert stats.median([1, 2, 3, 4]) == 2.5
                    assert stats.median([9, 1, 5, 3, 7, 11]) == 6
                    assert stats.median([7]) == 7
                """)),
                Check("Reports the tests passing", contains="ALL TESTS PASSED"),
            ])],
    ),
    EvalCase(
        id="code_debug_and_fix",
        description="Writes a deliberately buggy script, then finds and fixes the bug",
        tags=["tools", "write", "run", "debugging", "multi-turn"],
        timeout=400,
        turns=[
            Turn("Write a Python script called sum_range.py that is SUPPOSED to sum all integers "
                 "from 1 to 10 inclusive and print the result, but deliberately use range(1, 10) "
                 "instead of range(1, 11). Run it.", checks=[
                Check("Buggy output is 45", contains="45"),
            ]),
            Turn("The output is wrong: the sum of 1 to 10 is 55. Find the bug in sum_range.py, "
                 "fix it and run it again.", checks=[
                Check("Script now prints 55", command="python3 sum_range.py",
                      command_output=r"^\s*55\s*$"),
                Check("Reports 55", contains="55"),
                Check("Fixed with edit, not a rewrite", kind="efficiency", not_used_tool="write"),
            ]),
        ],
    ),
    EvalCase(
        id="large_code_generation",
        description="Writes eight maths functions; hidden tests check every one",
        tags=["tools", "write", "run", "large-output"],
        timeout=500,
        turns=[Turn(
            "Write a Python file called math_functions.py with these functions:\n"
            "1. is_prime(n)\n2. primes_up_to(n): all primes <= n\n3. factorial(n)\n"
            "4. fibonacci(n): 0-indexed, fibonacci(0) = 0, fibonacci(1) = 1\n"
            "5. gcd(a, b)\n6. lcm(a, b)\n"
            "7. is_perfect(n): n equals the sum of its proper divisors\n"
            "8. digit_sum(n): sum of the decimal digits of n\n"
            "Put a main block under if __name__ == '__main__' that prints primes_up_to(50), "
            "factorial(10), fibonacci(10), gcd(48, 18) and is_perfect(28). "
            "Run it and report the output.", checks=[
                Check("Hidden tests pass", command=py("""
                    import math_functions as m
                    assert m.is_prime(97) and not m.is_prime(1) and not m.is_prime(91)
                    assert m.primes_up_to(20) == [2, 3, 5, 7, 11, 13, 17, 19]
                    assert m.factorial(0) == 1 and m.factorial(10) == 3628800
                    assert m.fibonacci(0) == 0 and m.fibonacci(1) == 1 and m.fibonacci(20) == 6765
                    assert m.gcd(48, 18) == 6 and m.lcm(4, 6) == 12
                    assert m.is_perfect(28) and m.is_perfect(496) and not m.is_perfect(12)
                    assert m.digit_sum(9875) == 29
                """)),
                Check("Reports factorial(10)", matches=r"3,?628,?800"),
            ])],
    ),
    EvalCase(
        id="iterative_file_building",
        description="Builds a file over three turns: create, append, count",
        tags=["tools", "write", "run", "multi-turn"],
        timeout=400,
        turns=[
            Turn("Create words.txt with these 10 words, one per line: apple, banana, cherry, "
                 "date, elderberry, fig, grape, honeydew, kiwi, lemon"),
            Turn("Append 10 more words to words.txt, one per line: mango, nectarine, orange, "
                 "papaya, quince, raspberry, strawberry, tangerine, ugli, vanilla"),
            Turn("Write a Python script count_words.py that prints the number of non-empty "
                 "lines in words.txt. Run it.", checks=[
                lines_equal("words.txt", "apple banana cherry date elderberry fig grape honeydew "
                            "kiwi lemon mango nectarine orange papaya quince raspberry strawberry "
                            "tangerine ugli vanilla".split(), "words.txt has all 20 words in order"),
                Check("count_words.py prints 20", command="python3 count_words.py",
                      command_output=r"\b20\b"),
                Check("Reports 20", matches=r"\b20\b"),
            ]),
        ],
    ),
    EvalCase(
        id="tool_chain_pipeline",
        description="Script writes results to a file; the answer comes from that file",
        tags=["tools", "write", "run", "read", "pipeline"],
        timeout=400,
        turns=[Turn(
            "Write a Python script primes.py that finds all primes below 100, writes them to "
            "prime_results.txt one per line and prints 'Done: N primes found'. Run it. Then read "
            "prime_results.txt and tell me how many primes there are and the largest one.", checks=[
                lines_equal("prime_results.txt", PRIMES_BELOW_100, "prime_results.txt is correct"),
                Check("Reports 25", matches=r"\b25\b"),
                Check("Reports 97", contains="97"),
            ])],
    ),

    # ── The tools added in 1.2.0 ──────────────────────────────────────────────
    EvalCase(
        id="tool_edit_one_line",
        description="Changes one line with edit instead of rewriting the file",
        tags=["tools", "edit"],
        setup_files={"edit_eval/settings.py": "NAME = 'boris'\nRETRIES = 3\nTIMEOUT = 30\n"},
        turns=[Turn("In edit_eval/settings.py change RETRIES to 5. Leave everything else exactly "
                    "as it is.", checks=[
            Check("File is exactly right",
                  file_equals=("edit_eval/settings.py", "NAME = 'boris'\nRETRIES = 5\nTIMEOUT = 30")),
            Check("Used edit", used_tool="edit"),
            Check("Did not rewrite the file", kind="efficiency", not_used_tool="write"),
            Check("At most 4 tool calls", kind="efficiency", max_tool_calls=4),
        ])],
    ),
    EvalCase(
        id="tool_search_definition",
        description="Finds a definition with search",
        tags=["tools", "search"],
        setup_files={
            "search_eval/app/main.py": "from app.util import compute_checksum\n\nprint(compute_checksum(b'x'))\n",
            "search_eval/app/util.py": "import zlib\n\n\ndef compute_checksum(data):\n    return zlib.crc32(data)\n",
            "search_eval/app/models.py": "class User:\n    pass\n",
            "search_eval/README.md": "A small app.\n",
        },
        turns=[Turn("Which file under search_eval defines the function compute_checksum, and on "
                    "which line?", checks=[
            Check("Names util.py", contains="util.py"),
            Check("Gives line 4", matches=r"\b(line|:)\s*4\b|\b4\b"),
            Check("Used search", kind="efficiency", used_tool="search"),
            Check("At most 3 tool calls", kind="efficiency", max_tool_calls=3),
        ])],
    ),
    EvalCase(
        id="tool_move_rename",
        description="Renames a file with move",
        tags=["tools", "move"],
        setup_files={"move_eval/draft.txt": "the report\n"},
        turns=[Turn("Rename move_eval/draft.txt to move_eval/final.txt.", checks=[
            Check("New name has the content", file_equals=("move_eval/final.txt", "the report")),
            Check("Old name gone", file_not_exists="move_eval/draft.txt"),
            Check("Used move", used_tool="move"),
            Check("At most 2 tool calls", kind="efficiency", max_tool_calls=2),
        ])],
    ),
    EvalCase(
        id="tool_delete_directory",
        description="Deletes one directory tree and nothing else",
        tags=["tools", "delete"],
        setup_files={"delete_eval/tmp/a.log": "a\n", "delete_eval/tmp/b.log": "b\n",
                     "delete_eval/keep.txt": "keep\n"},
        turns=[Turn("Delete the directory delete_eval/tmp and everything in it. Do not touch "
                    "anything else.", checks=[
            Check("Directory gone", file_not_exists="delete_eval/tmp"),
            Check("Sibling kept", kind="safety", file_exists="delete_eval/keep.txt"),
            Check("Used delete", used_tool="delete"),
            Check("At most 2 tool calls", kind="efficiency", max_tool_calls=2),
        ])],
    ),
    EvalCase(
        id="tool_plan_multistep",
        description="Uses plan to track a multi-step task",
        tags=["tools", "plan"],
        turns=[Turn("Using your plan tool to track progress, create three files in plan_eval: "
                    "one.txt containing 1, two.txt containing 2 and three.txt containing 3.", checks=[
            Check("one.txt", file_equals=("plan_eval/one.txt", "1")),
            Check("two.txt", file_equals=("plan_eval/two.txt", "2")),
            Check("three.txt", file_equals=("plan_eval/three.txt", "3")),
            Check("Used plan", used_tool="plan"),
        ])],
    ),
    EvalCase(
        id="tool_fetch_page",
        description="Fetches a public web page (needs internet access)",
        tags=["tools", "fetch", "network"],
        flags=["--tools", "default,fetch", "--set", "behaviour.confirm_fetch=false"],
        turns=[Turn("Fetch https://example.com and tell me the page's main heading.", checks=[
            Check("Used fetch", used_tool="fetch"),
            Check("Reports the heading", contains="Example Domain"),
        ])],
    ),
]
