#!/usr/bin/env python3
"""Verify that the checked-in dependency lock and CycloneDX SBOM are current."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path


def main() -> int:
    root = Path(sys.argv[1]).resolve()
    lock = json.loads((root / "dependencies.lock.json").read_text(encoding="utf-8"))
    sbom = json.loads((root / "docs/supply-chain/sbom.cdx.json").read_text(encoding="utf-8"))
    errors: list[str] = []
    components = {component["name"]: component for component in sbom.get("components", [])}
    verified: list[str] = []
    for dependency in lock.get("dependencies", []):
        name = dependency["name"]
        path = root / dependency["path"]
        expected_commit = subprocess.check_output(
            ["git", "-C", str(path), "rev-parse", "HEAD"], text=True
        ).strip()
        if dependency["commit"] != expected_commit:
            errors.append(f"dependencies.lock.json does not match {name} commit")
        if not (root / dependency["license_file"]).is_file():
            errors.append(f"{name} license file is missing")
        if components.get(name, {}).get("version") != expected_commit:
            errors.append(f"CycloneDX SBOM does not match {name} commit")
        for transitive in dependency.get("transitive_submodules", []):
            transitive_path = path / transitive["name"].removeprefix("mbedtls-")
            if not transitive_path.is_dir():
                errors.append(f"{name} transitive submodule {transitive['name']} is missing")
                continue
            actual = subprocess.check_output(
                ["git", "-C", str(transitive_path), "rev-parse", "HEAD"], text=True
            ).strip()
            if transitive["commit"] != actual:
                errors.append(f"{name} transitive submodule {transitive['name']} is not locked")
        verified.append(f"{name}@{expected_commit[:12]}")
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"Dependency lock and SBOM: OK ({', '.join(verified)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
