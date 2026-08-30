#!/usr/bin/env python3
"""Verify that the checked-in dependency lock and CycloneDX SBOM are current."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path


def main() -> int:
    root = Path(sys.argv[1]).resolve()
    expected_commit = subprocess.check_output(
        ["git", "-C", str(root / "third_party/executor"), "rev-parse", "HEAD"], text=True
    ).strip()
    lock = json.loads((root / "dependencies.lock.json").read_text(encoding="utf-8"))
    sbom = json.loads((root / "docs/supply-chain/sbom.cdx.json").read_text(encoding="utf-8"))
    executor = lock["dependencies"][0]
    errors: list[str] = []
    if executor["name"] != "executor" or executor["commit"] != expected_commit:
        errors.append("dependencies.lock.json does not match the executor submodule commit")
    if not (root / executor["license_file"]).is_file():
        errors.append("executor license file is missing")
    components = {component["name"]: component for component in sbom.get("components", [])}
    if components.get("executor", {}).get("version") != expected_commit:
        errors.append("CycloneDX SBOM does not match the executor submodule commit")
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"Dependency lock and SBOM: OK ({expected_commit})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
