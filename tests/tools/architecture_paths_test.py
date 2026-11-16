"""The checkout's parent directory must not suppress architecture checks."""

import importlib.util
from pathlib import Path
import tempfile
import unittest


spec = importlib.util.spec_from_file_location(
    "architecture", Path(__file__).resolve().parents[2] / "tools/check_architecture.py"
)
architecture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(architecture)


class ArchitecturePathsTest(unittest.TestCase):
    def test_parent_names_do_not_exclude_checkout(self):
        for parent in ("ordinary", "third_party", "build"):
            with self.subTest(parent=parent), tempfile.TemporaryDirectory() as directory:
                root = Path(directory) / parent / "mira"
                (root / "src/third_party").mkdir(parents=True)
                (root / "src/build").mkdir()
                source = root / "src/owned.cpp"
                source.write_text("// owned\n// exceeds the fixture budget\n", encoding="utf-8")
                (root / "src/third_party/vendor.cpp").write_text("// vendor\n", encoding="utf-8")
                (root / "src/build/generated.cpp").write_text("// generated\n", encoding="utf-8")
                policy = {
                    "modules": [{"id": "core", "roots": ["src"], "requires": []}],
                    "external_modules": [],
                    "global": {"max_file_lines": 1},
                }
                self.assertEqual(list(architecture.iter_source_files(root, policy)), [("core", source)])
                violations = architecture.check(root, policy)
                self.assertEqual(len(violations), 1)
                self.assertEqual(violations[0]["file"], "src/owned.cpp")
                self.assertEqual(violations[0]["rule"], "max-file-lines")


if __name__ == "__main__":
    unittest.main()
