import importlib.util
import subprocess
import tempfile
import unittest
from pathlib import Path
from types import ModuleType
from unittest import mock


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
HELPER = REPOSITORY_ROOT / "tools" / "run_clang_tidy.py"


def load_helper() -> ModuleType:
    spec = importlib.util.spec_from_file_location("run_clang_tidy", HELPER)
    if spec is None or spec.loader is None:
        raise RuntimeError("unable to load clang-tidy helper")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class RunClangTidyPresenceTest(unittest.TestCase):
    def test_helper_exists(self) -> None:
        self.assertTrue(HELPER.is_file(), "tools/run_clang_tidy.py is absent")


@unittest.skipUnless(HELPER.is_file(), "clang-tidy helper is not implemented")
class RunClangTidyTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name).resolve()
        self.module = load_helper()

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def write_source(self, relative_path: str) -> Path:
        path = self.root / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("int value;\n", encoding="utf-8")
        return path.resolve()

    def test_selects_tracked_cpp_files_present_in_compile_database(self) -> None:
        app = self.write_source("apps/daemon/main.cpp")
        core = self.write_source("src/core/value.cpp")
        test = self.write_source("tests/unit/value_test.cpp")
        self.write_source("third_party/vendor.cpp")
        database = [
            {"directory": str(self.root), "file": str(test)},
            {"directory": str(self.root), "file": "apps/daemon/main.cpp"},
            {"directory": str(self.root), "file": "src/core/value.cpp"},
            {"directory": str(self.root), "file": "build/generated.cpp"},
        ]

        selected = self.module.select_sources(
            self.root,
            [
                "tests/unit/value_test.cpp",
                "src/core/value.cpp",
                "apps/daemon/main.cpp",
                "include/value.hpp",
                "third_party/vendor.cpp",
                "build/generated.cpp",
            ],
            database,
        )

        self.assertEqual(selected, [app, core, test])

    def test_rejects_empty_source_selection(self) -> None:
        with self.assertRaisesRegex(self.module.QualificationError, "no tracked"):
            self.module.select_sources(self.root, [], [])

    def test_rejects_malformed_compile_database(self) -> None:
        source = self.write_source("src/value.cpp")
        malformed_values = [{}, {"directory": str(self.root)}, {"file": str(source)}]
        for entry in malformed_values:
            with self.subTest(entry=entry):
                with self.assertRaisesRegex(
                    self.module.QualificationError, "compile_commands"
                ):
                    self.module.select_sources(
                        self.root, ["src/value.cpp"], [entry]
                    )

    def test_requires_exact_frozen_source_count(self) -> None:
        sources = [self.root / f"source-{index}.cpp" for index in range(12)]
        self.module.require_source_count(sources)
        for invalid in (sources[:-1], [*sources, self.root / "extra.cpp"]):
            with self.subTest(count=len(invalid)):
                with self.assertRaisesRegex(
                    self.module.QualificationError, "expected 12.*found"
                ):
                    self.module.require_source_count(invalid)

    def test_requires_absolute_regular_application(self) -> None:
        application = self.root / "fake-clang-tidy-18"
        application.write_bytes(b"fake")
        self.assertEqual(
            self.module.require_application(application), application.resolve()
        )
        with self.assertRaisesRegex(self.module.QualificationError, "absolute"):
            self.module.require_application(Path("clang-tidy-18"))
        with self.assertRaisesRegex(self.module.QualificationError, "regular file"):
            self.module.require_application(self.root / "missing-clang-tidy-18")

    def test_accepts_exact_llvm_version_from_injected_application(self) -> None:
        application = self.root / "fake-clang-tidy-18"
        application.write_bytes(b"fake")
        runner = mock.Mock(
            return_value=subprocess.CompletedProcess(
                [str(application), "--version"],
                0,
                stdout="Ubuntu LLVM version 18.1.3\n",
                stderr="",
            )
        )

        self.module.require_version(application, runner)

        runner.assert_called_once_with(
            [str(application), "--version"],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_rejects_version_mismatch_and_tool_error(self) -> None:
        application = self.root / "fake-clang-tidy-18"
        application.write_bytes(b"fake")
        results = [
            subprocess.CompletedProcess([], 0, "LLVM version 18.1.4\n", ""),
            subprocess.CompletedProcess([], 0, "LLVM version 18.1.30\n", ""),
            subprocess.CompletedProcess([], 2, "", "tool failed"),
        ]
        patterns = [
            "required clang-tidy version",
            "required clang-tidy version",
            "version command failed",
        ]
        for result, pattern in zip(results, patterns, strict=True):
            with self.subTest(pattern=pattern):
                with self.assertRaisesRegex(self.module.QualificationError, pattern):
                    self.module.require_version(application, mock.Mock(return_value=result))

    def test_propagates_clang_tidy_diagnostic_failure(self) -> None:
        application = self.root / "fake-clang-tidy-18"
        application.write_bytes(b"fake")
        source = self.write_source("src/value.cpp")
        runner = mock.Mock(
            return_value=subprocess.CompletedProcess(
                [], 1, "warning: controlled diagnostic\n", "controlled stderr\n"
            )
        )

        with self.assertRaisesRegex(self.module.QualificationError, "exit 1"):
            self.module.run_analysis(
                application, self.root / "build", [source], runner
            )

        arguments = runner.call_args.args[0]
        self.assertEqual(arguments[0], str(application))
        self.assertIn("--warnings-as-errors=*", arguments)
        self.assertIn(str(source), arguments)

    def test_runs_analysis_with_injected_success(self) -> None:
        application = self.root / "fake-clang-tidy-18"
        application.write_bytes(b"fake")
        source = self.write_source("tests/unit/value_test.cpp")
        runner = mock.Mock(
            return_value=subprocess.CompletedProcess([], 0, "clean\n", "")
        )

        self.module.run_analysis(application, self.root / "build", [source], runner)

        runner.assert_called_once()


if __name__ == "__main__":
    unittest.main()
