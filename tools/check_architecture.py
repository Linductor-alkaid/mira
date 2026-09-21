#!/usr/bin/env python3
"""Check Mira module dependency direction, include hygiene and size budgets.

The policy single source of truth is ``tools/architecture-policy.json``; rule
semantics are documented in ``docs/project/architecture_governance.md``. Legacy
violations are suppressed only through ``tools/architecture-baseline.json``,
which is refreshed manually after review via ``--update-baseline``; CI must
never refresh the baseline automatically.

Usage:
    python3 tools/check_architecture.py [--update-baseline] [--policy FILE] [ROOT]
"""

from __future__ import annotations

import datetime
import json
import re
import sys
from pathlib import Path, PurePosixPath

SOURCE_SUFFIXES = {".h", ".hpp", ".hh", ".c", ".cc", ".cpp", ".cxx"}

INCLUDE_RE = re.compile(r"^\s*#\s*include\s*(<[^>]+>|\"[^\"]+\")", re.MULTILINE)

# Headers treated as language runtime and never declared in the policy.
# Covers the C++ standard library and the C standard library headers that
# public C ABI shims (host_abi.h) legitimately include.
STDLIB_HEADERS = frozenset(
    """
algorithm any array atomic barrier bitset cassert ccomplex cctype cerrno cfenv
cfloat charconv chrono cinttypes ciso646 climits clocale cmath codecvt compare
complex concepts condition_variable coroutine csetjmp csignal cstdarg cstdbool
cstddef cstdint cstdio cstdlib cstring ctgmath ctime cuchar cwchar cwctype deque
expected execution exception filesystem flat_map flat_set format forward_list
fstream functional future generator initializer_list iomanip ios iosfwd iostream
istream iterator latch limits list locale map mdspan memory memory_resource mutex
new numbers numeric optional ostream print queue random ranges ratio regex
scoped_allocator semaphore set shared_mutex simd source_location span sstream
stack stdexcept stop_token streambuf string string_view strstream system_error
thread tuple typeindex typeinfo type_traits unordered_map unordered_set utility
valarray variant vector version
assert.h complex.h ctype.h errno.h fenv.h float.h inttypes.h iso646.h limits.h
locale.h math.h setjmp.h signal.h stdalign.h stdarg.h stdatomic.h stdbool.h
stddef.h stdint.h stdio.h stdlib.h string.h stdnoreturn.h tgmath.h time.h
uchar.h wchar.h wctype.h
""".split()
)

RULE_HINTS = {
    "module-dependency": "declare the dependency in the source module's "
    "`requires`, or remove the edge (cross-module code depends on public "
    "headers under include/mira only)",
    "deep-import": "include the public header under include/mira instead of "
    "another module's private source",
    "unregistered-include": "register the external header family in "
    "`external_modules` and add it to the consumer's `requires`, or drop it",
    "cycle": "split the owner or invert the dependency through a port",
    "max-file-lines": "split the file by responsibility; do not add an "
    "exception without a reviewed decision",
    "expired-exception": "resolve the violation and remove the expired "
    "exception from the policy",
}


class PolicyError(Exception):
    """Raised when the policy file itself is malformed."""


def load_policy(path: Path) -> dict:
    try:
        policy = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise PolicyError(f"{path}: invalid JSON: {exc}") from exc
    for field in ("version", "modules", "external_modules", "global"):
        if field not in policy:
            raise PolicyError(f"{path}: missing required field '{field}'")

    module_ids = set()
    for module in policy["modules"]:
        for field in ("id", "roots", "requires"):
            if field not in module:
                raise PolicyError(f"{path}: module missing '{field}': {module}")
        module_ids.add(module["id"])

    external_ids = set(policy["external_modules"])
    for module in policy["modules"]:
        unknown = set(module["requires"]) - module_ids - external_ids
        if unknown:
            raise PolicyError(
                f"{path}: module '{module['id']}' requires unknown targets: "
                f"{sorted(unknown)}"
            )
    return policy


def load_baseline(path: Path) -> set[str]:
    if not path.exists():
        return set()
    data = json.loads(path.read_text(encoding="utf-8"))
    return set(data.get("violations", []))


def module_for_path(policy: dict, relpath: str) -> str | None:
    """Resolve a file to its owning module by longest root-prefix match."""
    best: tuple[int, str] | None = None
    for module in policy["modules"]:
        for root in module["roots"]:
            root_posix = PurePosixPath(root)
            path_posix = PurePosixPath(relpath)
            if path_posix == root_posix or root_posix in path_posix.parents:
                depth = len(root_posix.parts)
                if best is None or depth > best[0]:
                    best = (depth, module["id"])
    return best[1] if best else None


