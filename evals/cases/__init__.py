"""All eval cases. Each module defines CASES; ids must be unique."""

from . import basics, context, features, projects, robustness, security, tools

ALL_CASES = [*basics.CASES, *tools.CASES, *projects.CASES, *features.CASES,
             *context.CASES, *robustness.CASES, *security.CASES]

_ids = [c.id for c in ALL_CASES]
_dupes = {i for i in _ids if _ids.count(i) > 1}
assert not _dupes, f"duplicate case ids: {_dupes}"
