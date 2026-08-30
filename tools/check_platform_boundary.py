#!/usr/bin/env python3
"""Reject platform SDK headers and preprocessor branches in Mira Core."""

from __future__ import annotations

import re
import sys
from pathlib import Path


FORBIDDEN = (
    re.compile(r"[<\"]jni\.h[>\"]"),
    re.compile(r"[<\"]android/"),
    re.compile(r"[<\"]windows\.h[>\"]"),
    re.compile(r"\b__ANDROID__\b"),
    re.compile(r"\b_WIN32\b"),
    re.compile(r"\bWIN32\b"),
    re.compile(r"[<\"](?:unistd|pthread)\.h[>\"]"),
    re.compile(r"[<\"]sys/"),
)


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) == 2 else Path(__file__).resolve().parents[1]
    scan_roots = (root / "include" / "mira", root / "src")
    violations: list[str] = []
    for scan_root in scan_roots:
        if not scan_root.exists():
            continue
        for path in sorted(scan_root.rglob("*")):
            if path.suffix not in {".h", ".hpp", ".c", ".cc", ".cpp", ".cxx"}:
                continue
            text = path.read_text(encoding="utf-8")
            for line_number, line in enumerate(text.splitlines(), start=1):
                if any(pattern.search(line) for pattern in FORBIDDEN):
                    violations.append(f"{path.relative_to(root)}:{line_number}: {line.strip()}")

    if violations:
        print("Mira Core platform boundary violation:")
        print("\n".join(violations))
        return 1
    print("Mira Core platform boundary is clean")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