def public_module_for_include(policy: dict, include: str) -> str | None:
    """Resolve an <mira/...> include to the owning module.

    Adapter public headers live under include/mira/adapters/<name>/; the owner
    is whichever module declared that root in the policy (ids do not follow a
    naming template, e.g. net -> net_transport).
    """
    parts = PurePosixPath(include).parts
    if parts and parts[0] == "mira":
        rest = parts[1:]
        if len(rest) >= 2 and rest[0] == "adapters":
            declared = f"include/mira/adapters/{rest[1]}"
            for module in policy["modules"]:
                if declared in module["roots"]:
                    return module["id"]
        return "core"
    return None


def external_module_for_include(policy: dict, include: str) -> str | None:
    for ext_id, spec in policy["external_modules"].items():
        for prefix in spec["headers"]:
            if include == prefix or include.startswith(prefix):
                return ext_id
    return None


def iter_source_files(root: Path, policy: dict):
    """Yield (module_id, path) with each file assigned to its longest-prefix
    module root exactly once."""
    skip_parts = {"third_party", ".git", "build", "Testing"}
    assignments: dict[Path, tuple[int, str]] = {}
    for module in policy["modules"]:
        for module_root in module["roots"]:
            base = root / module_root
            if not base.exists():
                continue
            depth = len(PurePosixPath(module_root).parts)
            for path in base.rglob("*"):
                if path.suffix not in SOURCE_SUFFIXES:
                    continue
                if skip_parts & set(path.parts):
                    continue
                prev = assignments.get(path)
                if prev is None or depth > prev[0]:
                    assignments[path] = (depth, module["id"])
    for path in sorted(assignments):
        yield assignments[path][1], path


def parse_includes(text: str):
    for line_number, line in enumerate(text.splitlines(), start=1):
        match = INCLUDE_RE.match(line)
        if not match:
            continue
        token = match.group(1)
        if token.startswith("<"):
            yield line_number, token[1:-1], False
        else:
            yield line_number, token[1:-1], True


def resolve_external_target(
    policy: dict, include: str
) -> tuple[str | None, str | None]:
    """Classify an angle include -> (internal module, external module).

    Returns (None, None) for std headers. Raises nothing; unknown externals are
    reported by the caller as `unregistered-include`.
    """
    internal = public_module_for_include(policy, include)
    if internal is not None:
        return internal, None
    if "/" not in include and "." not in include:
        if include in STDLIB_HEADERS:
            return None, None
    external = external_module_for_include(policy, include)
    if external is not None:
        return None, external
    if include in STDLIB_HEADERS:
        return None, None
    return None, "<unregistered>"


def find_cycles(
    policy: dict, module_edges: dict[str, set[str]]
) -> list[dict]:
    if not policy.get("global", {}).get("forbid_cycles", True):
        return []
    cycles: list[dict] = []
    seen: set[frozenset[str]] = set()

    def dfs(node: str, path: list[str], visiting: set[str]) -> None:
        for nxt in sorted(module_edges.get(node, ())):
            if nxt in visiting:
                idx = path.index(nxt)
                cycle = path[idx:]
                key = frozenset(cycle)
                if key not in seen and len(cycle) > 1:
                    seen.add(key)
                    cycles.append(
                        {
                            "rule": "cycle",
                            "file": "(module graph)",
                            "line": 0,
                            "detail": "",
                            "message": "dependency cycle: "
                            + " -> ".join([*cycle, nxt]),
                        }
                    )
            elif nxt not in path:
                dfs(nxt, [*path, nxt], visiting | {nxt})

    for start in sorted(module_edges):
        dfs(start, [start], {start})
    return cycles


