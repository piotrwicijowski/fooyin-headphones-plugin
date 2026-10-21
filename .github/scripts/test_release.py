"""Check version validation and the GitHub CLI publishing contract."""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from release import publish, validate_version


class VersionValidationTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.source = Path(self.temporary.name)
        (self.source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.19)\n"
            "project(FooyinHeadphones VERSION 0.1.0 LANGUAGES CXX)\n"
        )

    def test_stable_prerelease_and_build_metadata(self):
        for tag in ("v0.1.0", "v0.1.0-rc.1", "v0.1.0-0", "v0.1.0-alpha-beta",
                    "v0.1.0+build.01", "v0.1.0-rc.1+build.123"):
            with self.subTest(tag=tag):
                self.assertEqual(validate_version(self.source, f"refs/tags/{tag}"), (tag, "0.1.0"))

    def test_rejects_ambiguous_or_malformed_versions(self):
        for tag in ("0.1.0", "v0.1", "v00.1.0", "v0.01.0", "v0.1.00", "v0.1.0-01",
                    "v0.1.0-rc.01", "v0.1.0-", "v0.1.0+", "v0.1.0+build..1",
                    "v0.1.0/extra", "v0.1.0\n", "v0.1.0;touch-file"):
            with self.subTest(tag=tag), self.assertRaisesRegex(ValueError, "Invalid version tag"):
                validate_version(self.source, f"refs/tags/{tag}")

    def test_manual_dispatch_requires_a_tag(self):
        for ref in ("refs/heads/main", "refs/heads/v0.1.0", "v0.1.0"):
            with self.subTest(ref=ref), self.assertRaisesRegex(ValueError, "existing version tag"):
                validate_version(self.source, ref)

    def test_version_mismatch(self):
        with self.assertRaisesRegex(ValueError, "CMakeLists.txt declares 0.1.0"):
            validate_version(self.source, "refs/tags/v0.2.0-rc.1")

    def test_cmake_comments_and_multiline_project(self):
        (self.source / "CMakeLists.txt").write_text(
            "# project(FooyinHeadphones VERSION 9.9.9)\n"
            "project(\n  FooyinHeadphones\n  VERSION 0.1.0\n  LANGUAGES CXX\n)\n"
        )
        self.assertEqual(validate_version(self.source, "refs/tags/v0.1.0"), ("v0.1.0", "0.1.0"))

    def test_missing_project_version(self):
        (self.source / "CMakeLists.txt").write_text("project(FooyinHeadphones LANGUAGES CXX)\n")
        with self.assertRaisesRegex(ValueError, "Cannot read"):
            validate_version(self.source, "refs/tags/v0.1.0")


class ReleasePublishingTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        (self.directory / "CMakeLists.txt").write_text("project(FooyinHeadphones VERSION 0.1.0)\n")
        self.archive = self.directory / "headphones-test.tar.gz"
        self.archive.write_bytes(b"test archive")
        Path(str(self.archive) + ".sha256").write_text("test checksum")
        self.args = argparse.Namespace(
            source=self.directory, output=self.directory, repo="example/headphones", tag="v0.1.0"
        )
        self.write_metadata()
        self.gh = self.enterContext(patch("release.subprocess.run"))
        self.gh.return_value = subprocess.CompletedProcess([], 1)
        self.url = self.enterContext(patch("release.run", return_value="https://example.com/release"))
        self.outputs = self.enterContext(patch("release.outputs"))

    def write_metadata(self):
        (self.directory / "build-metadata.json").write_text(
            json.dumps({"tag": self.args.tag, "artifact": "headphones-test"})
        )

    def test_creates_release_with_both_assets_and_existing_tag(self):
        publish(self.args)
        command = self.gh.call_args.args[0]
        self.assertEqual(command[:4], ["gh", "release", "create", "v0.1.0"])
        self.assertIn(str(self.archive), command)
        self.assertIn(str(self.archive) + ".sha256", command)
        self.assertIn("--verify-tag", command)
        self.assertIn("--generate-notes", command)
        self.assertEqual(command[command.index("--repo") + 1], "example/headphones")
        self.assertNotIn("--prerelease", command)
        self.outputs.assert_called_once_with(url="https://example.com/release")

    def test_classifies_prerelease_without_confusing_build_metadata(self):
        for tag, prerelease in (("v0.1.0-rc.1+build.2", True), ("v0.1.0+build-with-hyphens", False)):
            with self.subTest(tag=tag):
                self.args.tag = tag
                self.write_metadata()
                publish(self.args)
                self.assertEqual("--prerelease" in self.gh.call_args.args[0], prerelease)

    def test_rerun_updates_existing_release_assets(self):
        self.gh.return_value = subprocess.CompletedProcess([], 0)
        publish(self.args)
        command = self.gh.call_args.args[0]
        self.assertEqual(command[:4], ["gh", "release", "upload", "v0.1.0"])
        self.assertIn("--clobber", command)

    def test_missing_assets_or_wrong_tag_fail_before_publishing(self):
        self.args.tag = "v0.1.0-rc.1"
        with self.assertRaisesRegex(ValueError, "does not match"):
            publish(self.args)
        self.args.tag = "v0.1.0"
        self.archive.unlink()
        with self.assertRaisesRegex(ValueError, "missing or empty"):
            publish(self.args)
        self.gh.assert_not_called()

    def test_failed_publish_does_not_report_a_release_url(self):
        self.gh.side_effect = [
            subprocess.CompletedProcess([], 1),
            subprocess.CalledProcessError(1, ["gh", "release", "create"]),
        ]
        with self.assertRaises(subprocess.CalledProcessError):
            publish(self.args)
        self.url.assert_not_called()
        self.outputs.assert_not_called()


if __name__ == "__main__":
    unittest.main()
