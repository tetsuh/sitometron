import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path
from types import ModuleType
from unittest import mock

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
VALIDATOR = REPOSITORY_ROOT / "tools" / "check_relative_links.py"


def load_validator() -> ModuleType:
    spec = importlib.util.spec_from_file_location("check_relative_links", VALIDATOR)
    if spec is None or spec.loader is None:
        raise RuntimeError("unable to load the relative-link validator")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module

class RelativeLinksPresenceTest(unittest.TestCase):
    def test_validator_exists(self) -> None:
        self.assertTrue(VALIDATOR.is_file(), "tools/check_relative_links.py is absent")

@unittest.skipUnless(VALIDATOR.is_file(), "relative-link validator is not implemented")
class SlugTest(unittest.TestCase):
    def setUp(self) -> None:
        self.module = load_validator()

    def slugs(self, markdown: str) -> list[str]:
        return self.module.emitted_anchors(markdown)
    def test_accepts_atx_headings_at_every_permitted_indent_and_depth(self) -> None:
        markdown = "# One\n ## Two\n  ### Three\n   #### Four\n##### Five\n###### Six\n"
        self.assertEqual(self.slugs(markdown), ["one", "two", "three", "four", "five", "six"])
    def test_rejects_non_atx_and_malformed_headings(self) -> None:
        cases = {
            "four leading spaces": "    # Indented\n",
            "seven hashes": "####### Seven\n",
            "no separator": "#NoSpace\n",
            "setext equals": "Title\n=====\n",
            "setext dashes": "Title\n-----\n",
        }
        for label, markdown in cases.items():
            with self.subTest(case=label):
                self.assertEqual(self.slugs(markdown), [])
    def test_rejects_a_bare_hash_heading_as_an_empty_slug(self) -> None:
        with self.assertRaises(self.module.ValidationError):
            self.slugs("#\n")
    def test_removes_optional_closing_hash_sequence(self) -> None:
        self.assertEqual(self.slugs("## Closing ##\n"), ["closing"])
        self.assertEqual(self.slugs("## Sharp # Inside\n"), ["sharp--inside"])
    def test_uses_visible_label_of_links_and_images(self) -> None:
        markdown = ("## See [the guide](docs/guide.md) and ![icon](icon.png)\n"
                    "## [Guide][manual]\n## ![Icon][image]\n## [Guide again][]\n## ![Icon again][]\n")
        self.assertEqual(self.slugs(markdown), ["see-the-guide-and-icon", "guide", "icon", "guide-again", "icon-again"])
    def test_scans_escaped_and_nested_reference_labels(self) -> None:
        markdown = ("## [outer \\] inner][manual]\n"
                    "## ![outer \\] image][manual]\n"
                    "## [outer [inner]][manual]\n"
                    "## ![outer [image]][manual]\n"
                    "## [outer \\] collapsed][]\n"
                    "## ![outer [collapsed]][]\n")
        self.assertEqual(self.slugs(markdown), [
            "outer--inner", "outer--image", "outer-inner", "outer-image",
            "outer--collapsed", "outer-collapsed",
        ])
    def test_removes_html_tags_and_unescapes_entities(self) -> None:
        self.assertEqual(self.slugs("## <b>Bold</b> &amp; more\n"), ["bold--more"])
        self.assertEqual(self.slugs("## Caf&#233;\n"), ["café"])
    def test_retains_inline_code_text_without_delimiters(self) -> None:
        self.assertEqual(self.slugs("## Use `NFR-005` now\n"), ["use-nfr-005-now"])
        self.assertEqual(self.slugs("## *Emphasis* and _under_ and **strong**\n"),
                         ["emphasis-and-under-and-strong"])
    def test_retains_intraword_underscores_in_every_script(self) -> None:
        self.assertEqual(self.slugs("## café_漢\n"), ["café_漢"])
        self.assertEqual(self.slugs("## 漢_字\n"), ["漢_字"])
        self.assertEqual(self.slugs("## snake_case_name\n"), ["snake_case_name"])
        self.assertEqual(self.slugs("## _emphasis_ text\n"), ["emphasis-text"])
        self.assertEqual(self.slugs("## trail_ and _lead\n"), ["trail-and-lead"])
        self.assertEqual(self.slugs("## `NFR_005` café_漢\n"), ["nfr_005-café_漢"])

    def test_normalizes_unicode_and_case(self) -> None:
        composed = "## Cafe\u0301 MIXED\n"
        self.assertEqual(self.slugs(composed), ["café-mixed"])
        self.assertEqual(self.slugs("## Café MIXED\n"), self.slugs(composed))
        self.assertEqual(self.slugs("## Straße\n"), ["strasse"])
    def test_collapses_whitespace_runs_and_drops_other_punctuation(self) -> None:
        self.assertEqual(self.slugs("##   Spaced \t out  \n"), ["spaced-out"])
        self.assertEqual(self.slugs("## Punct: ,.;!?() kept-and_kept\n"), ["punct--kept-and_kept"])
    def test_appends_numeric_suffixes_to_colliding_slugs(self) -> None:
        markdown = "# Same\n# Same\n# Same\n# Other\n# Same\n"
        self.assertEqual(self.slugs(markdown), ["same", "same-1", "same-2", "other", "same-3"])
    def test_avoids_collisions_between_natural_and_generated_suffixes(self) -> None:
        self.assertEqual(self.slugs("# X\n# X-1\n# X\n"), ["x", "x-1", "x-2"])
    def test_rejects_headings_whose_slug_would_be_empty(self) -> None:
        with self.assertRaises(self.module.ValidationError):
            self.slugs("## ...\n")
        with self.assertRaises(self.module.ValidationError):
            self.slugs("## <b></b>\n")
    def test_ignores_fenced_code_contents(self) -> None:
        markdown = "# Real\n```text\n# Fake\n```\n~~~\n## Also fake\n~~~\n## Second real\n"
        self.assertEqual(self.slugs(markdown), ["real", "second-real"])
    def test_rejects_backticks_only_in_backtick_fence_info(self) -> None:
        self.assertEqual(self.slugs("``` language `option`\n# Visible\n"), ["visible"])
        self.assertEqual(self.slugs("~~~ language `option`\n# Hidden\n~~~\n"), [])
    def test_does_not_close_fences_with_non_ascii_trailing_whitespace(self) -> None:
        for marker in ("```", "~~~"):
            with self.subTest(marker=marker):
                markdown = f"{marker}\n# Hidden\n{marker}\u00a0\n## Still hidden\n{marker}\n# Visible\n"
                self.assertEqual(self.slugs(markdown), ["visible"])
    def test_handles_crlf_and_lf_identically(self) -> None:
        self.assertEqual(self.slugs("# One\r\n## Two\r\n"), self.slugs("# One\n## Two\n"))