def check(root: Path, policy: dict) -> list[dict]:
    violations: list[dict] = []
    module_edges: dict[str, set[str]] = {}
    known_ids = {m["id"] for m in policy["modules"]}

    for owner, path in iter_source_files(root, policy):
        relpath = path.relative_to(root).as_posix()
        text = path.read_text(encoding="utf-8", errors="replace")

        budget = policy["global"].get("max_file_lines")
        line_count = len(text.splitlines())
        if budget and line_count > budget:
            violations.append(
                {
                    "rule": "max-file-lines",
                    "file": relpath,
                    "line": 0,
                    "detail": "",
                    "message": f"file has {line_count} lines (budget {budget})",
                }
            )

        for line_number, include, quoted in parse_includes(text):
            if quoted:
                continue
            internal, external = resolve_external_target(policy, include)
            if internal == "<unregistered>" or external == "<unregistered>":
                violations.append(
                    {
                        "rule": "unregistered-include",
                        "file": relpath,
                        "line": line_number,
                        "detail": include,
                        "message": f"header <{include}> is neither a std "
                        "header nor declared in external_modules",
                    }
                )
                continue
            target = internal or external
            if target is None or target == owner:
                continue
            module_edges.setdefault(owner, set()).add(target)
            module = next(m for m in policy["modules"] if m["id"] == owner)
            if target not in module["requires"]:
                violations.append(
                    {
                        "rule": "module-dependency",
                        "file": relpath,
                        "line": line_number,
                        "detail": target,
                        "message": f"{owner} -> {target} is not declared in "
                        "`requires`",
                    }
                )

        for line_number, include, quoted in parse_includes(text):
            if not quoted:
                continue
            resolved = (path.parent / include).resolve()
            if not resolved.is_file():
                continue
            try:
                resolved_rel = resolved.relative_to(root).as_posix()
            except ValueError:
                continue
            target_owner = module_for_path(policy, resolved_rel)
            if target_owner in known_ids and target_owner != owner:
                violations.append(
                    {
                        "rule": "deep-import",
                        "file": relpath,
                        "line": line_number,
                        "detail": resolved_rel,
                        "message": f"quoted include reaches into module "
                        f"'{target_owner}' private sources",
                    }
                )

    violations.extend(find_cycles(policy, module_edges))
    return violations


def violation_key(entry: dict) -> str:
    key = f"{entry['rule']}:{entry['file']}"
    if entry["detail"]:
        key = f"{key}:{entry['detail']}"
    return key


def apply_exceptions(
    violations: list[dict], policy: dict
) -> tuple[list[dict], list[str]]:
    today = datetime.date.today()
    active: list[dict] = []
    expired: list[str] = []
    exceptions = policy.get("exceptions", [])
    for entry in violations:
        suppressed = False
        for exc in exceptions:
            if exc.get("rule") != entry["rule"]:
                continue
            pattern = exc.get("path", "")
            if pattern.endswith("/"):
                matched = entry["file"].startswith(pattern)
            else:
                matched = entry["file"] == pattern
            if not matched:
                continue
            suppressed = True
            expires = exc.get("expires")
            if expires:
                try:
                    expiry = datetime.date.fromisoformat(expires)
                except ValueError:
                    expired.append(f"{entry['file']}: invalid expiry {expires}")
                    continue
                if expiry < today:
                    expired.append(
                        f"{entry['file']}: exception for "
                        f"'{entry['rule']}' expired {expires}"
                    )
            break
        if not suppressed:
            active.append(entry)
    return active, expired


def update_baseline(path: Path, violations: list[dict]) -> None:
    keys = sorted({violation_key(entry) for entry in violations})
    path.write_text(
        json.dumps({"version": 1, "violations": keys}, indent=2, ensure_ascii=False)
        + "\n",
        encoding="utf-8",
    )


def main() -> int:
    args = list(sys.argv[1:])
    update = "--update-baseline" in args
    if update:
        args.remove("--update-baseline")
    policy_path = Path("tools/architecture-policy.json")
    if "--policy" in args:
        idx = args.index("--policy")
        policy_path = Path(args[idx + 1])
        del args[idx : idx + 2]
    root = Path(args[0]).resolve() if args else Path(__file__).resolve().parents[1]

    try:
        policy = load_policy(root / policy_path)
    except PolicyError as exc:
        print(f"architecture policy error: {exc}")
        return 2

    violations = check(root, policy)
    active, expired = apply_exceptions(violations, policy)

    baseline_path = root / "tools/architecture-baseline.json"
    baseline = load_baseline(baseline_path)
    keys = {violation_key(entry) for entry in active}
    new = [e for e in active if violation_key(e) not in baseline]
    stale = sorted(baseline - keys)

    if update:
        update_baseline(baseline_path, active)
        print(f"baseline updated: {len(active)} violations recorded")
        return 0

    print("Mira architecture policy check")
    print(f"  modules: {len(policy['modules'])}")
    print(f"  baseline violations (suppressed): {len(active) - len(new)}")
    print(f"  stale baseline entries (removable): {len(stale)}")
    for key in stale:
        print(f"    - {key}")
    if expired:
        print(f"  expired exceptions: {len(expired)}")
        for item in expired:
            print(f"    - {item}")
    if new:
        print(f"  NEW violations: {len(new)}")
        for entry in sorted(new, key=violation_key):
            location = (
                f"{entry['file']}:{entry['line']}"
                if entry["line"]
                else entry["file"]
            )
            print(f"    - [{entry['rule']}] {location}: {entry['message']}")
            hint = RULE_HINTS.get(entry["rule"])
            if hint:
                print(f"      fix: {hint}")
        print("new architecture policy violations block the change")
        return 1
    if expired:
        return 1
    print("architecture policy is clean")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
