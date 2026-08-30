#!/usr/bin/env python3
"""Check Markdown links and fenced-code balance without external packages."""

from __future__ import annotations

import re
import sys
from pathlib import Path


def main() -> int:
    root = Path(sys.argv[1]).resolve()
    failures: list[str] = []
    link_pattern = re.compile(r"(?<!!)\[[^\]]+\]\(([^)]+)\)")
    for document in sorted((root / "docs").rglob("*.md")):
        text = document.read_text(encoding="utf-8")
        fences = sum(1 for line in text.splitlines() if line.lstrip().startswith("```"))
        if fences % 2:
            failures.append(f"{document.relative_to(root)}: unbalanced code fences")
        for raw_target in link_pattern.findall(text):
            target = raw_target.strip().split("#", 1)[0]
            if not target or "://" in target or target.startswith("mailto:"):
                continue
            target = target.split(" ", 1)[0].strip("<>")
            resolved = (document.parent / target).resolve()
            try:
                resolved.relative_to(root)
            except ValueError:
                failures.append(
                    f"{document.relative_to(root)}: link escapes repository: {raw_target}"
                )
                continue
            if not resolved.exists():
                failures.append(
                    f"{document.relative_to(root)}: missing link target: {raw_target}"
                )
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print("Markdown links and fences: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