@unittest.skipUnless(VALIDATOR.is_file(), "relative-link validator is not implemented")
class TargetTest(unittest.TestCase):
    def setUp(self) -> None:
        self.module = load_validator()
    def test_ignores_exactly_the_three_external_schemes_case_insensitively(self) -> None:
        for target in [
            "http://example.com/x", "HTTPS://example.com/x", "MailTo:person@example.com",
            "https://example.com/x?query=1#frag",
        ]:
            with self.subTest(target=target):
                self.assertTrue(self.module.is_external(target))
        for target in ["docs/guide.md", "#anchor", "./guide.md"]:
            with self.subTest(target=target):
                self.assertFalse(self.module.is_external(target))
    def test_rejects_every_other_scheme(self) -> None:
        for target in [
            "ftp://example.com/x", "file:///etc/passwd", "javascript:alert(1)",
            "data:text/plain;base64,AA==", "ssh://host/repo.git", "HTTPX://example.com",
        ]:
            with self.subTest(target=target):
                with self.assertRaises(self.module.ValidationError):
                    self.module.split_local_target(target)
    def test_splits_path_query_and_fragment_before_decoding(self) -> None:
        self.assertEqual(self.module.split_local_target("docs/a.md#sec"), ("docs/a.md", "sec"))
        self.assertEqual(self.module.split_local_target("docs/a.md"), ("docs/a.md", ""))
        self.assertEqual(self.module.split_local_target("#sec"), ("", "sec"))
        self.assertEqual(self.module.split_local_target("docs/a.md#a%2Db"), ("docs/a.md", "a-b"))
    def test_rejects_any_query_on_a_local_target(self) -> None:
        for target in ["docs/a.md?x=1", "docs/a.md?", "docs/a.md?x=1#sec", "?x=1"]:
            with self.subTest(target=target):
                with self.assertRaises(self.module.ValidationError):
                    self.module.split_local_target(target)
    def test_rejects_malformed_or_ambiguous_percent_encoding(self) -> None:
        for target in [
            "docs/%zz.md", "docs/%2.md", "docs/%.md", "docs/a%2Fb.md", "docs/a%2fb.md",
            "docs/a%5Cb.md", "docs/a%00b.md", "docs/a%01b.md", "docs/a%2525b.md",
            "docs/a.md#a%2500", "docs/%C3.md", "docs/a%C2%80b.md", "docs/a.md#sec%C2%85",
        ]:
            with self.subTest(target=target):
                with self.assertRaises(self.module.ValidationError):
                    self.module.split_local_target(target)
    def test_rejects_non_posix_relative_shapes(self) -> None:
        for target in [
            "/docs/a.md", "//server/share/a.md", "C:/docs/a.md", "c:\\docs\\a.md",
            "docs\\a.md", "..\\a.md", "docs//a.md", "docs/./a.md", "../../etc/passwd",
        ]:
            with self.subTest(target=target):
                with self.assertRaises(self.module.ValidationError):
                    self.module.resolve_local_path("docs/source.md", target)
    def test_resolves_relative_paths_against_the_containing_directory(self) -> None:
        self.assertEqual(self.module.resolve_local_path("docs/a.md", "b.md"), "docs/b.md")
        self.assertEqual(self.module.resolve_local_path("docs/a.md", "../README.md"), "README.md")
        self.assertEqual(self.module.resolve_local_path("docs/adr/a.md", "../b.md"), "docs/b.md")
        self.assertEqual(self.module.resolve_local_path("README.md", "docs/a.md"), "docs/a.md")
    def test_rejects_escaping_the_repository_root(self) -> None:
        for source, target in [("README.md", "../outside.md"), ("docs/a.md", "../../outside.md")]:
            with self.subTest(source=source, target=target):
                with self.assertRaises(self.module.ValidationError):
                    self.module.resolve_local_path(source, target)

