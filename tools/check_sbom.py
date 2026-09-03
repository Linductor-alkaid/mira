#!/usr/bin/env python3
"""Verify that the checked-in dependency lock and CycloneDX SBOM are current."""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from pathlib import Path


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def check_git_dependency(
    root: Path, dependency: dict, components: dict[str, dict], errors: list[str]
) -> str:
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
    return f"{name}@{expected_commit[:12]}"


def check_vendored_dependency(
    root: Path, dependency: dict, components: dict[str, dict], errors: list[str]
) -> str:
    name = dependency["name"]
    version = dependency["version"]
    if not (root / dependency["license_file"]).is_file():
        errors.append(f"{name} license file is missing")
    for entry in dependency.get("files", []):
        file_path = root / entry["path"]
        if not file_path.is_file():
            errors.append(f"{name} vendored file {entry['path']} is missing")
            continue
        if sha256_file(file_path) != entry["sha256"]:
            errors.append(f"{name} vendored file {entry['path']} digest mismatch")
    unexpected = {
        str(path.relative_to(root))
        for path in (root / dependency["path"]).rglob("*")
        if path.is_file()
        and path.name
        not in {"sqlite3.c", "sqlite3.h", "sqlite3ext.h", "LICENSE.md", "README.md", "CMakeLists.txt"}
    }
    if unexpected:
        errors.append(f"{name} has untracked vendored files: {', '.join(sorted(unexpected))}")
    component = components.get(name, {})
    if component.get("version") != version:
        errors.append(f"CycloneDX SBOM does not match {name} version {version}")
    sbom_hashes = {
        entry.get("content", "").lower()
        for entry in component.get("hashes", [])
        if entry.get("alg") == "SHA-256"
    }
    if dependency["archive_sha256"].lower() not in sbom_hashes:
        errors.append(f"CycloneDX SBOM does not carry the {name} archive digest")
    return f"{name}@{version}"


def main() -> int:
    root = Path(sys.argv[1]).resolve()
    lock = json.loads((root / "dependencies.lock.json").read_text(encoding="utf-8"))
    sbom = json.loads((root / "docs/supply-chain/sbom.cdx.json").read_text(encoding="utf-8"))
    errors: list[str] = []
    components = {component["name"]: component for component in sbom.get("components", [])}
    verified: list[str] = []
    for dependency in lock.get("dependencies", []):
        if dependency.get("distribution", "git") == "vendored":
            verified.append(check_vendored_dependency(root, dependency, components, errors))
        else:
            verified.append(check_git_dependency(root, dependency, components, errors))
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"Dependency lock and SBOM: OK ({', '.join(verified)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
