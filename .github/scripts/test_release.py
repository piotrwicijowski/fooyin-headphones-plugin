"""Check version validation, distribution packaging, and GitHub CLI publishing."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from release import distribution_target, package, publish, validate_version


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


class DistributionPackagingTest(unittest.TestCase):
    distributions = (
        ({"ID": "ubuntu", "VERSION_ID": "24.04", "PRETTY_NAME": "Ubuntu 24.04 LTS"}, "ubuntu24.04"),
        ({"ID": "ubuntu", "VERSION_ID": "26.04", "PRETTY_NAME": "Ubuntu 26.04 LTS"}, "ubuntu26.04"),
        # Rolling Arch does not define VERSION_ID in /etc/os-release.
        ({"ID": "arch", "PRETTY_NAME": "Arch Linux"}, "archlinux"),
    )

    def test_rejects_unsupported_distribution_or_architecture(self):
        for os_release, architecture in (
            ({"ID": "ubuntu", "VERSION_ID": "22.04"}, "x86_64"),
            ({"ID": "ubuntu"}, "x86_64"),
            ({"ID": "debian", "VERSION_ID": "13"}, "x86_64"),
            ({"ID": "arch"}, "aarch64"),
        ):
            with self.subTest(os_release=os_release, architecture=architecture):
                with self.assertRaisesRegex(ValueError, "Package targets require"):
                    distribution_target(os_release, architecture)

    def test_packages_distribution_specific_archives_and_install_notes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "CMakeLists.txt").write_text("project(FooyinHeadphones VERSION 0.1.0)\n")
            (root / "LICENSE").write_text("test license\n")
            build = root / "build"
            build.mkdir()
            args = argparse.Namespace(
                source=root, build=build, fooyin_source=root, fooyin_build=build,
                tag="v0.1.0", output=root / "dist",
            )

            def fake_run(*command, **kwargs):
                if command[0] == "git":
                    return "4cb697bd19cfde409afca420b84d15bccc1ffb1e"
                if command[0] == "ldd":
                    return "libswresample.so.6 => /usr/lib/libswresample.so.6"
                if command[0] == "readelf":
                    return "(RUNPATH) Library runpath: [$ORIGIN/..]"
                if command[0] == str(build / "tests/pluginbundlecheck"):
                    archive, destination = Path(command[1]), Path(command[2])
                    self.assertEqual(archive.suffix, ".zip")
                    plugin = destination / "piotrwicijowski.headphonespreview/fyplugin_headphones.so"
                    plugin.parent.mkdir(parents=True)
                    with zipfile.ZipFile(archive) as zipped:
                        plugin.write_bytes(zipped.read("linux-x64/fyplugin_headphones.so"))
                    return str(plugin)
                return "test version"

            def fake_subprocess(command, **kwargs):
                if command[0] == "cmake":
                    stage = Path(command[command.index("--prefix") + 1])
                    plugin = stage / "lib/fooyin/plugins/fyplugin_headphones.so"
                    plugin.parent.mkdir(parents=True)
                    plugin.write_bytes(b"test plugin")
                else:
                    self.assertEqual(command[0], str(build / "tests/pluginlifecycletest"))
                    self.assertEqual(Path(command[1]).read_bytes(), b"test plugin")
                    self.assertEqual(kwargs["env"]["QT_QPA_PLATFORM"], "offscreen")

            cache = {
                "CMAKE_PROJECT_VERSION": "0.1.0", "CMAKE_BUILD_TYPE": "Release",
                "CMAKE_CXX_COMPILER": "/usr/bin/g++",
            }
            with patch("release.cache_value", side_effect=lambda build, name: cache[name]), \
                    patch("release.run", side_effect=fake_run), \
                    patch("release.subprocess.run", side_effect=fake_subprocess) as commands, \
                    patch("release.platform.machine", return_value="x86_64"), \
                    patch("release.outputs"):
                for os_release, target in self.distributions:
                    with self.subTest(target=target), \
                            patch("release.platform.freedesktop_os_release", return_value=os_release):
                        package(args)
                        metadata = json.loads((args.output / "build-metadata.json").read_text())
                        self.assertEqual(metadata["distribution"], target)
                        self.assertEqual(metadata["os"], os_release["PRETTY_NAME"])
                        self.assertIn(f"-linux-x86_64-{target}-fooyin-", metadata["artifact"])
                        self.assertIn("libpipewire-0.3", metadata["libraries"])
                        self.assertTrue(metadata["built_at"])
                        archive = args.output / f"{metadata['artifact']}.zip"
                        checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
                        self.assertEqual(
                            Path(str(archive) + ".sha256").read_text(), f"{checksum}  {archive.name}\n"
                        )
                        with zipfile.ZipFile(archive) as zipped:
                            self.assertEqual(set(zipped.namelist()), {
                                "linux-x64/fyplugin_headphones.so", "LICENSE", "INSTALL.txt",
                                "build-metadata.json", "dependencies.txt", "elf-dynamic.txt",
                            })
                            self.assertEqual(zipped.testzip(), None)
                            self.assertEqual(zipped.read("linux-x64/fyplugin_headphones.so"), b"test plugin")
                            self.assertEqual(json.loads(zipped.read("build-metadata.json")), metadata)
                            notes = zipped.read("INSTALL.txt").decode()
                        self.assertIn(f"Built for {os_release['PRETTY_NAME']} x86-64", notes)
                        self.assertIn("Settings > Plugins > Install", notes)
                        self.assertIn(f"{metadata['artifact']}.zip.sha256", notes)
                # Each target is installed and lifecycle-checked after extraction.
                self.assertEqual(commands.call_count, 6)


class ReleasePublishingTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        (self.directory / "CMakeLists.txt").write_text("project(FooyinHeadphones VERSION 0.1.0)\n")
        self.archive = self.directory / "headphones-test.zip"
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

    def test_publishes_all_distribution_assets_in_one_release(self):
        self.args.output = self.directory / "dist"
        expected = []
        for target in ("ubuntu24.04", "ubuntu26.04", "archlinux"):
            directory = self.args.output / f"release-linux-{target}"
            directory.mkdir(parents=True)
            artifact = f"headphones-{target}"
            (directory / "build-metadata.json").write_text(
                json.dumps({"tag": self.args.tag, "artifact": artifact})
            )
            for suffix in (".zip", ".zip.sha256"):
                asset = directory / f"{artifact}{suffix}"
                asset.write_bytes(b"test asset")
                expected.append(str(asset))
        publish(self.args)
        self.assertEqual(self.gh.call_count, 2)  # One lookup, one create.
        command = self.gh.call_args.args[0]
        self.assertEqual(command[:4], ["gh", "release", "create", "v0.1.0"])
        for asset in expected:
            self.assertEqual(command.count(asset), 1)

    def test_checks_every_build_before_publishing(self):
        directory = self.directory / "second-build"
        directory.mkdir()
        manifest = directory / "build-metadata.json"
        manifest.write_text(json.dumps({"tag": "v0.1.0", "artifact": "missing"}))
        with self.assertRaisesRegex(ValueError, "missing or empty"):
            publish(self.args)
        manifest.write_text(json.dumps({"tag": "v0.2.0", "artifact": "missing"}))
        with self.assertRaisesRegex(ValueError, "does not match"):
            publish(self.args)
        manifest.write_text(json.dumps({"tag": "v0.1.0", "artifact": "headphones-test"}))
        (directory / "headphones-test.zip").write_bytes(b"duplicate")
        with self.assertRaisesRegex(ValueError, "Duplicate release asset"):
            publish(self.args)
        self.gh.assert_not_called()

    def test_rejects_empty_output_before_publishing(self):
        self.args.output = self.directory / "empty"
        with self.assertRaisesRegex(ValueError, "No packaged builds"):
            publish(self.args)
        self.gh.assert_not_called()

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