@unittest.skipUnless(VALIDATOR.is_file(), "relative-link validator is not implemented")
class RepositoryCheckTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name).resolve()
        self.module = load_validator()
        self.files: dict[str, str] = {}

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def write(self, relative_path: str, text: str) -> None:
        path = self.root / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode("utf-8"))
        self.files[relative_path] = text

    def check(self) -> list:
        return self.module.check_repository(self.root, sorted(self.files))

    def test_accepts_valid_file_directory_and_fragment_links(self) -> None:
        self.write("docs/target.md", "# Target Section\n\n## Second Part\n")
        self.write("docs/keep.txt", "data\n")
        self.write("README.md", (
            "# Home\n\n"
            "- [file](docs/target.md)\n"
            "- [fragment](docs/target.md#second-part)\n"
            "- [self](#home)\n"
            "- [directory](docs/)\n"
            "- [plain file](docs/keep.txt)\n"
            "- [external](https://example.com/a?b=1#c)\n"
            "- [mail](mailto:person@example.com)\n"
        ))
        self.assertEqual(self.check(), [])

    def test_scans_nested_and_escaped_labels(self) -> None:
        self.write("README.md", "[outer [inner]](absent.md)\n[outer \\] label](also-absent.md)\n"
                   "[nested scheme [inner]](ftp://example.test/x)\n[escaped scheme \\] label](ftp://example.test/x)\n")
        findings = self.check()
        self.assertEqual([f.target for f in findings],
                         ["absent.md", "also-absent.md", "ftp://example.test/x", "ftp://example.test/x"])

    def test_reports_missing_files_directories_and_anchors(self) -> None:
        self.write("docs/target.md", "# Target\n")
        self.write("README.md", (
            "[missing file](docs/absent.md)\n"
            "[missing dir](absent/)\n"
            "[missing anchor](docs/target.md#absent)\n"
            "[missing self anchor](#absent)\n"
        ))
        findings = self.check()
        self.assertEqual(len(findings), 4, findings)
        self.assertTrue(all(f.file == "README.md" for f in findings))
        self.assertEqual([f.line for f in findings], [1, 2, 3, 4])

    def test_requires_exact_tracked_case(self) -> None:
        self.write("docs/Target.md", "# Target\n")
        self.write("README.md", "[wrong case](docs/target.md)\n[right case](docs/Target.md)\n")
        findings = self.check()
        self.assertEqual(len(findings), 1, findings)
        self.assertEqual(findings[0].line, 1)

    def test_directory_links_require_a_trailing_slash_and_no_fragment(self) -> None:
        self.write("docs/target.md", "# Target\n")
        self.write("README.md", "[no slash](docs)\n[fragment](docs/#target)\n[ok](docs/)\n")
        findings = self.check()
        self.assertEqual(len(findings), 2, findings)
        self.assertEqual([f.line for f in findings], [1, 2])

    def test_performs_no_implicit_readme_or_index_resolution(self) -> None:
        self.write("docs/README.md", "# Docs\n")
        self.write("README.md", "[implicit](docs/README)\n")
        self.assertEqual(len(self.check()), 1)

    def test_reports_unknown_schemes_queries_and_traversal(self) -> None:
        self.write("README.md", (
            "[scheme](ftp://example.com/x)\n"
            "[query](README.md?x=1)\n"
            "[encoded slash](docs%2Fa.md)\n"
            "[backslash](docs\\a.md)\n"
            "[absolute](/etc/passwd)\n"
            "[escape](../outside.md)\n"
        ))
        findings = self.check()
        self.assertEqual(len(findings), 6, findings)

    def test_ignores_links_inside_fenced_code(self) -> None:
        self.write("README.md", "```text\n[broken](absent.md)\n```\n\n[real](README.md)\n")
        self.assertEqual(self.check(), [])

    def test_requires_reference_definitions_and_validates_their_targets(self) -> None:
        self.write("README.md", "[missing][ref]\n\n[ref]: absent.md\n")
        findings = self.check()
        self.assertEqual(len(findings), 2, findings)
        self.write("README.md", "[valid][ref]\n\n[ref]: README.md\n")
        self.assertEqual(self.check(), [])

    def test_accepts_angle_bracket_reference_destinations(self) -> None:
        self.write("docs/target file.md", "# Target\n")
        self.write("README.md", "[valid][ref]\n\n[ref]: <docs/target file.md>\n")
        self.assertEqual(self.check(), [])

    def test_rejects_malformed_angle_bracket_reference_destinations(self) -> None:
        for destination in ("<docs/absent file.md>", "<docs/absent.md"):
            with self.subTest(destination=destination):
                self.write("README.md", f"[missing][ref]\n\n[ref]: {destination}\n")
                findings = self.check()
                self.assertEqual(len(findings), 2, findings)
                self.assertEqual([finding.line for finding in findings], [1, 3])

    def test_rejects_repeated_trailing_directory_markers(self) -> None:
        self.write("docs/keep.txt", "data\n")
        self.write("README.md", "[double](docs//)\n[triple](docs///)\n")
        findings = self.check()
        self.assertEqual(len(findings), 2, findings)

    def test_requires_exact_fragment_case_and_normalization(self) -> None:
        self.write("docs/target.md", "# Summary\n# Café\n")
        self.write("README.md", "[case](docs/target.md#SUMMARY)\n[unicode](docs/target.md#cafe\u0301)\n")
        findings = self.check()
        self.assertEqual(len(findings), 2, findings)

    def test_ignores_link_syntax_inside_inline_code_spans(self) -> None:
        self.write("README.md", (
            "Write `[a](absent.md)` in a doc.\n"
            "Double ``[b](absent.md)`` too.\n"
            "[real](README.md)\n"
        ))
        self.assertEqual(self.check(), [])

    def test_does_not_pair_mismatched_long_backtick_runs(self) -> None:
        self.write("README.md", (
            ("`" * 4096) + " [broken](absent.md) " + ("`" * 4095) + "\n"
        ))
        findings = self.check()
        self.assertEqual(len(findings), 1, findings)
        self.assertEqual(findings[0].target, "absent.md")

    def test_reads_angle_bracket_and_parenthesised_destinations(self) -> None:
        self.write("docs/a(1).md", "# Target\n")
        self.write("docs/plain.md", "# Plain\n")
        self.write("README.md", (
            "[angle](<docs/plain.md>)\n"
            "[parens](docs/a(1).md)\n"
            "[title](docs/plain.md \"a title\")\n"
        ))
        self.assertEqual(self.check(), [])
        self.write("README.md", "[angle](<docs/absent.md>)\n[parens](docs/b(2).md)\n")
        findings = self.check()
        self.assertEqual([f.target for f in findings], ["docs/absent.md", "docs/b(2).md"])

    def test_validates_each_reference_definition_at_its_own_line(self) -> None:
        self.write("docs/target.md", "# Target\n")
        self.write("README.md", (
            "[a]: docs/absent.md\n"
            "[a]: docs/target.md\n"
            "\n"
            "use [x][a]\n"
        ))
        findings = self.check()
        self.assertEqual([f.line for f in findings], [1, 4],
                         "the first definition binds the label and every definition is checked")
        self.assertTrue(all("docs/absent.md" == f.target for f in findings), findings)

    def test_resolves_every_reference_form_including_images_and_nesting(self) -> None:
        self.write("docs/target.md", "# Target\n")
        self.write("icon.png", "")
        self.write("README.md", (
            "[full]: docs/target.md\n"
            "[collapsed]: docs/target.md\n"
            "[shortcut]: docs/target.md\n"
            "[img]: icon.png\n"
            "\n"
            "full [text][full]\n"
            "collapsed [collapsed][]\n"
            "shortcut [shortcut]\n"
            "image ![alt][img]\n"
            "nested [outer [inner] label][full]\n"
            "escaped [not a label\\] still text][full]\n"
        ))
        self.assertEqual(self.check(), [])

    def test_reports_reference_uses_without_a_definition(self) -> None:
        self.write("README.md", (
            "full [text][absent]\n"
            "collapsed [absent][]\n"
            "image ![alt][absent]\n"
            "shortcut [absent]\n"
        ))
        findings = self.check()
        self.assertEqual([f.line for f in findings], [1, 2, 3],
                         "an undefined shortcut is literal text in CommonMark, not a link")

    def test_reference_labels_are_normalized_but_not_confused(self) -> None:
        self.write("docs/target.md", "# Target\n")
        self.write("README.md", (
            "[Mixed   Case]: docs/target.md\n"
            "\n"
            "[text][mixed case]\n"
            "[text][mixed  case]\n"
            "[text][mixedcase]\n"
        ))
        findings = self.check()
        self.assertEqual([f.line for f in findings], [5])

    def test_validates_links_whose_label_spans_lines(self) -> None:
        self.write("docs/target.md", "# Target\n")
        self.write("README.md", (
            "see [ok\nlabel](docs/target.md) here\n"
            "and [broken\nlabel](docs/absent.md) there\n"
        ))
        findings = self.check()
        self.assertEqual([(f.line, f.target) for f in findings], [(3, "docs/absent.md")],
                         "a multiline link is reported at the line its label opens on")

    def test_validates_a_link_however_many_lines_its_paragraph_spans(self) -> None:
        for lines in [2, 5, 6, 12]:
            with self.subTest(lines=lines):
                label = "\n".join(f"word{index}" for index in range(lines))
                self.write("README.md", f"see [{label}](docs/absent.md) here\n")
                self.assertEqual([f.target for f in self.check()], ["docs/absent.md"],
                                 "no line count may silently skip a link")

    def test_treats_a_blank_line_as_ending_a_candidate_link(self) -> None:
        self.write("README.md", "see [a\n\nb](docs/absent.md) here\n")
        self.assertEqual(self.check(), [], "a label cannot contain a blank line")
        self.write("README.md", "[a](\n\ndocs/absent.md)\n")
        self.assertEqual(self.check(), [], "a destination cannot contain a blank line")

    def test_honours_escapes_in_reference_definition_labels(self) -> None:
        self.write("docs/target.md", "# Target\n")
        self.write("README.md", (
            "[a\\]b]: docs/absent.md\n"
            "\n"
            "use [a\\]b]\n"
        ))
        findings = self.check()
        self.assertEqual([(f.line, f.target) for f in findings],
                         [(1, "docs/absent.md"), (3, "docs/absent.md")],
                         "an escaped bracket must not hide the definition or its use")

    def test_treats_whitespace_only_lines_as_blank(self) -> None:
        for separator in ["   ", "\t", " \t "]:
            with self.subTest(separator=repr(separator)):
                self.write("README.md", f"see [a\n{separator}\nb](docs/absent.md) here\n")
                self.assertEqual(self.check(), [],
                                 "a whitespace-only line ends the paragraph, so this is not a link")
        self.write("docs/target.md", "# Target\n")
        self.write("README.md", "see [a\nb](docs/target.md) here\n")
        self.assertEqual(self.check(), [])

    def test_blanks_code_spans_that_close_on_a_later_line(self) -> None:
        self.write("README.md", (
            "Write `[a](docs/absent.md)\n"
            "still code` here\n"
            "and ``[b](docs/absent.md)\n"
            "more`` there\n"
        ))
        self.assertEqual(self.check(), [])
        self.write("README.md", "real [x](docs/absent.md)\n")
        self.assertEqual(len(self.check()), 1)

    def test_reports_destinations_nested_beyond_the_supported_depth(self) -> None:
        self.write("docs/a((((((())))))).md", "# Deep\n")
        self.write("README.md", "[ok](docs/a((((((())))))).md)\n")
        self.assertEqual(self.check(), [], "a destination inside the depth bound is validated")
        for depth in [8, 9, 20]:
            with self.subTest(depth=depth):
                destination = "docs/a" + "(" * depth + ")" * depth + ".md"
                self.write("README.md", f"[deep]({destination})\n")
                findings = self.check()
                self.assertEqual(len(findings), 1, findings)
                self.assertIn("nests deeper", findings[0].reason,
                              "an unsupported depth fails closed instead of being skipped")

    def test_reads_angle_destinations_containing_parentheses(self) -> None:
        self.write("docs/a(1).md", "# Deep\n")
        self.write("docs/b)2.md", "# Other\n")
        self.write("README.md", (
            "[paren](<docs/a(1).md>)\n"
            "[close](<docs/b)2.md>)\n"
        ))
        self.assertEqual(self.check(), [],
                         "parentheses inside an angle destination are literal")
        self.write("README.md", "[missing](<docs/absent(1.md>)\n")
        findings = self.check()
        self.assertEqual([f.target for f in findings], ["docs/absent(1.md"],
                         "a missing angle destination must not disappear from extraction")

    def test_reports_labels_nested_beyond_the_supported_depth(self) -> None:
        self.write("docs/target.md", "# Target\n")
        opening, closing = "[" * 9, "]" * 9
        forms = {
            "inline": f"{opening}x{closing}(docs/absent.md)\n",
            "image": f"!{opening}x{closing}(docs/absent.md)\n",
            "definition": f"{opening}label{closing}: docs/absent.md\n",
            "reference use": f"[r]: docs/target.md\n\nuse [t]{opening}x{closing}\n",
        }
        for label, text in forms.items():
            with self.subTest(form=label):
                self.write("README.md", text)
                findings = [f for f in self.check() if "nests deeper" in f.reason]
                self.assertEqual(len(findings), 1, f"{label}: {self.check()}")
        self.write("README.md", "[" * 8 + "x" + "]" * 8 + "(docs/target.md)\n")
        self.assertEqual(self.check(), [], "a label inside the bound is validated normally")

    def test_reports_unreadable_markdown_as_a_validation_error(self) -> None:
        (self.root / "bad.md").write_bytes(b"# Title\n\xff\n")
        self.files["bad.md"] = ""
        with self.assertRaises(self.module.ValidationError):
            self.check()

    def test_matches_only_compatible_fence_character_and_width(self) -> None:
        self.write("README.md", "````text\n[hidden](absent.md)\n```\n[still-hidden](absent.md)\n````\n")
        self.assertEqual(self.check(), [])

    def test_matches_anchors_after_normalization(self) -> None:
        self.write("docs/target.md", "# Cafe\u0301 Section\n# Cafe\u0301 Section\n")
        self.write("README.md", (
            "[nfc](docs/target.md#caf%C3%A9-section)\n"
            "[collision](docs/target.md#café-section-1)\n"
        ))
        self.assertEqual(self.check(), [])

    def test_rejects_html_and_setext_anchor_definitions(self) -> None:
        self.write("docs/target.md", '<a id="html-anchor"></a>\n\nSetext\n======\n\n# Real\n')
        self.write("README.md", (
            "[html](docs/target.md#html-anchor)\n"
            "[setext](docs/target.md#setext)\n"
            "[real](docs/target.md#real)\n"
        ))
        findings = self.check()
        self.assertEqual(len(findings), 2, findings)
        self.assertEqual([f.line for f in findings], [1, 2])

    def test_validates_headings_in_every_tracked_markdown_file(self) -> None:
        self.write("README.md", "# Home\n")
        for heading in ("## ...\n", "##\n"):
            self.write("docs/empty.md", heading)
            with self.assertRaises(self.module.ValidationError):
                self.check()

    def test_reports_findings_for_every_tracked_markdown_file(self) -> None:
        self.write("a.md", "[x](absent.md)\n")
        self.write("docs/b.md", "[y](absent.md)\n")
        self.write("c.txt", "[z](absent.md)\n")
        findings = self.check()
        self.assertEqual(sorted(f.file for f in findings), ["a.md", "docs/b.md"])

    def test_main_returns_zero_for_a_clean_repository_and_one_otherwise(self) -> None:
        self.write("README.md", "# Home\n\n[self](#home)\n")
        with mock.patch.object(self.module, "tracked_files", lambda root: sorted(self.files)):
            self.assertEqual(self.module.main([], io.StringIO(), self.root), 0)
            self.write("README.md", "# Home\n\n[broken](absent.md)\n")
            self.assertEqual(self.module.main([], io.StringIO(), self.root), 1)

    def test_main_fails_closed_when_git_enumeration_fails(self) -> None:
        def failing(root: object) -> list[str]:
            raise self.module.ValidationError("git ls-files failed")

        with mock.patch.object(self.module, "tracked_files", failing):
            self.assertEqual(self.module.main([], io.StringIO(), self.root), 1)

    def test_main_rejects_unknown_arguments(self) -> None:
        for arguments in [["--repository", str(self.root)], ["--unknown"], ["extra"], ["-h"]]:
            with self.subTest(arguments=arguments):
                with self.assertRaises(SystemExit):
                    self.module.main(arguments, io.StringIO(), self.root)

    def test_repository_passes_its_own_validation(self) -> None:
        tracked = self.module.tracked_files(REPOSITORY_ROOT)
        findings = self.module.check_repository(REPOSITORY_ROOT, tracked)
        self.assertEqual(findings, [], f"repository link findings: {findings}")
        markdown = [p for p in tracked if p.lower().endswith(".md")]
        self.assertGreaterEqual(len(markdown), 26)


if __name__ == "__main__":
    unittest.main()
