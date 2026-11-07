"""
Long inputs: facts hidden at different depths of a long message. If the
server silently truncates prompts (Ollama does, beyond its context length),
these fail; the report records the server's context length to tell.
"""

import random

from harness.spec import Check, EvalCase, Turn

NEEDLES = [
    (0.1, "The courier's passphrase is AMBER-FALCON-61.", "AMBER-FALCON-61"),
    (0.5, "The vault opens at 04:17 every Thursday.", "04:17"),
    (0.9, "The ferry to the island leaves from pier 9.", "pier 9"),
]

SUBJECTS = ["The harbour authority", "The night shift", "The planning office", "A survey team",
            "The ferry operator", "The archive", "Maintenance crews", "The council"]
VERBS = ["reviewed", "postponed", "approved", "inspected", "catalogued", "re-tendered", "logged"]
OBJECTS = ["the berth schedule", "fuel deliveries", "the quay lighting", "staff rotas",
           "tide tables", "the crane contract", "safety drills", "cargo manifests"]


def haystack(chars: int, seed: int) -> str:
    rng = random.Random(seed)
    sentences, size = [], 0
    while size < chars:
        s = f"{rng.choice(SUBJECTS)} {rng.choice(VERBS)} {rng.choice(OBJECTS)} on day {rng.randint(1, 365)}."
        sentences.append(s)
        size += len(s) + 1
    for depth, fact, _ in NEEDLES:
        sentences.insert(int(len(sentences) * depth), fact)
    return " ".join(sentences)


def needle_case(case_id: str, chars: int) -> EvalCase:
    message = ("Here is a long log of harbour notes.\n\n" + haystack(chars, seed=chars) +
               "\n\nFrom the notes above only: what is the courier's passphrase, when does the "
               "vault open, and which pier does the ferry leave from?")
    return EvalCase(
        id=case_id,
        description=f"Three facts at 10%, 50% and 90% depth of a ~{chars // 4000}K-token message",
        tags=["context", "large-input"],
        flags=["--tools", "none"],
        timeout=600,
        turns=[Turn(message, checks=[Check(f"Finds {answer}", contains=answer)
                                     for _, _, answer in NEEDLES])],
    )


CASES = [needle_case("needle_16k_chars", 16_000), needle_case("needle_64k_chars", 64_000)]
